/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Phenix-1255 contributors
 *
 * Sample counter derivation
 * -------------------------
 * The RX eDMA channel runs one TCD forever over a ring buffer (no auto-stop,
 * DLAST wraps the destination). Half and major loop interrupts increment
 * rx_halves. The current position is
 *
 *   counter = halves * half_size + (BITER - CITER) * samples_per_minor_loop
 *
 * where halves is corrected by one if the hardware already crossed a half
 * boundary whose interrupt has not been serviced yet (detected by comparing
 * the parity of halves with the half the DMA is currently in).
 *
 * TX runs synchronous to RX (SAI TCR2.SYNC = 1). TE is set before RE, so both
 * start on the same frame sync and TX ring element n is emitted in the same
 * I2S frame as RX sample n is received.
 *
 * TX buffer
 * ---------
 * Host TX samples are stored in tx_buf at index (counter mod size). The TX
 * DMA interrupt of half k copies the block [(k + 1) * B, (k + 2) * B) into
 * the DMA ring and clears it in tx_buf, so after it everything below
 * (k + 2) * B is committed. A write is accepted only if it starts at or
 * after that boundary and ends within one buffer length of it. The check
 * and the copy run with interrupts locked, so a block is never refilled
 * while a packet is being written into it.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <fsl_clock.h>
#include <fsl_edma.h>
#include <fsl_sai.h>
#include <fsl_reset.h>

#include "board/board_io.h"
#include "radio/sx1255.h"
#include "stream/stream.h"

LOG_MODULE_REGISTER(stream, LOG_LEVEL_INF);

#if defined(CONFIG_DMA_MCUX_EDMA) || defined(CONFIG_DMA_MCUX_EDMA_V3) ||                          \
	defined(CONFIG_DMA_MCUX_EDMA_V4)
#error "eDMA0 is driven directly by the stream module, disable the Zephyr eDMA driver"
#endif
#if defined(CONFIG_I2S_MCUX_SAI)
#error "SAI1 is driven directly by the stream module, disable the Zephyr SAI driver"
#endif

#define SAI_NODE  DT_NODELABEL(sai1)
#define EDMA_NODE DT_NODELABEL(edma0)

/* eDMA channel assignment (plain literals, also used as devicetree indices) */
#define RX_DMA_CH 0
#define TX_DMA_CH 1

#define RX_DMA_REQ DT_DMAS_CELL_BY_NAME(SAI_NODE, rx, source)
#define TX_DMA_REQ DT_DMAS_CELL_BY_NAME(SAI_NODE, tx, source)

#define DMA_IRQ_PRIO DT_IRQ_BY_IDX(EDMA_NODE, 0, priority)
#define SAI_IRQ_PRIO 2

/* SAI data channel 0: RXD0 / TXD0 */
#define SAI_DATA_CH 0U

#define WORD_BYTES          4U
#define WORDS_PER_SAMPLE    2U
#define FIFO_WATERMARK      4U /* words per DMA request, FIFO depth 8 */
#define SAMPLES_PER_REQUEST (FIFO_WATERMARK / WORDS_PER_SAMPLE)
#define REQUEST_BYTES       (FIFO_WATERMARK * WORD_BYTES)

#define RX_HALF_SAMPLES (STREAM_RX_RING_SAMPLES / 2U)

/*
 * TX refill granularity (half of the TX ring). Chosen per sample rate so
 * that a refill covers about 0.4 ms; this bounds the minimum lead time of
 * timed TX data.
 */
#define TX_HALF_SAMPLES_MAX 256U
#define TX_HALF_SAMPLES_MIN 16U
#define TX_HALF_TARGET_US   400U

/* Time to wait for the first samples after enabling the SAI */
#define START_TIMEOUT_MS 20

/* TX tone generator: 1024-entry cosine table, 32-bit phase accumulator */
#define TONE_TABLE_BITS 10U
#define TONE_TABLE_SIZE BIT(TONE_TABLE_BITS)
#define TWO_PI          6.28318530717958647692f

