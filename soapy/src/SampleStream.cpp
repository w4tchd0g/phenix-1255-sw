// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors

#include "SampleStream.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phx {

namespace {

// RX: about 2.5 s at 600 kHz; TX: about 2.5 s of queued bursts
constexpr size_t RX_RING_PACKETS = 4096;
constexpr size_t TX_RING_PACKETS = 4096;

// The device time estimate is extrapolated at most this far
constexpr double MAX_EXTRAPOLATION_S = 0.1;

// Device counter warnings are logged at most once per interval
constexpr auto COUNTER_LOG_INTERVAL = std::chrono::seconds(1);

constexpr float CS16_SCALE = 32767.0f;

void toCs16(const void *src, SampleFormat fmt, size_t offset, phx_cs16 *dst, size_t n)
{
    if (fmt == SampleFormat::CS16) {
        std::memcpy(dst, static_cast<const int16_t *>(src) + 2 * offset, n * sizeof(*dst));
        return;
    }
    const float *f = static_cast<const float *>(src) + 2 * offset;
    for (size_t k = 0; k < n; k++) {
        const float i = std::max(-1.0f, std::min(1.0f, f[2 * k]));
        const float q = std::max(-1.0f, std::min(1.0f, f[2 * k + 1]));
        dst[k].i = static_cast<int16_t>(std::lrintf(i * CS16_SCALE));
        dst[k].q = static_cast<int16_t>(std::lrintf(q * CS16_SCALE));
    }
}

void fromCs16(const phx_cs16 *src, void *dst, SampleFormat fmt, size_t offset, size_t n)
{
    if (fmt == SampleFormat::CS16) {
        std::memcpy(static_cast<int16_t *>(dst) + 2 * offset, src, n * sizeof(*src));
        return;
    }
    float *f = static_cast<float *>(dst) + 2 * offset;
    for (size_t k = 0; k < n; k++) {
        f[2 * k] = src[k].i / CS16_SCALE;
        f[2 * k + 1] = src[k].q / CS16_SCALE;
    }
}

} // namespace

SampleStream::SampleStream(Transport &transport)
    : _transport(transport), _rxRing(RX_RING_PACKETS), _txRing(TX_RING_PACKETS)
{
    _rxThread = std::thread(&SampleStream::rxLoop, this);
    _txThread = std::thread(&SampleStream::txLoop, this);
}

SampleStream::~SampleStream()
{
    _running = false;
    _rxCond.notify_all();
    _txCond.notify_all();
    if (_rxThread.joinable())
        _rxThread.join();
    if (_txThread.joinable())
        _txThread.join();
}

void SampleStream::setSampleRate(double rate) { _rate = rate; }

void SampleStream::setLeadTime(double seconds) { _leadSeconds = seconds; }

long long SampleStream::ticksToNs(int64_t ticks) const
{
    return SoapySDR::ticksToTimeNs(ticks, _rate);
}

int64_t SampleStream::nsToTicks(long long ns) const
{
    return SoapySDR::timeNsToTicks(ns, _rate);
}

int64_t SampleStream::estimatedCounter() const
{
    std::lock_guard<std::mutex> lock(_timeMutex);
    if (_lastEnd < 0)
        return -1;
    const double elapsed =
        std::min(std::chrono::duration<double>(Clock::now() - _lastArrival).count(),
                 MAX_EXTRAPOLATION_S);
    return _lastEnd + static_cast<int64_t>(elapsed * _rate);
}

/*******************************************************************
 * RX
 ******************************************************************/

void SampleStream::rxStart()
{
    std::lock_guard<std::mutex> lock(_rxMutex);
    _rxHead = 0;
    _rxCount = 0;
    _rxExpected = -1;
    _rxActive = true;
}

void SampleStream::rxStop()
{
    std::lock_guard<std::mutex> lock(_rxMutex);
    _rxActive = false;
    _rxCount = 0;
    _rxCond.notify_all();
}

