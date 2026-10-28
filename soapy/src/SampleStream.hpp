// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Sample streaming on the host side:
//
// - RX: a receiver thread stores RX packets in a packet ring; readStream
//   copies from it and reports the device timestamp of the first sample.
// - TX: writeStream splits the data into packets with explicit timestamps;
//   a scheduler thread sends each packet when its timestamp is less than
//   the lead time ahead of the estimated device time.
// - The device time is estimated from the latest RX timestamp plus the host
//   time elapsed since that packet arrived. The device is not queried.

#pragma once

#include "Transport.hpp"

#include <phenix1255/protocol.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace phx {

enum class SampleFormat { CF32, CS16 };

class SampleStream
{
public:
    explicit SampleStream(Transport &transport);
    ~SampleStream();

    void setSampleRate(double rate);
    double sampleRate() const { return _rate; }

    /// TX lead time: how far ahead of the device time packets are sent.
    void setLeadTime(double seconds);
    double leadTime() const { return _leadSeconds; }

    // RX
    void rxStart();
    void rxStop();
    int read(void *buff, SampleFormat fmt, size_t numElems, int &flags, long long &timeNs,
             long timeoutUs);

    // TX
    void txStart();
    void txStop();
    int write(const void *buff, SampleFormat fmt, size_t numElems, int &flags,
              long long timeNs, long timeoutUs);

    /// Estimated current device sample counter, -1 while unknown.
    int64_t estimatedCounter() const;

    long long ticksToNs(int64_t ticks) const;
    int64_t nsToTicks(long long ns) const;

private:
    using Clock = std::chrono::steady_clock;

    struct Packet {
        int64_t timestamp;
        uint16_t count;
        uint16_t offset; // RX: samples already consumed
        phx_cs16 samples[PHX_MAX_PACKET_SAMPLES];
    };

    void rxLoop();
    void txLoop();
    void checkDeviceCounters(const phx_sample_hdr &hdr);

    Transport &_transport;
    std::atomic<double> _rate{600000.0};
    std::atomic<double> _leadSeconds{0.020};
    std::atomic<bool> _running{true};

    // RX packet ring
    std::mutex _rxMutex;
    std::condition_variable _rxCond;
    std::vector<Packet> _rxRing;
    size_t _rxHead = 0;  // oldest
    size_t _rxCount = 0;
    bool _rxActive = false;
    int64_t _rxExpected = -1;
    uint64_t _rxOverflows = 0;

    // Device time estimate
    mutable std::mutex _timeMutex;
    int64_t _lastEnd = -1;
    Clock::time_point _lastArrival;

    // Device counters seen in RX headers
    uint32_t _devRxDropped = 0;
    uint32_t _devTxLate = 0;
    uint32_t _devTxEarly = 0;

    // TX packet queue
    std::mutex _txMutex;
    std::condition_variable _txCond;
    std::vector<Packet> _txRing;
    size_t _txHead = 0;
    size_t _txCount = 0;
    bool _txActive = false;
    int64_t _txNext = -1;
    uint32_t _txSeq = 0;

    std::thread _rxThread;
    std::thread _txThread;
};

} // namespace phx
