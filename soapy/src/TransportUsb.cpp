// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors

#include "TransportUsb.hpp"

#include <phenix1255/protocol.h>

#include <SoapySDR/Logger.hpp>

#include <libusb.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace phx {

namespace {

constexpr int INTERFACE = 0;

// Queued IN transfers keep the endpoints busy while the host processes data
constexpr size_t CTRL_IN_TRANSFERS = 4;
constexpr size_t CTRL_IN_BUFFER = 512;
constexpr size_t SAMPLE_IN_TRANSFERS = 16;
constexpr size_t SAMPLE_IN_BUFFER = 2048;
// About 20 ms of TX packets at 600 kHz in flight
constexpr size_t SAMPLE_OUT_TRANSFERS = 32;

// Received messages kept until they are read
constexpr size_t CTRL_QUEUE_MAX = 64;
constexpr size_t SAMPLE_QUEUE_MAX = 4096;

constexpr unsigned CTRL_OUT_TIMEOUT_MS = 1000;
constexpr int TX_SLOT_TIMEOUT_MS = 1000;
constexpr auto RELEASE_TIMEOUT = std::chrono::seconds(2);

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::runtime_error usbError(const std::string &what, int rc)
{
    return std::runtime_error("Phenix-1255 USB: " + what + ": " +
                              libusb_error_name(rc));
}

/*
 * 1209:000A is a shared test PID (firmware/src/usb/_doc/usb.md), so the interface layout is
 * checked as well: vendor specific interface 0 with the four bulk endpoints.
 */
bool isPhenix(libusb_device *dev)
{
    libusb_device_descriptor desc;
    if (libusb_get_device_descriptor(dev, &desc) != 0 || desc.idVendor != PHX_USB_VID ||
        desc.idProduct != PHX_USB_PID)
        return false;

    libusb_config_descriptor *config = nullptr;
    if (libusb_get_config_descriptor(dev, 0, &config) != 0)
        return false;

    bool match = false;
    if (config->bNumInterfaces > INTERFACE && config->interface[INTERFACE].num_altsetting > 0) {
        const libusb_interface_descriptor &intf = config->interface[INTERFACE].altsetting[0];
        unsigned found = 0;
        for (uint8_t i = 0; i < intf.bNumEndpoints; i++) {
            const libusb_endpoint_descriptor &ep = intf.endpoint[i];
            if ((ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                continue;
            for (const uint8_t addr : {PHX_USB_EP_CTRL_OUT, PHX_USB_EP_CTRL_IN,
                                       PHX_USB_EP_SAMPLE_OUT, PHX_USB_EP_SAMPLE_IN})
                if (ep.bEndpointAddress == addr)
                    found++;
        }
        match = intf.bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC && found == 4;
    }
    libusb_free_config_descriptor(config);
    return match;
}

std::string readSerial(libusb_device_handle *handle, libusb_device *dev)
{
    libusb_device_descriptor desc;
    unsigned char buf[128];

    if (libusb_get_device_descriptor(dev, &desc) != 0 || desc.iSerialNumber == 0)
        return "";
    const int n = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, buf,
                                                     sizeof(buf));
    return (n > 0) ? lower(std::string(reinterpret_cast<char *>(buf), static_cast<size_t>(n)))
                   : "";
}

} // namespace

std::vector<TransportUsb::DeviceInfo> TransportUsb::enumerate()
{
    std::vector<DeviceInfo> result;
    libusb_context *ctx = nullptr;

    if (libusb_init(&ctx) != 0)
        return result;

    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        if (!isPhenix(list[i]))
            continue;
        DeviceInfo info;
        info.bus = libusb_get_bus_number(list[i]);
        info.address = libusb_get_device_address(list[i]);
        // Reading the serial number needs access rights (udev rule on Linux),
        // but no claimed interface, so a device in use is listed as well.
        libusb_device_handle *handle = nullptr;
        if (libusb_open(list[i], &handle) == 0) {
            info.serial = readSerial(handle, list[i]);
            libusb_close(handle);
        }
        result.push_back(info);
    }
    if (n >= 0)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    return result;
}

TransportUsb::TransportUsb(const std::string &serial)
{
    try {
        open(lower(serial));
        startReaders();
    } catch (...) {
        release();
        throw;
    }

    _eventsRunning = true;
    _eventThread = std::thread(&TransportUsb::eventLoop, this);
}