BUILD_ASSERT(IS_POWER_OF_TWO(STREAM_RX_RING_SAMPLES), "RX ring size");
BUILD_ASSERT(IS_POWER_OF_TWO(STREAM_TX_BUFFER_SAMPLES), "TX buffer size");

PINCTRL_DT_DEFINE(SAI_NODE);

static I2S_Type *const sai = (I2S_Type *)DT_REG_ADDR(SAI_NODE);
static EDMA_Type *const edma = (EDMA_Type *)DT_REG_ADDR(EDMA_NODE);

static struct stream_iq rx_ring[STREAM_RX_RING_SAMPLES] __aligned(32);
static struct stream_iq tx_ring[2U * TX_HALF_SAMPLES_MAX] __aligned(32);
static struct phx_cs16 tx_buf[STREAM_TX_BUFFER_SAMPLES] __aligned(32);

static uint32_t tx_half_samples;
static volatile uint64_t rx_halves;
static volatile uint64_t tx_halves;
static uint32_t rate;
static volatile bool running;
static volatile bool tx_enabled;
static bool initialized;

static atomic_t rx_fifo_errors;
static atomic_t tx_fifo_errors;
static atomic_t rx_sync_errors;
static atomic_t tx_sync_errors;
static atomic_t tx_refill_late;
static atomic_t tx_late;
static atomic_t tx_early;

static int32_t tone_table[TONE_TABLE_SIZE];
static volatile uint32_t tone_step;
static volatile int32_t tone_amplitude; /* Q15 scale factor, 0 = off */
static uint32_t tone_phase;

static stream_fault_handler_t fault_handler;
static struct k_work fault_work;

static K_MUTEX_DEFINE(lock);

/*
 * Remaining minor loops of the current major loop. EDMA_GetRemainingMajorLoopCount()
 * is not used: it returns 0 while the DONE flag is set, which in circular
 * operation stays set after a major loop until the next minor loop starts,
 * although CITER has already been reloaded.
 */
static uint32_t dma_citer(uint32_t ch)
{
	return ((uint32_t)EDMA_TCD_CITER(EDMA_TCD_BASE(edma, ch), EDMA_TCD_TYPE(edma)) &
		DMA_CITER_ELINKNO_CITER_MASK) >>
	       DMA_CITER_ELINKNO_CITER_SHIFT;
}

static uint32_t dma_position(uint32_t ch, uint32_t ring_samples)
{
	uint32_t minor_loops = (ring_samples / SAMPLES_PER_REQUEST) - dma_citer(ch);

	return minor_loops * SAMPLES_PER_REQUEST;
}

/*
 * Absolute position of a ring channel. Must be called with interrupts
 * locked so that halves and the DMA position are read consistently.
 */
static uint64_t ring_counter(uint32_t ch, uint64_t halves, uint32_t half_samples)
{
	uint32_t pos = dma_position(ch, 2U * half_samples);
	uint32_t half = (pos >= half_samples) ? 1U : 0U;

	/* Half boundary crossed, interrupt still pending */
	if ((uint32_t)(halves & 1U) != half) {
		halves++;
	}

	return halves * half_samples + (pos % half_samples);
}

uint64_t stream_counter(void)
{
	unsigned int key = irq_lock();
	uint64_t count = running ? ring_counter(RX_DMA_CH, rx_halves, RX_HALF_SAMPLES) : 0U;

	irq_unlock(key);

	return count;
}

static void tx_fill_tone(struct stream_iq *dst, uint32_t count, int32_t amp)
{
	for (uint32_t n = 0; n < count; n++) {
		uint32_t idx = tone_phase >> (32U - TONE_TABLE_BITS);
		/* sin(x) = cos(x - pi/2) */
		uint32_t idx_q = (idx - TONE_TABLE_SIZE / 4U) & (TONE_TABLE_SIZE - 1U);

		dst[n].i = (int32_t)(((int64_t)tone_table[idx] * amp) >> 15);
		dst[n].q = (int32_t)(((int64_t)tone_table[idx_q] * amp) >> 15);
		tone_phase += tone_step;
	}
}