void SampleStream::checkDeviceCounters(const phx_sample_hdr &hdr)
{
    static Clock::time_point lastLog;
    const auto now = Clock::now();

    if (now - lastLog < COUNTER_LOG_INTERVAL)
        return;

    if (hdr.rx_dropped != _devRxDropped) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
                       "Phenix-1255: device dropped %u RX samples (total %u)",
                       hdr.rx_dropped - _devRxDropped, hdr.rx_dropped);
        _devRxDropped = hdr.rx_dropped;
        lastLog = now;
    }
    if (hdr.tx_late != _devTxLate) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
                       "Phenix-1255: %u TX packets arrived too late and were discarded "
                       "(total %u)",
                       hdr.tx_late - _devTxLate, hdr.tx_late);
        _devTxLate = hdr.tx_late;
        lastLog = now;
    }
    if (hdr.tx_early != _devTxEarly) {
        SoapySDR::logf(SOAPY_SDR_WARNING,
                       "Phenix-1255: %u TX packets were too far ahead and were discarded "
                       "(total %u)",
                       hdr.tx_early - _devTxEarly, hdr.tx_early);
        _devTxEarly = hdr.tx_early;
        lastLog = now;
    }
}

void SampleStream::rxLoop()
{
    std::vector<uint8_t> buf(PHX_MAX_SAMPLE_PACKET_BYTES + 64);
    Clock::time_point lastOverflowLog;
    uint64_t overflowsLogged = 0;

    while (_running) {
        const size_t n = _transport.recvSamples(buf.data(), buf.size(), 100);
        if (n < sizeof(phx_sample_hdr))
            continue;

        phx_sample_hdr hdr;
        std::memcpy(&hdr, buf.data(), sizeof(hdr));
        if (hdr.magic != PHX_SAMPLE_MAGIC || hdr.version != PHX_PROTO_VERSION ||
            hdr.type != PHX_SAMPLE_RX || hdr.count == 0 ||
            hdr.count > PHX_MAX_PACKET_SAMPLES ||
            n < sizeof(hdr) + hdr.count * sizeof(phx_cs16))
            continue;

        const auto arrival = Clock::now();
        {
            std::lock_guard<std::mutex> lock(_timeMutex);
            _lastEnd = hdr.timestamp + hdr.count;
            _lastArrival = arrival;
        }
        checkDeviceCounters(hdr);

        std::lock_guard<std::mutex> lock(_rxMutex);
        if (!_rxActive)
            continue;

        // Lost samples are visible to the application through the timestamps
        if (_rxExpected >= 0 && hdr.timestamp != _rxExpected)
            SoapySDR::logf(SOAPY_SDR_WARNING, "Phenix-1255: RX discontinuity of %lld samples",
                           static_cast<long long>(hdr.timestamp - _rxExpected));
        _rxExpected = hdr.timestamp + hdr.count;

        if (_rxCount == _rxRing.size()) {
            // Application not reading fast enough: drop the oldest packet
            _rxHead = (_rxHead + 1) % _rxRing.size();
            _rxCount--;
            _rxOverflows++;
            if (arrival - lastOverflowLog > COUNTER_LOG_INTERVAL) {
                SoapySDR::logf(SOAPY_SDR_WARNING,
                               "Phenix-1255: RX overflow, %llu packets dropped on the host",
                               static_cast<unsigned long long>(_rxOverflows - overflowsLogged));
                overflowsLogged = _rxOverflows;
                lastOverflowLog = arrival;
            }
        }

        Packet &p = _rxRing[(_rxHead + _rxCount) % _rxRing.size()];
        p.timestamp = hdr.timestamp;
        p.count = hdr.count;
        p.offset = 0;
        std::memcpy(p.samples, buf.data() + sizeof(hdr), hdr.count * sizeof(phx_cs16));
        _rxCount++;
        _rxCond.notify_one();
    }
}

int SampleStream::read(void *buff, SampleFormat fmt, size_t numElems, int &flags,
                       long long &timeNs, long timeoutUs)
{
    std::unique_lock<std::mutex> lock(_rxMutex);

    flags = 0;
    if (!_rxCond.wait_for(lock, std::chrono::microseconds(std::max(timeoutUs, 0L)),
                          [&] { return _rxCount > 0 || !_rxActive || !_running; }))
        return SOAPY_SDR_TIMEOUT;
    if (_rxCount == 0)
        return _rxActive ? SOAPY_SDR_TIMEOUT : SOAPY_SDR_STREAM_ERROR;

    const int64_t first = _rxRing[_rxHead].timestamp + _rxRing[_rxHead].offset;
    size_t done = 0;

    // Copy contiguous samples; a discontinuity ends this read
    while (done < numElems && _rxCount > 0) {
        Packet &p = _rxRing[_rxHead];
        if (p.timestamp + p.offset != first + static_cast<int64_t>(done))
            break;
        const size_t take = std::min<size_t>(numElems - done, p.count - p.offset);
        fromCs16(p.samples + p.offset, buff, fmt, done, take);
        p.offset += static_cast<uint16_t>(take);
        done += take;
        if (p.offset == p.count) {
            _rxHead = (_rxHead + 1) % _rxRing.size();
            _rxCount--;
        }
    }

    timeNs = ticksToNs(first);
    flags = SOAPY_SDR_HAS_TIME;
    return static_cast<int>(done);
}

