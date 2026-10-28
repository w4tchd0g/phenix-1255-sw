// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Transport abstraction: a control channel carrying complete control
// messages and a sample channel carrying complete sample packets.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phx {

class Transport
{
public:
    virtual ~Transport() = default;

    /// Send one complete control message. Throws on a broken connection.
    virtual void sendControl(const uint8_t *msg, size_t len) = 0;

    /// Receive one complete control message. Returns false on timeout,
    /// throws when the connection is closed.
    virtual bool recvControl(std::vector<uint8_t> &msg, int timeoutMs) = 0;

    /// Send one TX sample packet.
    virtual void sendSamples(const uint8_t *pkt, size_t len) = 0;

    /// Receive one RX sample packet. Returns its length, 0 on timeout.
    virtual size_t recvSamples(uint8_t *buf, size_t maxLen, int timeoutMs) = 0;

    /// Host port for RX sample packets announced in HELLO (0 if not used).
    virtual uint16_t rxPort() const = 0;

    /// Human readable description, e.g. "eth 192.168.2.10".
    virtual std::string description() const = 0;

    /// Unblock pending receive calls (used on shutdown).
    virtual void shutdown() = 0;
};

} // namespace phx