TransportUsb::~TransportUsb() { release(); }

void TransportUsb::open(const std::string &serial)
{
    int rc = libusb_init(&_ctx);
    if (rc != 0) {
        _ctx = nullptr;
        throw usbError("init", rc);
    }

    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(_ctx, &list);
    libusb_device *dev = nullptr;
    for (ssize_t i = 0; i < n && _handle == nullptr; i++) {
        if (!isPhenix(list[i]))
            continue;
        libusb_device_handle *handle = nullptr;
        if (libusb_open(list[i], &handle) != 0)
            continue;
        const std::string s = readSerial(handle, list[i]);
        if (serial.empty() || s == serial) {
            _handle = handle;
            _serial = s;
            dev = list[i];
        } else {
            libusb_close(handle);
        }
    }

    if (_handle != nullptr)
        _mps = static_cast<size_t>(
            std::max(libusb_get_max_packet_size(dev, PHX_USB_EP_SAMPLE_OUT), 1));
    if (n >= 0)
        libusb_free_device_list(list, 1);
    if (_handle == nullptr)
        throw std::runtime_error("Phenix-1255 USB: no accessible device" +
                                 (serial.empty() ? std::string() : " with serial " + serial));

    // Linux: detach a kernel driver if one is bound (not expected for a
    // vendor specific interface)
    (void)libusb_set_auto_detach_kernel_driver(_handle, 1);

    int config = 0;
    if (libusb_get_configuration(_handle, &config) == 0 && config != 1) {
        rc = libusb_set_configuration(_handle, 1);
        if (rc != 0)
            throw usbError("set configuration", rc);
    }

    rc = libusb_claim_interface(_handle, INTERFACE);
    if (rc != 0)
        throw usbError("claim interface (device in use by another program?)", rc);
    _claimed = true;
}

void TransportUsb::startReaders()
{
    auto setup = [this](std::vector<Slot> &slots, size_t count, size_t size) {
        slots.resize(count);
        for (Slot &s : slots) {
            s.owner = this;
            s.buffer.resize(size);
            s.transfer = libusb_alloc_transfer(0);
            if (s.transfer == nullptr)
                throw std::runtime_error("Phenix-1255 USB: out of memory");
        }
    };

    setup(_ctrlIn, CTRL_IN_TRANSFERS, CTRL_IN_BUFFER);
    setup(_sampleIn, SAMPLE_IN_TRANSFERS, SAMPLE_IN_BUFFER);
    setup(_sampleOut, SAMPLE_OUT_TRANSFERS, PHX_MAX_SAMPLE_PACKET_BYTES);

    auto submit = [this](std::vector<Slot> &slots, unsigned char ep) {
        for (Slot &s : slots) {
            libusb_fill_bulk_transfer(s.transfer, _handle, ep, s.buffer.data(),
                                      static_cast<int>(s.buffer.size()), &inCallback, &s, 0);
            const int rc = libusb_submit_transfer(s.transfer);
            if (rc != 0)
                throw usbError("submit", rc);
            s.busy = true;
            std::lock_guard<std::mutex> lock(_pendingMutex);
            _pending++;
        }
    };

    submit(_ctrlIn, PHX_USB_EP_CTRL_IN);
    submit(_sampleIn, PHX_USB_EP_SAMPLE_IN);
}

