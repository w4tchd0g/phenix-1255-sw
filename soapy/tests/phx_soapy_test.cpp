// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Test program for the phenix-1255 SoapySDR module.
//
//   phx_soapy_test rx    [args] [seconds] [rate]
//   phx_soapy_test tone  [args] [seconds] [rate] [offset_hz]
//
// "args" is a SoapySDR device argument string, e.g. "driver=phenix-1255".
// "rx" checks RX timestamps (always present, contiguous, rate) and the
// getHardwareTime() estimate. "tone" additionally transmits a tone in timed
// blocks of 1.5 ms (as TETRA-Bluestation does) and measures it in the RX
// stream; it transmits, so use an attenuator or dummy load.

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Time.hpp>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using cf = std::complex<float>;
using Clock = std::chrono::steady_clock;

static double powerAt(const std::vector<cf> &x, double fs, double f, double &acPowerDb)
{
    cf mean(0, 0);
    for (const auto &v : x)
        mean += v;
    mean /= static_cast<float>(x.size());

    double total = 0;
    std::complex<double> c(0, 0);
    for (size_t k = 0; k < x.size(); k++) {
        const std::complex<double> v(x[k] - mean);
        total += std::norm(v);
        c += v * std::polar(1.0, -2 * M_PI * f * static_cast<double>(k) / fs);
    }
    total /= x.size();
    c /= static_cast<double>(x.size());
    acPowerDb = 10 * std::log10(total + 1e-20);
    return 10 * std::log10(std::norm(c) / (total + 1e-20) + 1e-20);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s rx|tone [args] [seconds] [rate] [offset_hz]\n",
                     argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const std::string args = (argc > 2) ? argv[2] : "driver=phenix-1255";
    const double seconds = (argc > 3) ? std::atof(argv[3]) : 5.0;
    const double rate = (argc > 4) ? std::atof(argv[4]) : 600e3;
    const double offset = (argc > 5) ? std::atof(argv[5]) : 50e3;
    const bool tx = (mode == "tone");

    auto *dev = SoapySDR::Device::make(args);
    std::printf("driver %s, hardware %s\n", dev->getDriverKey().c_str(),
                dev->getHardwareKey().c_str());

    dev->setSampleRate(SOAPY_SDR_RX, 0, rate);
    dev->setFrequency(SOAPY_SDR_RX, 0, 434e6);
    dev->setFrequency(SOAPY_SDR_TX, 0, 434e6);
    if (tx) {
        // Cable with attenuator from TX to RX
        dev->setGain(SOAPY_SDR_RX, 0, "LNA", 0);
        dev->setGain(SOAPY_SDR_RX, 0, "PGA", 0);
        dev->setGain(SOAPY_SDR_TX, 0, "DAC", 0);
        dev->setGain(SOAPY_SDR_TX, 0, "MIXER", 30);
    }

    auto *rxs = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    SoapySDR::Stream *txs = tx ? dev->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32) : nullptr;
    dev->activateStream(rxs);
    if (txs)
        dev->activateStream(txs);

    const size_t block = static_cast<size_t>(std::lround(rate * 1.5e-3));
    std::vector<cf> rxbuf(block), txbuf(block), capture;
    const long long leadNs = 10000000; // first TX block 10 ms after the hardware time
    long long nextTxNs = -1;
    long long expectedNs = -1;
    long long firstNs = -1, lastNs = 0;
    size_t samples = 0, gaps = 0, noTime = 0, reads = 0;
    long long maxHwLagNs = 0, minHwLagNs = 1LL << 62;
    bool streamError = false;
    const auto t0 = Clock::now();

    while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
        void *rb[] = {rxbuf.data()};
        int flags = 0;
        long long timeNs = 0;
        const int n = dev->readStream(rxs, rb, rxbuf.size(), flags, timeNs, 1000000);
        if (n == SOAPY_SDR_TIMEOUT) {
            std::printf("readStream timeout\n");
            continue;
        }
        if (n <= 0) {
            // Stream error (e.g. device disconnected): stop instead of spinning
            std::printf("readStream returned %d, aborting\n", n);
            streamError = true;
            break;
        }
        reads++;
        if (!(flags & SOAPY_SDR_HAS_TIME))
            noTime++;
        if (firstNs < 0)
            firstNs = timeNs;
        const long long ticks = SoapySDR::timeNsToTicks(timeNs, rate);
        if (expectedNs >= 0 && ticks != SoapySDR::timeNsToTicks(expectedNs, rate))
            gaps++;
        samples += n;
        expectedNs = SoapySDR::ticksToTimeNs(ticks + n, rate);
        lastNs = expectedNs;

        // Hardware time must be at or slightly after the end of the last read
        const long long lag = dev->getHardwareTime() - expectedNs;
        maxHwLagNs = std::max(maxHwLagNs, lag);
        minHwLagNs = std::min(minHwLagNs, lag);

        if (txs) {
            // As TETRA-Bluestation: schedule relative to the hardware time
            const long long hwNs = dev->getHardwareTime();
            if (nextTxNs < 0)
                nextTxNs = hwNs + leadNs;
            // Keep about 20 ms of TX blocks scheduled ahead
            while (nextTxNs < hwNs + 2 * leadNs) {
                const long long t = SoapySDR::timeNsToTicks(nextTxNs, rate);
                for (size_t k = 0; k < block; k++)
                    txbuf[k] = std::polar(0.3f, static_cast<float>(
                                                    2 * M_PI * offset * (t + k) / rate));
                const void *tb[] = {txbuf.data()};
                int txFlags = SOAPY_SDR_HAS_TIME;
                const int w = dev->writeStream(txs, tb, block, txFlags, nextTxNs, 100000);
                if (w != static_cast<int>(block))
                    std::printf("writeStream returned %d\n", w);
                nextTxNs = SoapySDR::ticksToTimeNs(t + block, rate);
            }
            const double el = std::chrono::duration<double>(Clock::now() - t0).count();
            if (el > seconds / 2 && capture.size() < 8192)
                capture.insert(capture.end(), rxbuf.begin(), rxbuf.begin() + n);
        }
    }

    if (txs) {
        dev->deactivateStream(txs);
        dev->closeStream(txs);
    }
    dev->deactivateStream(rxs);
    dev->closeStream(rxs);

    const double span = (lastNs - firstNs) * 1e-9;
    std::printf("%zu reads, %zu samples, %zu without HAS_TIME, %zu discontinuities\n", reads,
                samples, noTime, gaps);
    std::printf("timestamp span %.3f s for %zu samples: %.1f S/s\n", span, samples,
                samples / span);
    std::printf("getHardwareTime - end of last read: %.2f .. %.2f ms\n", minHwLagNs * 1e-6,
                maxHwLagNs * 1e-6);

    int rc = (!streamError && samples > 0 && noTime == 0 && gaps == 0) ? 0 : 1;
    if (txs) {
        double ac;
        const double pPos = powerAt(capture, rate, offset, ac);
        const double pNeg = powerAt(capture, rate, -offset, ac);
        std::printf("RX AC power %.1f dBFS, tone at %+.0f Hz %.1f dB, image %.1f dB\n", ac,
                    offset, pPos, pNeg);
        // Also fails without captured samples (NaN)
        if (!(pPos >= -1.0))
            rc = 1;
    }

    SoapySDR::Device::unmake(dev);
    std::printf("%s\n", rc == 0 ? "PASS" : "FAIL");
    return rc;
}