/* Move TX samples [first, first + count) from tx_buf into the DMA ring */
static void tx_fill(struct stream_iq *dst, uint64_t first, uint32_t count)
{
	int32_t amp = tone_amplitude;

	if (amp != 0) {
		tx_fill_tone(dst, count, amp);
		return;
	}

	for (uint32_t n = 0; n < count; n++) {
		struct phx_cs16 *s = &tx_buf[(first + n) & (STREAM_TX_BUFFER_SAMPLES - 1U)];

		/* CS16 into the upper half of the left-aligned I2S word */
		dst[n].i = (int32_t)s->i * 65536;
		dst[n].q = (int32_t)s->q * 65536;
		s->i = 0;
		s->q = 0;
	}
}

static void rx_dma_isr(const void *arg)
{
	ARG_UNUSED(arg);

	EDMA_ClearChannelStatusFlags(edma, RX_DMA_CH, kEDMA_InterruptFlag);
	rx_halves++;
}

static void tx_dma_isr(const void *arg)
{
	uint32_t done_half;
	uint32_t pos;

	ARG_UNUSED(arg);

	EDMA_ClearChannelStatusFlags(edma, TX_DMA_CH, kEDMA_InterruptFlag);
	tx_halves++;

	/* The half that has just been read is refilled for its next pass */
	done_half = (uint32_t)((tx_halves - 1U) & 1U);

	/* The DMA must be in the other half, otherwise the refill is too late */
	pos = dma_position(TX_DMA_CH, 2U * tx_half_samples);
	if (((pos >= tx_half_samples) ? 0U : 1U) != done_half) {
		atomic_inc(&tx_refill_late);
	}

	tx_fill(&tx_ring[done_half * tx_half_samples], (tx_halves + 1U) * tx_half_samples,
		tx_half_samples);
}

static void sai_isr(const void *arg)
{
	bool error = false;

	ARG_UNUSED(arg);

	/* Frame sync errors: frame sync earlier than expected (diagnostics) */
	if (SAI_RxGetStatusFlag(sai) & kSAI_SyncErrorFlag) {
		SAI_RxClearStatusFlags(sai, kSAI_SyncErrorFlag);
		atomic_inc(&rx_sync_errors);
	}
	if (SAI_TxGetStatusFlag(sai) & kSAI_SyncErrorFlag) {
		SAI_TxClearStatusFlags(sai, kSAI_SyncErrorFlag);
		atomic_inc(&tx_sync_errors);
	}

	if (SAI_RxGetStatusFlag(sai) & kSAI_FIFOErrorFlag) {
		SAI_RxClearStatusFlags(sai, kSAI_FIFOErrorFlag);
		atomic_inc(&rx_fifo_errors);
		error = true;
	}
	if (SAI_TxGetStatusFlag(sai) & kSAI_FIFOErrorFlag) {
		SAI_TxClearStatusFlags(sai, kSAI_FIFOErrorFlag);
		atomic_inc(&tx_fifo_errors);
		error = true;
	}

	/*
	 * A FIFO error loses or repeats samples, so the counter no longer
	 * matches the air interface. Switch the PA off immediately and stop
	 * the stream from thread context.
	 */
	if (error && running) {
		tx_enabled = false;
		board_pa_set(false);
		k_work_submit(&fault_work);
	}
}

static void configure_dma(uint32_t ch, uint32_t request, void *src, int16_t src_off,
			  void *dst, int16_t dst_off, uint32_t ring_bytes)
{
	edma_transfer_config_t cfg;

	EDMA_ResetChannel(edma, ch);
	EDMA_PrepareTransferConfig(&cfg, src, WORD_BYTES, src_off, dst, WORD_BYTES, dst_off,
				   REQUEST_BYTES, ring_bytes);
	EDMA_SetTransferConfig(edma, ch, &cfg, NULL);
	/* Wrap the memory side back to the ring start after each major loop */
	EDMA_SetMajorOffsetConfig(edma, ch, (src_off != 0) ? -(int32_t)ring_bytes : 0,
				  (dst_off != 0) ? -(int32_t)ring_bytes : 0);
	/* Run forever: no request disable at the end of the major loop */
	EDMA_EnableAutoStopRequest(edma, ch, false);
	EDMA_EnableChannelInterrupts(edma, ch,
				     kEDMA_MajorInterruptEnable | kEDMA_HalfInterruptEnable);
	EDMA_SetChannelMux(edma, ch, request);
}

