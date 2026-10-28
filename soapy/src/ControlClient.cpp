// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors

#include "ControlClient.hpp"

#include <phenix1255/protocol.h>

#include <SoapySDR/Logger.hpp>

#include <chrono>
#include <cstring>
#include <random>

// The protocol is little endian; structures are used directly
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little endian host required");

namespace phx {

/*
 * The sequence numbers start at a random value: on USB, responses of a
 * previous host process may still be queued in the device and must not be
 * taken for responses of this session.
 */
ControlClient::ControlClient(Transport &transport)
    : _transport(transport), _seq(static_cast<uint16_t>(std::random_device{}()))
{
}

ControlClient::~ControlClient() { stop(); }

void ControlClient::start()
{
    _running = true;
    _reader = std::thread(&ControlClient::readerLoop, this);
    _keepalive = std::thread(&ControlClient::keepaliveLoop, this);
}

void ControlClient::stop()
{
    _running = false;
    _cond.notify_all();
    if (_reader.joinable())
        _reader.join();
    if (_keepalive.joinable())
        _keepalive.join();
}

uint16_t ControlClient::nextSeq()
{
    std::lock_guard<std::mutex> lock(_mutex);
    uint16_t seq = ++_seq;
    if (seq == 0) // 0 is used by device events
        seq = ++_seq;
    return seq;
}

void ControlClient::send(uint8_t opcode, uint16_t seq, const void *payload, size_t len)
{
    std::vector<uint8_t> msg(sizeof(phx_ctrl_hdr) + len);
    const phx_ctrl_hdr hdr{PHX_CTRL_MAGIC, PHX_PROTO_VERSION, opcode, seq,
                           static_cast<uint16_t>(len)};
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    if (len > 0)
        std::memcpy(msg.data() + sizeof(hdr), payload, len);

    std::lock_guard<std::mutex> lock(_sendMutex);
    _transport.sendControl(msg.data(), msg.size());
}

std::vector<uint8_t> ControlClient::request(uint8_t opcode, const void *payload, size_t len,
                                            int timeoutMs)
{
    if (!_connected)
        throw std::runtime_error("Phenix-1255: connection to the device lost");
    if (len > PHX_CTRL_MAX_PAYLOAD)
        throw std::invalid_argument("Phenix-1255: control payload too large");

    const uint16_t seq = nextSeq();
    send(opcode, seq, payload, len);

    std::unique_lock<std::mutex> lock(_mutex);
    const bool ok = _cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return _responses.count(seq) > 0 || !_connected;
    });
    if (!ok || _responses.count(seq) == 0)
        throw std::runtime_error(_connected ? "Phenix-1255: request timeout"
                                            : "Phenix-1255: connection to the device lost");

    std::vector<uint8_t> resp = std::move(_responses[seq]);
    _responses.erase(seq);
    lock.unlock();

    if (resp.size() < sizeof(phx_resp_status))
        throw std::runtime_error("Phenix-1255: short response");
    phx_resp_status st;
    std::memcpy(&st, resp.data(), sizeof(st));
    if (st.status != 0)
        throw DeviceError("Phenix-1255: request 0x" + std::to_string(opcode) +
                              " failed with status " + std::to_string(st.status),
                          st.status);

    return std::vector<uint8_t>(resp.begin() + sizeof(st), resp.end());
}

void ControlClient::readerLoop()
{
    std::vector<uint8_t> msg;

    while (_running) {
        try {
            if (!_transport.recvControl(msg, 100))
                continue;
        } catch (const std::exception &e) {
            if (_running)
                SoapySDR::logf(SOAPY_SDR_ERROR, "Phenix-1255: %s", e.what());
            break;
        }

        phx_ctrl_hdr hdr;
        std::memcpy(&hdr, msg.data(), sizeof(hdr));
        const uint8_t *payload = msg.data() + sizeof(hdr);

        if (hdr.opcode == PHX_OP_EVENT) {
            phx_event ev{};
            std::memcpy(&ev, payload, std::min<size_t>(hdr.length, sizeof(ev)));
            if (ev.code == PHX_EVENT_STREAM_ERROR)
                SoapySDR::logf(SOAPY_SDR_ERROR,
                               "Phenix-1255: stream stopped by the device (error %d)",
                               ev.detail);
            else if (ev.code == PHX_EVENT_PA_SHUTDOWN)
                SoapySDR::log(SOAPY_SDR_ERROR, "Phenix-1255: PA switched off by the device");
            else
                SoapySDR::logf(SOAPY_SDR_WARNING, "Phenix-1255: device event %u", ev.code);
            continue;
        }

        if ((hdr.opcode & PHX_OP_RESPONSE) == 0)
            continue;

        std::lock_guard<std::mutex> lock(_mutex);
        if (_ignored.erase(hdr.seq) > 0)
            continue;
        _responses[hdr.seq].assign(payload, payload + hdr.length);
        _cond.notify_all();
    }

    _connected = false;
    _cond.notify_all();
}

/*
 * Keepalives are sent at a fixed interval without waiting for the response,
 * so that a delayed response cannot stall the keepalive and trigger the
 * device link timeout. Their responses are discarded by the reader.
 */
void ControlClient::keepaliveLoop()
{
    while (_running && _connected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(PHX_KEEPALIVE_INTERVAL_MS));
        if (!_running || !_connected)
            break;

        const uint16_t seq = nextSeq();
        {
            std::lock_guard<std::mutex> lock(_mutex);
            // Bound the set in case responses never arrive
            if (_ignored.size() > 64)
                _ignored.clear();
            _ignored[seq] = true;
        }
        try {
            send(PHX_OP_KEEPALIVE, seq, nullptr, 0);
        } catch (const std::exception &e) {
            if (_running)
                SoapySDR::logf(SOAPY_SDR_WARNING, "Phenix-1255: keepalive: %s", e.what());
        }
    }
}

} // namespace phx