void TransportUsb::eventLoop()
{
    while (_eventsRunning) {
        timeval tv{0, 100000};
        const int rc = libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
        if (rc != 0 && rc != LIBUSB_ERROR_INTERRUPTED && rc != LIBUSB_ERROR_TIMEOUT) {
            markLost("event handling", rc);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void TransportUsb::markLost(const char *what, int status)
{
    if (!_lost.exchange(true) && !_shutdown)
        SoapySDR::logf(SOAPY_SDR_ERROR, "Phenix-1255 USB: %s failed (%d), link lost", what,
                       status);
    _ctrlQueue.cond.notify_all();
    _sampleQueue.cond.notify_all();
    _txCond.notify_all();
}

void TransportUsb::inCallback(libusb_transfer *t)
{
    Slot *slot = static_cast<Slot *>(t->user_data);
    TransportUsb *self = slot->owner;
    bool resubmit = false;

    switch (t->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        if (t->actual_length > 0) {
            Queue &q = (t->endpoint == PHX_USB_EP_CTRL_IN) ? self->_ctrlQueue
                                                           : self->_sampleQueue;
            const size_t max = (&q == &self->_ctrlQueue) ? CTRL_QUEUE_MAX : SAMPLE_QUEUE_MAX;
            std::lock_guard<std::mutex> lock(q.mutex);
            if (q.items.size() >= max)
                q.items.pop_front();
            q.items.emplace_back(t->buffer, t->buffer + t->actual_length);
            q.cond.notify_one();
        }
        resubmit = true;
        break;
    case LIBUSB_TRANSFER_TIMED_OUT:
    case LIBUSB_TRANSFER_OVERFLOW:
        resubmit = true;
        break;
    case LIBUSB_TRANSFER_CANCELLED:
        break;
    default:
        self->markLost("IN transfer", t->status);
        break;
    }

    if (resubmit && !self->_shutdown && !self->_lost) {
        const int rc = libusb_submit_transfer(t);
        if (rc == 0)
            return;
        self->markLost("resubmit", rc);
    }

    std::lock_guard<std::mutex> lock(self->_pendingMutex);
    slot->busy = false;
    self->_pending--;
    self->_pendingCond.notify_all();
}

void TransportUsb::outCallback(libusb_transfer *t)
{
    Slot *slot = static_cast<Slot *>(t->user_data);
    TransportUsb *self = slot->owner;

    // A failed TX packet shows up in the device counters
    if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
        self->markLost("OUT transfer", t->status);

    {
        std::lock_guard<std::mutex> lock(self->_txMutex);
        slot->busy = false;
    }
    self->_txCond.notify_one();

    std::lock_guard<std::mutex> lock(self->_pendingMutex);
    self->_pending--;
    self->_pendingCond.notify_all();
}

void TransportUsb::bulkOutSync(unsigned char ep, const uint8_t *data, size_t len)
{
    if (_lost)
        throw std::runtime_error("Phenix-1255 USB: device disconnected");

    int done = 0;
    int rc = libusb_bulk_transfer(_handle, ep, const_cast<uint8_t *>(data),
                                  static_cast<int>(len), &done, CTRL_OUT_TIMEOUT_MS);
    if (rc == 0 && len % _mps == 0) {
        // Terminate a transfer of whole packets
        uint8_t dummy = 0;
        rc = libusb_bulk_transfer(_handle, ep, &dummy, 0, &done, CTRL_OUT_TIMEOUT_MS);
    }
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_NO_DEVICE)
            markLost("control OUT", rc);
        throw usbError("control send", rc);
    }
}

void TransportUsb::sendControl(const uint8_t *msg, size_t len)
{
    std::lock_guard<std::mutex> lock(_ctrlOutMutex);
    bulkOutSync(PHX_USB_EP_CTRL_OUT, msg, len);
}

bool TransportUsb::recvControl(std::vector<uint8_t> &msg, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_ctrlQueue.mutex);
    _ctrlQueue.cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return !_ctrlQueue.items.empty() || _lost || _shutdown;
    });
    if (_ctrlQueue.items.empty()) {
        if (_lost && !_shutdown)
            throw std::runtime_error("USB device disconnected");
        return false;
    }

    msg = std::move(_ctrlQueue.items.front());
    _ctrlQueue.items.pop_front();
    lock.unlock();

    phx_ctrl_hdr hdr;
    if (msg.size() < sizeof(hdr))
        throw std::runtime_error("short control message");
    std::memcpy(&hdr, msg.data(), sizeof(hdr));
    if (hdr.magic != PHX_CTRL_MAGIC || hdr.length > PHX_CTRL_MAX_PAYLOAD ||
        msg.size() < sizeof(hdr) + hdr.length)
        throw std::runtime_error("invalid control message");
    msg.resize(sizeof(hdr) + hdr.length);
    return true;
}

TransportUsb::Slot *TransportUsb::acquireTxSlot(int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_txMutex);
    Slot *free = nullptr;
    _txCond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        for (Slot &s : _sampleOut)
            if (!s.busy) {
                free = &s;
                return true;
            }
        return _lost || _shutdown;
    });
    if (free != nullptr)
        free->busy = true;
    return free;
}

