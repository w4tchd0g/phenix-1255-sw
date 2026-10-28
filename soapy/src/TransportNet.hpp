// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Ethernet transport: TCP control channel, UDP sample channel.

#pragma once

#include "Transport.hpp"

#include <atomic>
#include <string>

namespace phx {

class TransportNet : public Transport
{
public:
    /// Connect to the device. host may be a name or a dotted quad; names are
    /// resolved as IPv4 only.
    TransportNet(const std::string &host, uint16_t ctrlPort, uint16_t samplePort,
                 int connectTimeoutMs = 2000);
    ~TransportNet() override;

    void sendControl(const uint8_t *msg, size_t len) override;
    bool recvControl(std::vector<uint8_t> &msg, int timeoutMs) override;
    void sendSamples(const uint8_t *pkt, size_t len) override;
    size_t recvSamples(uint8_t *buf, size_t maxLen, int timeoutMs) override;
    uint16_t rxPort() const override { return _rxPort; }
    std::string description() const override { return "eth " + _address; }
    void shutdown() override;

    const std::string &address() const { return _address; }

    /// Resolve a host name to an IPv4 dotted quad. Throws on failure.
    static std::string resolve(const std::string &host);

private:
    bool readExact(uint8_t *buf, size_t len, int timeoutMs);

    std::string _address;
    int _tcp = -1;
    int _udp = -1;
    uint16_t _rxPort = 0;
    std::atomic<bool> _shutdown{false};
};

} // namespace phx
