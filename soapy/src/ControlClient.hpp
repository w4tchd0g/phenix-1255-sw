// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Control channel client: request/response matching by sequence number,
// periodic keepalive and device event handling.

#pragma once

#include "Transport.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace phx {

/// A request answered with a non-zero status.
class DeviceError : public std::runtime_error
{
public:
    DeviceError(const std::string &what, int status) : std::runtime_error(what), status(status) {}
    const int status;
};

class ControlClient
{
public:
    explicit ControlClient(Transport &transport);
    ~ControlClient();

    /// Start the reader and keepalive threads.
    void start();

    /// Stop the threads (the transport must be shut down first or later).
    void stop();

    /// Send a request and wait for its response. Returns the response data
    /// after the status word. Throws DeviceError or std::runtime_error.
    std::vector<uint8_t> request(uint8_t opcode, const void *payload = nullptr,
                                 size_t len = 0, int timeoutMs = 2000);

    /// False after the connection to the device was lost.
    bool connected() const { return _connected; }

private:
    void readerLoop();
    void keepaliveLoop();
    uint16_t nextSeq();
    void send(uint8_t opcode, uint16_t seq, const void *payload, size_t len);

    Transport &_transport;
    std::mutex _sendMutex;
    std::mutex _mutex;
    std::condition_variable _cond;
    std::map<uint16_t, std::vector<uint8_t>> _responses;
    std::map<uint16_t, bool> _ignored; // keepalive sequence numbers
    uint16_t _seq = 0;
    std::atomic<bool> _running{false};
    std::atomic<bool> _connected{true};
    std::thread _reader;
    std::thread _keepalive;
};

} // namespace phx
