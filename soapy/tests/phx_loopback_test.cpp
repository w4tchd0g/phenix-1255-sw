// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Loopback timing test for the phenix-1255 SoapySDR module.
//
//   phx_loopback_test [args] [rates] [bursts] [restarts] [rx_antenna]
//
//   args        SoapySDR device arguments, default "driver=phenix-1255"
//   rates       "all" (default) or a comma separated list, e.g. 600000,50000
//   bursts      timed bursts per stream start, default 20
//   restarts    stream starts per rate, default 3 (the device counter
//               restarts at 0 every time)
//   rx_antenna  "RX" (default, external TX->RX path with attenuator) or
//               "LB" (SX1255 internal RF loopback)
//
// A band-limited pseudo-random QPSK burst is transmitted at sample counter N
// with a timed writeStream. Its position in the RX stream is found by
// cross-correlation. The delay (RX position - N) consists only of the SX1255
// filter and RF path delays, so it must be identical for every burst and
// every stream start at a given sample rate. The test fails if any burst is
// missing or appears at a different integer sample position.
//
// The test transmits at 434 MHz with low gains (LNA 0, PGA 0, DAC 0,
// MIXER 30 dB), matching a TX -> 20 dB attenuator -> RX cable. With higher
// gains the receiver clips and the correlation becomes ambiguous. The
// environment variable PHX_VERBOSE=1 prints the delays of every stream
// start, the correlation peaks and the received power envelope.

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Time.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using cf = std::complex<float>;

namespace {

constexpr size_t PN_LENGTH = 512;
constexpr float PN_AMPLITUDE = 0.5f;      // peak per I and Q component
constexpr double SCHEDULE_AHEAD_S = 0.05; // burst time ahead of the hardware time
constexpr size_t SEARCH_WINDOW = 1024;    // delays searched: 0 .. SEARCH_WINDOW - 1
constexpr double MIN_PEAK_RATIO = 20.0;   // correlation peak / background, detection

const std::vector<double> ALL_RATES = {600e3, 300e3, 150e3, 75e3, 50e3, 25e3};

/*
 * Band-limited pseudo-random QPSK burst: independent I and Q chips at a
 * quarter of the sample rate, interpolated with a windowed sinc filter, so
 * that the burst occupies about +-fs/8 and passes the SX1255 filters at
 * every sample rate. The correlation is evaluated in magnitude because the
 * RF loop rotates the phase; independent I/Q chips keep the sidelobes low
 * for every phase.
 */
std::vector<cf> makePn()
{
    constexpr size_t OVERSAMPLE = 4;
    constexpr int TAPS_HALF = 16;
    const size_t chips = PN_LENGTH / OVERSAMPLE;

    std::mt19937 gen(1255);
    std::vector<cf> chip(chips);
    for (auto &c : chip) {
        const uint32_t r = gen();
        c = cf((r & 1) ? 1.0f : -1.0f, (r & 2) ? 1.0f : -1.0f);
    }

    std::vector<cf> pn(PN_LENGTH, cf(0, 0));
    float peak = 0;
    for (size_t k = 0; k < PN_LENGTH; k++) {
        for (size_t c = 0; c < chips; c++) {
            const int t = static_cast<int>(k) - static_cast<int>(c * OVERSAMPLE);
            if (t < -TAPS_HALF || t > TAPS_HALF)
                continue;
            const double x = static_cast<double>(t) / OVERSAMPLE;
            const double sinc = (t == 0) ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            const double window = 0.5 + 0.5 * std::cos(M_PI * t / (TAPS_HALF + 1));
            pn[k] += chip[c] * static_cast<float>(sinc * window);
        }
        peak = std::max(peak, std::max(std::abs(pn[k].real()), std::abs(pn[k].imag())));
    }
    for (auto &v : pn)
        v *= PN_AMPLITUDE / peak;
    return pn;
}

/// RX samples indexed by the device sample counter.
class RxBuffer
{
public:
    RxBuffer(SoapySDR::Device *dev, SoapySDR::Stream *rx, double rate)
        : _dev(dev), _rx(rx), _rate(rate), _chunk(4096)
    {
    }

    /// Read until samples up to (excluding) end are available.
    bool readUntil(long long end)
    {
        while (_start < 0 || _start + static_cast<long long>(_data.size()) < end) {
            void *buffs[] = {_chunk.data()};
            int flags = 0;
            long long timeNs = 0;
            const int n = _dev->readStream(_rx, buffs, _chunk.size(), flags, timeNs, 1000000);
            if (n == SOAPY_SDR_TIMEOUT)
                continue;
            if (n <= 0 || !(flags & SOAPY_SDR_HAS_TIME)) {
                std::printf("readStream returned %d (flags 0x%x)\n", n, flags);
                return false;
            }
            const long long t = SoapySDR::timeNsToTicks(timeNs, _rate);
            if (_start < 0)
                _start = t;
            const long long expected = _start + static_cast<long long>(_data.size());
            if (t != expected) {
                std::printf("RX discontinuity: %lld samples\n", t - expected);
                return false;
            }
            _data.insert(_data.end(), _chunk.begin(), _chunk.begin() + n);
        }
        return true;
    }