static void configure_sai(void)
{
	sai_transceiver_t cfg;

	SAI_TxSoftwareReset(sai, kSAI_ResetAll);
	SAI_RxSoftwareReset(sai, kSAI_ResetAll);

	/*
	 * SX1255 IISM mode B2: standard I2S, 2 x 32-bit slots, WS low = I,
	 * WS one bit clock ahead of the MSB (frame sync early), bit clock and
	 * frame sync from the SX1255. Verified with RX/TX tones in both RF
	 * loopback and over the air.
	 */
	SAI_GetClassicI2SConfig(&cfg, kSAI_WordWidth32bits, kSAI_Stereo, kSAI_Channel0Mask);
	cfg.masterSlave = kSAI_Slave;
	cfg.fifo.fifoWatermark = FIFO_WATERMARK;
	/* Keep the word/slot alignment after a FIFO error */
	cfg.fifo.fifoContinueOneError = true;

	cfg.syncMode = kSAI_ModeAsync;
	SAI_RxSetConfig(sai, &cfg);

	cfg.syncMode = kSAI_ModeSync;
	SAI_TxSetConfig(sai, &cfg);
}

static uint32_t tx_half_for_rate(uint32_t sample_rate)
{
	uint32_t target = (uint32_t)(((uint64_t)sample_rate * TX_HALF_TARGET_US) / 1000000U);
	uint32_t half = TX_HALF_SAMPLES_MIN;

	while (half * 2U <= target && half < TX_HALF_SAMPLES_MAX) {
		half *= 2U;
	}

	return half;
}

static void stop_hw(void)
{
	/* TX is synchronous to RX: disable TX first */
	sai->TCSR = (sai->TCSR & ~(I2S_TCSR_TE_MASK | I2S_TCSR_FRDE_MASK | I2S_TCSR_FEIE_MASK |
				   I2S_TCSR_SEIE_MASK)) &
		    0xFFE3FFFFU;
	sai->RCSR = (sai->RCSR & ~(I2S_RCSR_RE_MASK | I2S_RCSR_FRDE_MASK | I2S_RCSR_FEIE_MASK |
				   I2S_RCSR_SEIE_MASK | I2S_RCSR_BCE_MASK)) &
		    0xFFE3FFFFU;

	irq_disable(DT_IRQ_BY_IDX(EDMA_NODE, RX_DMA_CH, irq));
	irq_disable(DT_IRQ_BY_IDX(EDMA_NODE, TX_DMA_CH, irq));
	irq_disable(DT_IRQN(SAI_NODE));

	EDMA_DisableChannelRequest(edma, RX_DMA_CH);
	EDMA_DisableChannelRequest(edma, TX_DMA_CH);
}

static void fault_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_ERR("SAI FIFO error (rx %ld, tx %ld), stream stopped", atomic_get(&rx_fifo_errors),
		atomic_get(&tx_fifo_errors));
	stream_stop();

	if (fault_handler != NULL) {
		fault_handler(-EIO);
	}
}

