// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// HS USB transport (libusb-1.0): vendor specific interface 0 with bulk
// endpoints 0x01/0x81 (control) and 0x02/0x82 (samples). Every control
// message and every sample packet is one bulk transfer; a transfer whose
// length is a multiple of the packet size is terminated by a zero-length
// packet. See firmware/src/usb/_doc/usb.md.

#pragma once

#include "Transport.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace phx {

class TransportUsb : public Transport
{
public:
    struct DeviceInfo
    {
        std::string serial;  ///< Lower case hex, empty if the device could not be opened
        uint8_t bus = 0;
        uint8_t address = 0;
    };

    /// List attached devices with the Phenix-1255 VID:PID.
    static std::vector<DeviceInfo> enumerate();

    /// Open the device with the given serial number (case insensitive), or
    /// the first device if serial is empty. Throws on failure.
    explicit TransportUsb(const std::string &serial);
    ~TransportUsb() override;

    void sendControl(const uint8_t *msg, size_t len) override;
    bool recvControl(std::vector<uint8_t> &msg, int timeoutMs) override;
    void sendSamples(const uint8_t *pkt, size_t len) override;
    size_t recvSamples(uint8_t *buf, size_t maxLen, int timeoutMs) override;
    uint16_t rxPort() const override { return 0; }
    std::string description() const override { return "usb " + _serial; }
    void shutdown() override;

    const std::string &serial() const { return _serial; }

private:
    struct Slot
    {
        TransportUsb *owner = nullptr;
        libusb_transfer *transfer = nullptr;
        std::vector<uint8_t> buffer;
        bool busy = false;
    };

    struct Queue
    {
        std::mutex mutex;
        std::condition_variable cond;
        std::deque<std::vector<uint8_t>> items;
    };

    static void inCallback(libusb_transfer *transfer);
    static void outCallback(libusb_transfer *transfer);

    void open(const std::string &serial);
    void drain(unsigned char ep);
    void startReaders();
    void eventLoop();
    void markLost(const char *what, int status);
    void bulkOutSync(unsigned char ep, const uint8_t *data, size_t len);
    Slot *acquireTxSlot(int timeoutMs);
    void submitTx(Slot *slot, const uint8_t *data, size_t len);
    void cancelAll();
    void release();

    libusb_context *_ctx = nullptr;
    libusb_device_handle *_handle = nullptr;
    bool _claimed = false;
    std::string _serial;
    size_t _mps = 512;

    std::thread _eventThread;
    std::atomic<bool> _eventsRunning{false};
    std::atomic<bool> _shutdown{false};
    std::atomic<bool> _lost{false};

    std::vector<Slot> _ctrlIn;
    std::vector<Slot> _sampleIn;
    std::vector<Slot> _sampleOut;
    std::mutex _txMutex;
    std::condition_variable _txCond;
    std::mutex _ctrlOutMutex;

    // Transfers submitted and not yet completed, guarded by _pendingMutex
    std::mutex _pendingMutex;
    std::condition_variable _pendingCond;
    int _pending = 0;

    Queue _ctrlQueue;
    Queue _sampleQueue;
};

} // namespace phx
