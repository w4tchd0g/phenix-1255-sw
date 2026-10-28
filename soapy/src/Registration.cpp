// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// Device discovery and registration of the "phenix-1255" driver.
//
// Device arguments:
//   driver=phenix-1255
//   transport=usb|eth         default: USB if a device is attached, else Ethernet
//   addr=<host or IPv4>       Ethernet, default phenix-1255.local
//   ctrl_port=<port>          Ethernet, default 1255
//   sample_port=<port>        Ethernet, default 1256
//   serial=<hex>              select a device by serial number
//   lead_ms=<ms>              TX lead time, default 20 ms
//
// USB devices are always listed (enumeration is fast). Network discovery
// runs only when the driver is requested explicitly or an address is given,
// so that a generic enumeration of all SoapySDR modules is not delayed by
// mDNS timeouts.

#include "Phenix1255Device.hpp"
#include "TransportNet.hpp"
#ifdef PHX_HAVE_USB
#include "TransportUsb.hpp"
#endif

#include <phenix1255/protocol.h>

#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace {

constexpr int PROBE_TIMEOUT_MS = 1000;

std::string arg(const SoapySDR::Kwargs &args, const char *key, const std::string &def)
{
    const auto it = args.find(key);
    return (it != args.end()) ? it->second : def;
}

uint16_t portArg(const SoapySDR::Kwargs &args, const char *key, uint16_t def)
{
    return static_cast<uint16_t>(std::stoul(arg(args, key, std::to_string(def))));
}

/// Connect, read the device information and disconnect again.
bool probeNet(const SoapySDR::Kwargs &args, SoapySDR::Kwargs &result)
{
    const std::string host = arg(args, "addr", std::string(PHX_MDNS_HOSTNAME) + ".local");
    const uint16_t ctrlPort = portArg(args, "ctrl_port", PHX_DEFAULT_CTRL_PORT);
    const uint16_t samplePort = portArg(args, "sample_port", PHX_DEFAULT_SAMPLE_PORT);

    try {
        phx::TransportNet net(host, ctrlPort, samplePort, PROBE_TIMEOUT_MS);
        phx::ControlClient control(net);
        control.start();

        std::string serial;
        try {
            const phx_hello_req hello{PHX_PROTO_VERSION, 0};
            const auto resp = control.request(PHX_OP_HELLO, &hello, sizeof(hello),
                                              PROBE_TIMEOUT_MS);
            if (resp.size() >= sizeof(phx_info)) {
                phx_info info;
                std::memcpy(&info, resp.data(), sizeof(info));
                serial.assign(info.serial, strnlen(info.serial, sizeof(info.serial)));
            }
        } catch (const std::exception &) {
            // Connected but no answer: another host holds the session
        }
        net.shutdown();
        control.stop();

        if (args.count("serial") && !serial.empty() && args.at("serial") != serial)
            return false;

        result["driver"] = phx::DRIVER_KEY;
        result["transport"] = "eth";
        result["addr"] = net.address();
        if (ctrlPort != PHX_DEFAULT_CTRL_PORT)
            result["ctrl_port"] = std::to_string(ctrlPort);
        if (samplePort != PHX_DEFAULT_SAMPLE_PORT)
            result["sample_port"] = std::to_string(samplePort);
        if (!serial.empty())
            result["serial"] = serial;
        result["label"] = "Phenix-1255 [" + net.address() +
                          (serial.empty() ? std::string(" busy") : " " + serial) + "]";
        return true;
    } catch (const std::exception &e) {
        SoapySDR::logf(SOAPY_SDR_DEBUG, "Phenix-1255: no device at %s: %s", host.c_str(),
                       e.what());
        return false;
    }
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

#ifdef PHX_HAVE_USB
/// List attached USB devices without opening a session.
void findUsb(const SoapySDR::Kwargs &args, SoapySDR::KwargsList &results)
{
    const std::string wanted = lower(arg(args, "serial", ""));

    for (const auto &dev : phx::TransportUsb::enumerate()) {
        if (!wanted.empty() && dev.serial != wanted)
            continue;
        SoapySDR::Kwargs result;
        result["driver"] = phx::DRIVER_KEY;
        result["transport"] = "usb";
        if (!dev.serial.empty())
            result["serial"] = dev.serial;
        result["label"] = "Phenix-1255 [USB " +
                          (dev.serial.empty() ? std::string("no access") : dev.serial) + "]";
        results.push_back(result);
    }
}
#endif

SoapySDR::KwargsList findPhenix(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList results;

    const bool explicitDriver = args.count("driver") > 0;
    if (explicitDriver && args.at("driver") != phx::DRIVER_KEY)
        return results;

    const std::string transport = arg(args, "transport", "");
#ifdef PHX_HAVE_USB
    if (transport == "usb" || (transport.empty() && !args.count("addr")))
        findUsb(args, results);
#endif
    if ((transport.empty() || transport == "eth") && (explicitDriver || args.count("addr"))) {
        SoapySDR::Kwargs dev;
        if (probeNet(args, dev))
            results.push_back(dev);
    }

    return results;
}

SoapySDR::Device *makePhenix(const SoapySDR::Kwargs &args)
{
    std::string kind = arg(args, "transport", "");
#ifdef PHX_HAVE_USB
    // Without an explicit transport, an attached USB device is preferred
    if (kind.empty() && !args.count("addr") && !phx::TransportUsb::enumerate().empty())
        kind = "usb";
#endif
    if (kind.empty())
        kind = "eth";

    std::unique_ptr<phx::Transport> transport;
    if (kind == "usb") {
#ifdef PHX_HAVE_USB
        transport.reset(new phx::TransportUsb(arg(args, "serial", "")));
#else
        throw std::runtime_error("Phenix-1255: module built without USB support");
#endif
    } else if (kind == "eth") {
        const std::string host =
            arg(args, "addr", std::string(PHX_MDNS_HOSTNAME) + ".local");
        transport.reset(
            new phx::TransportNet(host, portArg(args, "ctrl_port", PHX_DEFAULT_CTRL_PORT),
                                  portArg(args, "sample_port", PHX_DEFAULT_SAMPLE_PORT)));
    } else {
        throw std::invalid_argument("Phenix-1255: unknown transport " + kind);
    }

    return new phx::Phenix1255Device(std::move(transport), args);
}

const SoapySDR::Registry registration(phx::DRIVER_KEY, &findPhenix, &makePhenix,
                                      SOAPY_SDR_ABI_VERSION);

} // namespace