int stream_init(void)
{
	edma_config_t edma_cfg;
	int ret;

	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(SAI_NODE), PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		LOG_ERR("SAI pinctrl failed (%d)", ret);
		return ret;
	}

	RESET_ReleasePeripheralReset(kDMA0_RST_SHIFT_RSTn);
	CLOCK_EnableClock(kCLOCK_Dma0);

	EDMA_GetDefaultConfig(&edma_cfg);
	/* Keep streaming while the core is halted by a debugger */
	edma_cfg.enableDebugMode = false;
	EDMA_Init(edma, &edma_cfg);

	/* SAI1 clock is enabled by the board code when the node is okay */
	SAI_Init(sai);

	for (uint32_t i = 0; i < TONE_TABLE_SIZE; i++) {
		/* Scaled slightly below full scale to avoid float rounding overflow */
		tone_table[i] = (int32_t)(cosf(TWO_PI * (float)i / TONE_TABLE_SIZE) * 2147483520.0f);
	}

	k_work_init(&fault_work, fault_work_handler);

	IRQ_CONNECT(DT_IRQ_BY_IDX(EDMA_NODE, RX_DMA_CH, irq), DMA_IRQ_PRIO, rx_dma_isr, NULL, 0);
	IRQ_CONNECT(DT_IRQ_BY_IDX(EDMA_NODE, TX_DMA_CH, irq), DMA_IRQ_PRIO, tx_dma_isr, NULL, 0);
	IRQ_CONNECT(DT_IRQN(SAI_NODE), SAI_IRQ_PRIO, sai_isr, NULL, 0);

	initialized = true;

	return 0;
}

void stream_set_fault_handler(stream_fault_handler_t handler)
{
	fault_handler = handler;
}

int stream_start(void)
{
	uint32_t sample_rate;
	int64_t deadline;
	int ret;

	if (!initialized || !sx1255_is_ready()) {
		return -ENODEV;
	}

	k_mutex_lock(&lock, K_FOREVER);

	if (running) {
		ret = -EALREADY;
		goto out;
	}

	sample_rate = sx1255_get_sample_rate();

	/* SX1255 receiver and transmitter on; the TX driver stays off */
	ret = sx1255_set_mode(true, true, false);
	if (ret < 0) {
		goto out;
	}

	tx_half_samples = tx_half_for_rate(sample_rate);
	rx_halves = 0U;
	tx_halves = 0U;
	tx_enabled = false;
	atomic_clear(&rx_fifo_errors);
	atomic_clear(&tx_fifo_errors);
	atomic_clear(&rx_sync_errors);
	atomic_clear(&tx_sync_errors);
	atomic_clear(&tx_refill_late);
	atomic_clear(&tx_late);
	atomic_clear(&tx_early);

	memset(rx_ring, 0, sizeof(rx_ring));
	memset(tx_buf, 0, sizeof(tx_buf));
	tone_phase = 0U;
	tx_fill(tx_ring, 0U, 2U * tx_half_samples);

	configure_sai();

	configure_dma(RX_DMA_CH, RX_DMA_REQ,
		      (void *)SAI_RxGetDataRegisterAddress(sai, SAI_DATA_CH), 0, rx_ring,
		      WORD_BYTES, sizeof(rx_ring));
	configure_dma(TX_DMA_CH, TX_DMA_REQ, tx_ring, WORD_BYTES,
		      (void *)SAI_TxGetDataRegisterAddress(sai, SAI_DATA_CH), 0,
		      2U * tx_half_samples * sizeof(struct stream_iq));

	EDMA_EnableChannelRequest(edma, RX_DMA_CH);
	EDMA_EnableChannelRequest(edma, TX_DMA_CH);

	irq_enable(DT_IRQ_BY_IDX(EDMA_NODE, RX_DMA_CH, irq));
	irq_enable(DT_IRQ_BY_IDX(EDMA_NODE, TX_DMA_CH, irq));
	irq_enable(DT_IRQN(SAI_NODE));

	SAI_TxClearStatusFlags(sai, kSAI_FIFOErrorFlag | kSAI_SyncErrorFlag);
	SAI_RxClearStatusFlags(sai, kSAI_FIFOErrorFlag | kSAI_SyncErrorFlag);

	/*
	 * Enable TX first: it waits for the receiver's bit clock, which only
	 * starts with RE, so both begin on the same frame sync. The TX DMA
	 * fills the FIFO in between.
	 */
	sai->TCSR = (sai->TCSR & 0xFFE3FFFFU) | I2S_TCSR_FRDE_MASK | I2S_TCSR_FEIE_MASK |
		    I2S_TCSR_SEIE_MASK | I2S_TCSR_TE_MASK;
	sai->RCSR = (sai->RCSR & 0xFFE3FFFFU) | I2S_RCSR_FRDE_MASK | I2S_RCSR_FEIE_MASK |
		    I2S_RCSR_SEIE_MASK | I2S_RCSR_BCE_MASK | I2S_RCSR_RE_MASK;

	rate = sample_rate;
	running = true;

	/* The SX1255 must deliver bit clock and frame sync */
	deadline = k_uptime_get() + START_TIMEOUT_MS;
	while (stream_counter() == 0U) {
		if (k_uptime_get() > deadline) {
			LOG_ERR("No I2S clock from the SX1255");
			stop_hw();
			running = false;
			sx1255_set_mode(false, false, false);
			ret = -ETIMEDOUT;
			goto out;
		}
		k_msleep(1);
	}

	LOG_INF("Streaming at %u S/s, TX refill block %u samples", rate, tx_half_samples);
	ret = 0;

out:
	k_mutex_unlock(&lock);
	return ret;
}

