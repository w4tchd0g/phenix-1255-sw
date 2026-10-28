// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors
//
// SoapySDR device for the Phenix-1255 (SX1255 on FRDM-MCXN947).
// Gains, antennas and sample rates follow SoapySX, so that application
// settings for SXceiver/uCell apply unchanged.

#pragma once

#include "ControlClient.hpp"
#include "SampleStream.hpp"
#include "Transport.hpp"

#include <SoapySDR/Device.hpp>

#include <map>
#include <memory>
#include <mutex>

namespace phx {

constexpr const char *DRIVER_KEY = "phenix-1255";

class Phenix1255Device : public SoapySDR::Device
{
public:
    Phenix1255Device(std::unique_ptr<Transport> transport, const SoapySDR::Kwargs &args);
    ~Phenix1255Device() override;

    // Identification
    std::string getDriverKey() const override { return DRIVER_KEY; }
    std::string getHardwareKey() const override { return DRIVER_KEY; }
    SoapySDR::Kwargs getHardwareInfo() const override;

    // Channels
    size_t getNumChannels(const int direction) const override;
    bool getFullDuplex(const int direction, const size_t channel) const override;

    // Streams
    std::vector<std::string> getStreamFormats(const int direction,
                                              const size_t channel) const override;
    std::string getNativeStreamFormat(const int direction, const size_t channel,
                                      double &fullScale) const override;
    SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                  const std::vector<size_t> &channels,
                                  const SoapySDR::Kwargs &args) override;
    void closeStream(SoapySDR::Stream *stream) override;
    size_t getStreamMTU(SoapySDR::Stream *stream) const override;
    int activateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs,
                       const size_t numElems) override;
    int deactivateStream(SoapySDR::Stream *stream, const int flags,
                         const long long timeNs) override;
    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems,
                   int &flags, long long &timeNs, const long timeoutUs) override;
    int writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t numElems,
                    int &flags, const long long timeNs, const long timeoutUs) override;

    // Antennas
    std::vector<std::string> listAntennas(const int direction,
                                          const size_t channel) const override;
    void setAntenna(const int direction, const size_t channel,
                    const std::string &name) override;
    std::string getAntenna(const int direction, const size_t channel) const override;

    // Gains
    std::vector<std::string> listGains(const int direction,
                                       const size_t channel) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel,
                                 const std::string &name) const override;
    void setGain(const int direction, const size_t channel, const double value) override;
    void setGain(const int direction, const size_t channel, const std::string &name,
                 const double value) override;
    double getGain(const int direction, const size_t channel,
                   const std::string &name) const override;

    // Frequency
    void setFrequency(const int direction, const size_t channel, const double frequency,
                      const SoapySDR::Kwargs &args) override;
    double getFrequency(const int direction, const size_t channel) const override;
    SoapySDR::RangeList getFrequencyRange(const int direction,
                                          const size_t channel) const override;

    // Sample rate
    std::vector<double> listSampleRates(const int direction,
                                        const size_t channel) const override;
    SoapySDR::RangeList getSampleRateRange(const int direction,
                                           const size_t channel) const override;
    void setSampleRate(const int direction, const size_t channel, const double rate) override;
    double getSampleRate(const int direction, const size_t channel) const override;

    // Time
    bool hasHardwareTime(const std::string &what) const override;
    long long getHardwareTime(const std::string &what) const override;

    // Registers
    void writeRegister(const unsigned addr, const unsigned value) override;
    unsigned readRegister(const unsigned addr) const override;

    // Settings
    SoapySDR::ArgInfoList getSettingInfo() const override;
    void writeSetting(const std::string &key, const std::string &value) override;
    std::string readSetting(const std::string &key) const override;

private:
    struct StreamHandle {
        int direction;
        SampleFormat format;
        bool active = false;
    };

    std::vector<uint8_t> request(uint8_t opcode, const void *payload = nullptr,
                                 size_t len = 0) const;
    void applyFrequency(int direction, double frequency);
    void applyGain(int element, double value);
    void setTxOutput(bool enable);

    std::unique_ptr<Transport> _transport;
    std::unique_ptr<ControlClient> _control;
    std::unique_ptr<SampleStream> _samples;

    // Device information from HELLO
    std::string _fwVersion;
    std::string _serial;
    uint32_t _xtalHz = 0;
    double _freqMinHz = 0;
    double _freqMaxHz = 0;
    uint32_t _txBufferSamples = 0;
    std::vector<double> _rates;

    // Cached settings (the device does not report them back)
    mutable std::recursive_mutex _mutex;
    double _rate = 0;
    double _freq[2] = {0, 0};
    double _gain[4] = {0, 0, 0, 0};
    bool _rxLoopback = false;
    bool _txOutput = true;
    StreamHandle *_streams[2] = {nullptr, nullptr};
};

} // namespace phx