/*******************************************************************
 * TX
 ******************************************************************/

void SampleStream::txStart()
{
    std::lock_guard<std::mutex> lock(_txMutex);
    _txHead = 0;
    _txCount = 0;
    _txNext = -1;
    _txActive = true;
}

void SampleStream::txStop()
{
    std::lock_guard<std::mutex> lock(_txMutex);
    _txActive = false;
    _txCount = 0;
    _txCond.notify_all();
}

int SampleStream::write(const void *buff, SampleFormat fmt, size_t numElems, int &flags,
                        long long timeNs, long timeoutUs)
{
    const auto deadline = Clock::now() + std::chrono::microseconds(std::max(timeoutUs, 0L));
    std::unique_lock<std::mutex> lock(_txMutex);

    if (!_txActive)
        return SOAPY_SDR_STREAM_ERROR;

    int64_t ts;
    if (flags & SOAPY_SDR_HAS_TIME) {
        ts = nsToTicks(timeNs);
    } else {
        // Continue after the previous write, or start one lead time ahead
        const int64_t est = estimatedCounter();
        if (_txNext >= 0 && (est < 0 || _txNext >= est))
            ts = _txNext;
        else
            ts = (est < 0) ? 0 : est + static_cast<int64_t>(_leadSeconds * _rate);
    }

    size_t done = 0;
    while (done < numElems) {
        if (_txCount == _txRing.size()) {
            if (!_txCond.wait_until(lock, deadline, [&] {
                    return _txCount < _txRing.size() || !_txActive || !_running;
                }))
                break;
            if (!_txActive || !_running)
                break;
            continue;
        }

        Packet &p = _txRing[(_txHead + _txCount) % _txRing.size()];
        const size_t take = std::min<size_t>(numElems - done, PHX_MAX_PACKET_SAMPLES);
        p.timestamp = ts + static_cast<int64_t>(done);
        p.count = static_cast<uint16_t>(take);
        p.offset = 0;
        toCs16(buff, fmt, done, p.samples, take);
        _txCount++;
        done += take;
    }

    _txNext = ts + static_cast<int64_t>(done);
    _txCond.notify_all();
    flags = 0;

    return (done > 0) ? static_cast<int>(done) : SOAPY_SDR_TIMEOUT;
}

void SampleStream::txLoop()
{
    std::vector<uint8_t> pkt(PHX_MAX_SAMPLE_PACKET_BYTES);

    while (_running) {
        std::unique_lock<std::mutex> lock(_txMutex);
        if (_txCount == 0) {
            _txCond.wait_for(lock, std::chrono::milliseconds(10));
            continue;
        }

        const Packet &p = _txRing[_txHead];
        const int64_t est = estimatedCounter();
        const int64_t lead = static_cast<int64_t>(_leadSeconds * _rate);

        // Without RX timing the packets are sent immediately
        if (est >= 0 && p.timestamp > est + lead) {
            const double waitS = std::min(std::max((p.timestamp - est - lead) / _rate, 50e-6),
                                          5e-3);
            _txCond.wait_for(lock, std::chrono::duration<double>(waitS));
            continue;
        }

        phx_sample_hdr hdr{};
        hdr.magic = PHX_SAMPLE_MAGIC;
        hdr.version = PHX_PROTO_VERSION;
        hdr.type = PHX_SAMPLE_TX;
        hdr.count = p.count;
        hdr.seq = _txSeq++;
        hdr.timestamp = p.timestamp;
        std::memcpy(pkt.data(), &hdr, sizeof(hdr));
        std::memcpy(pkt.data() + sizeof(hdr), p.samples, p.count * sizeof(phx_cs16));
        const size_t len = sizeof(hdr) + p.count * sizeof(phx_cs16);

        _txHead = (_txHead + 1) % _txRing.size();
        _txCount--;
        lock.unlock();
        _txCond.notify_all();

        _transport.sendSamples(pkt.data(), len);
    }
}

} // namespace phx