int stream_stop(void)
{
	k_mutex_lock(&lock, K_FOREVER);

	if (running) {
		stream_tx_enable(false, false);
		stop_hw();
		running = false;
		sx1255_set_mode(false, false, false);
		LOG_INF("Streaming stopped");
	}

	k_mutex_unlock(&lock);

	return 0;
}

bool stream_is_running(void)
{
	return running;
}

int stream_tx_enable(bool enable, bool tx_driver)
{
	bool rx, tx, drv;
	int ret;

	if (enable) {
		if (!running) {
			return -EAGAIN;
		}

		/* PA supply first, then the drive signal */
		ret = board_pa_set(true);
		if (ret < 0) {
			return ret;
		}
		ret = sx1255_set_mode(true, true, tx_driver);
		if (ret < 0) {
			board_pa_set(false);
			return ret;
		}
		tx_enabled = true;
		LOG_INF("TX enabled (driver %s)", tx_driver ? "on" : "off");
		return 0;
	}

	/* Stop accepting data, then remove drive signal and PA supply */
	tx_enabled = false;
	sx1255_get_mode(&rx, &tx, &drv);
	if (drv) {
		sx1255_set_mode(rx, tx, false);
	}
	ret = board_pa_set(false);

	/*
	 * Clear pending TX data. The refill ISR may run concurrently but only
	 * writes zeros to tx_buf as well; no new data is accepted.
	 */
	memset(tx_buf, 0, sizeof(tx_buf));

	return ret;
}

bool stream_tx_is_enabled(void)
{
	return tx_enabled;
}

int stream_tx_write(int64_t timestamp, const struct phx_cs16 *src, size_t count)
{
	unsigned int key;
	uint64_t committed;
	size_t first;
	size_t part;

	if (count == 0U || count > STREAM_TX_BUFFER_SAMPLES || timestamp < 0) {
		return -EINVAL;
	}

	key = irq_lock();

	if (!running || !tx_enabled) {
		irq_unlock(key);
		return -EAGAIN;
	}

	committed = (tx_halves + 2U) * tx_half_samples;
	if ((uint64_t)timestamp < committed) {
		irq_unlock(key);
		atomic_inc(&tx_late);
		return -ETIME;
	}
	if ((uint64_t)timestamp + count > committed + STREAM_TX_BUFFER_SAMPLES) {
		irq_unlock(key);
		atomic_inc(&tx_early);
		return -EFBIG;
	}

	first = (size_t)((uint64_t)timestamp & (STREAM_TX_BUFFER_SAMPLES - 1U));
	part = MIN(count, STREAM_TX_BUFFER_SAMPLES - first);
	memcpy(&tx_buf[first], src, part * sizeof(*src));
	if (part < count) {
		memcpy(&tx_buf[0], &src[part], (count - part) * sizeof(*src));
	}

	irq_unlock(key);

	return 0;
}