    /// Drop samples before index.
    void discardBefore(long long index)
    {
        if (_start < 0 || index <= _start)
            return;
        const size_t n = std::min(_data.size(), static_cast<size_t>(index - _start));
        _data.erase(_data.begin(), _data.begin() + static_cast<long>(n));
        _start += static_cast<long long>(n);
    }

    long long end() const { return _start + static_cast<long long>(_data.size()); }

    const cf &at(long long index) const { return _data[static_cast<size_t>(index - _start)]; }

private:
    SoapySDR::Device *_dev;
    SoapySDR::Stream *_rx;
    double _rate;
    std::vector<cf> _chunk;
    std::vector<cf> _data;
    long long _start = -1;
};

// Print details of every burst and stream start (environment PHX_VERBOSE)
const bool verbose = std::getenv("PHX_VERBOSE") != nullptr;

struct Detection
{
    bool found = false;
    long long delay = 0;  // integer correlation peak, samples
    double frac = 0.0;    // parabolic peak interpolation, -0.5 .. 0.5
    double ratio = 0.0;   // peak / mean correlation magnitude
};

Detection detect(const RxBuffer &rx, long long n, const std::vector<cf> &pn)
{
    // The RX DC offset (LO leakage) is removed before correlating
    const size_t span = SEARCH_WINDOW + pn.size();
    std::vector<std::complex<double>> x(span);
    std::complex<double> dc(0, 0);
    for (size_t k = 0; k < span; k++) {
        x[k] = std::complex<double>(rx.at(n + static_cast<long long>(k)));
        dc += x[k];
    }
    dc /= static_cast<double>(span);

    std::vector<double> mag(SEARCH_WINDOW);
    for (size_t d = 0; d < SEARCH_WINDOW; d++) {
        std::complex<double> c(0, 0);
        for (size_t k = 0; k < pn.size(); k++)
            c += (x[d + k] - dc) * std::conj(std::complex<double>(pn[k]));
        mag[d] = std::abs(c);
    }

    Detection det;
    const size_t peak =
        static_cast<size_t>(std::max_element(mag.begin(), mag.end()) - mag.begin());

    // Background: correlation magnitude outside the burst (+-PN_LENGTH)
    double mean = 0;
    size_t count = 0;
    for (size_t d = 0; d < mag.size(); d++) {
        if (d + pn.size() > peak && d < peak + pn.size())
            continue;
        mean += mag[d];
        count++;
    }
    mean /= static_cast<double>(std::max<size_t>(count, 1));

    det.delay = static_cast<long long>(peak);
    det.ratio = mag[peak] / (mean + 1e-20);

    if (verbose) {
        // Received power in blocks of 32 samples from N on (dBFS)
        std::printf("    envelope:");
        for (size_t b = 0; b + 32 <= span; b += 32) {
            double pw = 0;
            for (size_t k = b; k < b + 32; k++)
                pw += std::norm(x[k] - dc);
            std::printf(" %.0f", 10 * std::log10(pw / 32 + 1e-20));
        }
        std::printf("\n");
        // Strongest correlation outside +-8 samples of the main peak
        size_t second = 0;
        for (size_t d = 0; d < mag.size(); d++)
            if ((d + 8 < peak || d > peak + 8) && mag[d] > mag[second])
                second = d;
        std::printf("    peak %zu (%.0f), second %zu (%.0f dB)\n", peak, det.ratio, second,
                    20 * std::log10(mag[second] / mag[peak]));
    }
    det.found = det.ratio >= MIN_PEAK_RATIO;
    if (peak > 0 && peak + 1 < mag.size()) {
        const double l = mag[peak - 1], c = mag[peak], r = mag[peak + 1];
        const double den = l - 2 * c + r;
        det.frac = (den != 0) ? 0.5 * (l - r) / den : 0.0;
    }
    return det;
}

std::vector<double> parseRates(const std::string &s)
{
    if (s == "all")
        return ALL_RATES;
    std::vector<double> rates;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        rates.push_back(std::atof(item.c_str()));
    return rates;
}

struct RateResult
{
    size_t bursts = 0;
    size_t missing = 0;
    std::map<long long, size_t> delays; // delay -> count
    double fracMin = 1, fracMax = -1;
    double ratioMin = 1e9;
    size_t sessions = 0;
};


/// One stream start with the given number of bursts.
bool runSession(SoapySDR::Device *dev, double rate, size_t bursts, const std::vector<cf> &pn,
                RateResult &res)
{
    SoapySDR::Stream *rxs = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    SoapySDR::Stream *txs = dev->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    dev->activateStream(rxs);
    dev->activateStream(txs);

    bool ok = true;
    std::vector<long long> delays;
    RxBuffer rx(dev, rxs, rate);
    const long long ahead = static_cast<long long>(SCHEDULE_AHEAD_S * rate);

    // Let the host time estimate settle
    ok = rx.readUntil(-1) && rx.readUntil(rx.end() + static_cast<long long>(0.05 * rate));

    for (size_t b = 0; ok && b < bursts; b++) {
        const long long now = SoapySDR::timeNsToTicks(dev->getHardwareTime(), rate);
        const long long n = std::max(now, rx.end()) + ahead;

        const void *tb[] = {pn.data()};
        int flags = SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST;
        const int w = dev->writeStream(txs, tb, pn.size(), flags,
                                       SoapySDR::ticksToTimeNs(n, rate), 1000000);
        if (w != static_cast<int>(pn.size())) {
            std::printf("writeStream returned %d\n", w);
            ok = false;
            break;
        }

        if (!rx.readUntil(n + static_cast<long long>(SEARCH_WINDOW + pn.size()))) {
            ok = false;
            break;
        }

        const Detection det = detect(rx, n, pn);
        res.bursts++;
        if (!det.found) {
            res.missing++;
            std::printf("  burst at %lld not found (peak ratio %.1f at %lld)\n", n, det.ratio,
                        det.delay);
        } else {
            delays.push_back(det.delay);
            res.delays[det.delay]++;
            res.fracMin = std::min(res.fracMin, det.frac);
            res.fracMax = std::max(res.fracMax, det.frac);
            res.ratioMin = std::min(res.ratioMin, det.ratio);
        }
        rx.discardBefore(n + static_cast<long long>(SEARCH_WINDOW + pn.size()));
    }

    dev->deactivateStream(txs);
    dev->deactivateStream(rxs);
    dev->closeStream(txs);
    dev->closeStream(rxs);

    if (verbose) {
        std::printf("  stream start %zu:", res.sessions);
        for (const long long d : delays)
            std::printf(" %lld", d);
        std::printf("\n");
    }
    res.sessions++;
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    const std::string args = (argc > 1) ? argv[1] : "driver=phenix-1255";
    const std::vector<double> rates = parseRates((argc > 2) ? argv[2] : "all");
    const size_t bursts = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 20;
    const size_t restarts = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 3;
    const std::string antenna = (argc > 5) ? argv[5] : "RX";

    SoapySDR::Device *dev = SoapySDR::Device::make(args);
    dev->setAntenna(SOAPY_SDR_RX, 0, antenna);
    // 70 cm band, low gains for the TX -> 20 dB attenuator -> RX setup (as
    // phx_soapy_test); the RX must not be driven into clipping
    dev->setFrequency(SOAPY_SDR_RX, 0, 434e6);
    dev->setFrequency(SOAPY_SDR_TX, 0, 434e6);
    dev->setGain(SOAPY_SDR_RX, 0, "LNA", 0);
    dev->setGain(SOAPY_SDR_RX, 0, "PGA", 0);
    dev->setGain(SOAPY_SDR_TX, 0, "DAC", 0);
    dev->setGain(SOAPY_SDR_TX, 0, "MIXER", 30);
    const std::vector<cf> pn = makePn();

    std::printf("PN burst of %zu samples, %zu bursts x %zu stream starts per rate, RX %s\n",
                pn.size(), bursts, restarts, antenna.c_str());

    bool pass = true;
    std::vector<std::pair<double, RateResult>> results;
    for (const double rate : rates) {
        dev->setSampleRate(SOAPY_SDR_RX, 0, rate);
        RateResult res;
        bool ok = true;
        for (size_t r = 0; ok && r < restarts; r++)
            ok = runSession(dev, rate, bursts, pn, res);

        const bool consistent = ok && res.missing == 0 && res.delays.size() == 1;
        pass = pass && consistent;
        results.emplace_back(rate, res);

        std::printf("%7.0f S/s: %zu bursts, %zu missing, delays:", rate, res.bursts,
                    res.missing);
        for (const auto &d : res.delays)
            std::printf(" %lld (%zux)", d.first, d.second);
        if (!res.delays.empty())
            std::printf(", fraction %+.2f..%+.2f, min peak ratio %.0f", res.fracMin,
                        res.fracMax, res.ratioMin);
        std::printf(" -> %s\n", consistent ? "OK" : "INCONSISTENT");
    }

    std::printf("\n| Sample rate | Delay (samples) | Delay (us) |\n|---|---|---|\n");
    for (const auto &r : results) {
        if (r.second.delays.size() != 1)
            continue;
        const long long d = r.second.delays.begin()->first;
        std::printf("| %.0f | %lld | %.1f |\n", r.first, d, d * 1e6 / r.first);
    }

    SoapySDR::Device::unmake(dev);
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