void TransportUsb::submitTx(Slot *slot, const uint8_t *data, size_t len)
{
    if (len > 0)
        std::memcpy(slot->buffer.data(), data, len);
    libusb_fill_bulk_transfer(slot->transfer, _handle, PHX_USB_EP_SAMPLE_OUT,
                              slot->buffer.data(), static_cast<int>(len), &outCallback, slot, 0);
    {
        std::lock_guard<std::mutex> lock(_pendingMutex);
        _pending++;
    }
    const int rc = libusb_submit_transfer(slot->transfer);
    if (rc != 0) {
        {
            std::lock_guard<std::mutex> lock(_txMutex);
            slot->busy = false;
        }
        {
            std::lock_guard<std::mutex> lock(_pendingMutex);
            _pending--;
        }
        markLost("submit OUT", rc);
    }
}

void TransportUsb::sendSamples(const uint8_t *pkt, size_t len)
{
    if (_lost || _shutdown || len > PHX_MAX_SAMPLE_PACKET_BYTES)
        return;

    Slot *slot = acquireTxSlot(TX_SLOT_TIMEOUT_MS);
    if (slot == nullptr)
        return;
    submitTx(slot, pkt, len);

    if (len % _mps == 0) {
        // Terminate a transfer of whole packets
        slot = acquireTxSlot(TX_SLOT_TIMEOUT_MS);
        if (slot != nullptr)
            submitTx(slot, nullptr, 0);
    }
}

size_t TransportUsb::recvSamples(uint8_t *buf, size_t maxLen, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(_sampleQueue.mutex);
    _sampleQueue.cond.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return !_sampleQueue.items.empty() || _lost || _shutdown;
    });
    if (_sampleQueue.items.empty())
        return 0;

    std::vector<uint8_t> pkt = std::move(_sampleQueue.items.front());
    _sampleQueue.items.pop_front();
    lock.unlock();

    const size_t n = std::min(pkt.size(), maxLen);
    std::memcpy(buf, pkt.data(), n);
    return n;
}

void TransportUsb::shutdown()
{
    _shutdown = true;
    _ctrlQueue.cond.notify_all();
    _sampleQueue.cond.notify_all();
    _txCond.notify_all();
}

void TransportUsb::cancelAll()
{
    for (auto *slots : {&_ctrlIn, &_sampleIn, &_sampleOut})
        for (Slot &s : *slots)
            if (s.transfer != nullptr)
                (void)libusb_cancel_transfer(s.transfer);
}

void TransportUsb::release()
{
    shutdown();

    if (_handle != nullptr) {
        cancelAll();

        // Let the callbacks of the cancelled transfers run
        std::unique_lock<std::mutex> lock(_pendingMutex);
        const auto deadline = std::chrono::steady_clock::now() + RELEASE_TIMEOUT;
        while (_pending > 0 && std::chrono::steady_clock::now() < deadline) {
            if (_eventsRunning) {
                _pendingCond.wait_until(lock, deadline);
            } else {
                lock.unlock();
                timeval tv{0, 50000};
                libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
                lock.lock();
            }
        }
        if (_pending > 0)
            SoapySDR::logf(SOAPY_SDR_WARNING, "Phenix-1255 USB: %d transfers did not finish",
                           _pending);
    }

    if (_eventThread.joinable()) {
        _eventsRunning = false;
        libusb_interrupt_event_handler(_ctx);
        _eventThread.join();
    }

    // Transfers that never completed are leaked rather than freed while in use
    if (_pending == 0)
        for (auto *slots : {&_ctrlIn, &_sampleIn, &_sampleOut})
            for (Slot &s : *slots)
                if (s.transfer != nullptr) {
                    libusb_free_transfer(s.transfer);
                    s.transfer = nullptr;
                }

    if (_claimed) {
        libusb_release_interface(_handle, INTERFACE);
        _claimed = false;
    }
    if (_handle != nullptr) {
        libusb_close(_handle);
        _handle = nullptr;
    }
    if (_ctx != nullptr && _pending == 0) {
        libusb_exit(_ctx);
        _ctx = nullptr;
    }
}

} // namespace phx