static inline int16_t to_cs16(int32_t v)
{
	/* Round to the upper 16 bits with saturation */
	int64_t r = ((int64_t)v + 0x8000) >> 16;

	return (int16_t)CLAMP(r, -32767, 32767);
}

int stream_rx_read(uint64_t index, struct phx_cs16 *dst, size_t count)
{
	uint64_t now;

	if (!running) {
		return -EAGAIN;
	}

	now = stream_counter();
	if (index + count > now) {
		return -EAGAIN;
	}
	if (now - index > STREAM_RX_RING_SAMPLES) {
		return -EOVERFLOW;
	}

	for (size_t n = 0; n < count; n++) {
		const struct stream_iq *s = &rx_ring[(index + n) & (STREAM_RX_RING_SAMPLES - 1U)];

		dst[n].i = to_cs16(s->i);
		dst[n].q = to_cs16(s->q);
	}

	/* The DMA must not have overwritten the range while copying */
	if (stream_counter() - index > STREAM_RX_RING_SAMPLES) {
		return -EOVERFLOW;
	}

	return 0;
}

void stream_get_stats(struct stream_stats *stats)
{
	stats->counter = stream_counter();
	stats->rate = rate;
	stats->tx_block_samples = tx_half_samples;
	stats->rx_fifo_errors = (uint32_t)atomic_get(&rx_fifo_errors);
	stats->tx_fifo_errors = (uint32_t)atomic_get(&tx_fifo_errors);
	stats->rx_sync_errors = (uint32_t)atomic_get(&rx_sync_errors);
	stats->tx_sync_errors = (uint32_t)atomic_get(&tx_sync_errors);
	stats->tx_refill_late = (uint32_t)atomic_get(&tx_refill_late);
	stats->tx_late = (uint32_t)atomic_get(&tx_late);
	stats->tx_early = (uint32_t)atomic_get(&tx_early);
	stats->tx_blocks = tx_halves;
	stats->running = running;
	stats->tx_enabled = tx_enabled;
}

void stream_get_sai_regs(struct stream_sai_regs *regs)
{
	regs->tcsr = sai->TCSR;
	regs->tcr2 = sai->TCR2;
	regs->tcr3 = sai->TCR3;
	regs->tcr4 = sai->TCR4;
	regs->tcr5 = sai->TCR5;
	regs->tfr0 = sai->TFR[SAI_DATA_CH];
	regs->rcsr = sai->RCSR;
	regs->rcr2 = sai->RCR2;
	regs->rcr3 = sai->RCR3;
	regs->rcr4 = sai->RCR4;
	regs->rcr5 = sai->RCR5;
	regs->rfr0 = sai->RFR[SAI_DATA_CH];
}

int stream_rx_snapshot(struct stream_iq *dst, size_t count, uint64_t *first_index)
{
	uint64_t end;
	uint64_t start;

	if (!running) {
		return -EAGAIN;
	}
	if (count == 0U || count > RX_HALF_SAMPLES) {
		return -EINVAL;
	}

	end = stream_counter();
	if (end < count) {
		return -EAGAIN;
	}
	start = end - count;

	for (size_t n = 0; n < count; n++) {
		dst[n] = rx_ring[(start + n) & (STREAM_RX_RING_SAMPLES - 1U)];
	}

	/* The DMA must not have overwritten the copied range meanwhile */
	if (stream_counter() - start > STREAM_RX_RING_SAMPLES) {
		return -EOVERFLOW;
	}

	*first_index = start;

	return 0;
}

void stream_tx_set_tone(int32_t freq_hz, uint16_t amplitude_permille)
{
	uint32_t sample_rate = sx1255_get_sample_rate();
	int64_t step;
	unsigned int key;

	if (sample_rate == 0U) {
		return;
	}

	step = ((int64_t)freq_hz * (int64_t)BIT64(32)) / (int64_t)sample_rate;

	key = irq_lock();
	tone_step = (uint32_t)step;
	tone_amplitude = (int32_t)((MIN(amplitude_permille, 1000U) * 32767U) / 1000U);
	irq_unlock(key);
}
