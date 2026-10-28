// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Phenix-1255 contributors

#include "Phenix1255Device.hpp"

#include <phenix1255/protocol.h>

#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace phx {

namespace {

// Defaults applied at open (SX1255 datasheet defaults, as SoapySX)
constexpr double DEFAULT_FREQ = 433.92e6;
constexpr double DEFAULT_RATE = 600e3;
constexpr double DEFAULT_GAIN[4] = {48.0, 30.0, 6.0, 28.0}; // LNA, PGA, DAC, MIXER
constexpr double DEFAULT_LEAD_MS = 20.0;

const char *const GAIN_NAMES[4] = {"LNA", "PGA", "DAC", "MIXER"};
const SoapySDR::Range GAIN_RANGES[4] = {
    {0.0, 48.0, 6.0}, // LNA
    {0.0, 30.0, 2.0}, // PGA
    {0.0, 9.0, 3.0},  // DAC
    {0.0, 30.0, 2.0}, // MIXER
};

// Overall gain distribution as in SoapySX: the fine-step element is kept
// around a target value while the coarse element covers the range.
constexpr double RX_PGA_TARGET = 12.0;
constexpr double TX_MIXER_TARGET = 26.0;

int gainIndex(int direction, const std::string &name)
{
    const int first = (direction == SOAPY_SDR_RX) ? PHX_GAIN_LNA : PHX_GAIN_DAC;
    for (int i = first; i < first + 2; i++)
        if (name == GAIN_NAMES[i])
            return i;
    throw std::invalid_argument("Phenix-1255: unknown gain element " + name);
}

void checkChannel(size_t channel)
{
    if (channel != 0)
        throw std::invalid_argument("Phenix-1255: only channel 0 exists");
}

} // namespace

Phenix1255Device::Phenix1255Device(std::unique_ptr<Transport> transport,
                                   const SoapySDR::Kwargs &args)
    : _transport(std::move(transport))
{
    _control.reset(new ControlClient(*_transport));
    _control->start();

    const phx_hello_req hello{PHX_PROTO_VERSION, _transport->rxPort()};
    const auto resp = request(PHX_OP_HELLO, &hello, sizeof(hello));
    if (resp.size() < sizeof(phx_info))
        throw std::runtime_error("Phenix-1255: short HELLO response");

    phx_info info;
    std::memcpy(&info, resp.data(), sizeof(info));
    _fwVersion.assign(info.fw_version, strnlen(info.fw_version, sizeof(info.fw_version)));
    _serial.assign(info.serial, strnlen(info.serial, sizeof(info.serial)));
    _xtalHz = info.xtal_hz;
    _freqMinHz = info.freq_min_hz;
    _freqMaxHz = info.freq_max_hz;
    _txBufferSamples = info.tx_buffer_samples;
    for (unsigned i = 0; i < info.num_rates && i < PHX_MAX_RATES; i++)
        _rates.push_back(info.rates[i]);

    SoapySDR::logf(SOAPY_SDR_INFO, "Phenix-1255: %s, firmware %s, serial %s",
                   _transport->description().c_str(), _fwVersion.c_str(), _serial.c_str());

    _samples.reset(new SampleStream(*_transport));

    double leadMs = DEFAULT_LEAD_MS;
    if (args.count("lead_ms"))
        leadMs = std::stod(args.at("lead_ms"));
    _samples->setLeadTime(leadMs / 1000.0);

    // Bring the device into a known state
    setSampleRate(SOAPY_SDR_RX, 0, DEFAULT_RATE);
    applyFrequency(SOAPY_SDR_RX, DEFAULT_FREQ);
    applyFrequency(SOAPY_SDR_TX, DEFAULT_FREQ);
    for (int i = 0; i < 4; i++)
        applyGain(i, DEFAULT_GAIN[i]);
    setAntenna(SOAPY_SDR_RX, 0, "RX");
    setTxOutput(true);
}

Phenix1255Device::~Phenix1255Device()
{
    // TX first: PA off before anything else
    for (const uint8_t op : {static_cast<uint8_t>(PHX_OP_STREAM_TX),
                             static_cast<uint8_t>(PHX_OP_STREAM_RX)}) {
        try {
            const phx_u8_req req{0, {0, 0, 0}};
            request(op, &req, sizeof(req));
        } catch (const std::exception &) {
        }
    }
    _samples.reset();
    _transport->shutdown();
    _control->stop();
}

std::vector<uint8_t> Phenix1255Device::request(uint8_t opcode, const void *payload,
                                               size_t len) const
{
    return _control->request(opcode, payload, len);
}

SoapySDR::Kwargs Phenix1255Device::getHardwareInfo() const
{
    SoapySDR::Kwargs info;
    info["firmware"] = _fwVersion;
    info["serial"] = _serial;
    info["transport"] = _transport->description();
    info["protocol_version"] = std::to_string(PHX_PROTO_VERSION);
    info["xtal_hz"] = std::to_string(_xtalHz);
    info["tx_buffer_samples"] = std::to_string(_txBufferSamples);
    return info;
}

size_t Phenix1255Device::getNumChannels(const int) const { return 1; }

bool Phenix1255Device::getFullDuplex(const int, const size_t) const { return true; }

/*******************************************************************
 * Streams
 ******************************************************************/

std::vector<std::string> Phenix1255Device::getStreamFormats(const int, const size_t) const
{
    return {SOAPY_SDR_CF32, SOAPY_SDR_CS16};
}

std::string Phenix1255Device::getNativeStreamFormat(const int, const size_t,
                                                    double &fullScale) const
{
    fullScale = 32767;
    return SOAPY_SDR_CS16;
}

SoapySDR::Stream *Phenix1255Device::setupStream(const int direction, const std::string &format,
                                                const std::vector<size_t> &channels,
                                                const SoapySDR::Kwargs &)
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0))
        throw std::invalid_argument("Phenix-1255: only channel 0 exists");
    if (direction != SOAPY_SDR_RX && direction != SOAPY_SDR_TX)
        throw std::invalid_argument("Phenix-1255: invalid direction");
    if (_streams[direction] != nullptr)
        throw std::runtime_error("Phenix-1255: stream already set up");

    SampleFormat fmt;
    if (format == SOAPY_SDR_CF32)
        fmt = SampleFormat::CF32;
    else if (format == SOAPY_SDR_CS16)
        fmt = SampleFormat::CS16;
    else
        throw std::invalid_argument("Phenix-1255: unsupported format " + format);

    // Stream arguments of other SX1255 drivers (e.g. "period") are ignored
    _streams[direction] = new StreamHandle{direction, fmt};
    return reinterpret_cast<SoapySDR::Stream *>(_streams[direction]);
}

void Phenix1255Device::closeStream(SoapySDR::Stream *stream)
{
    auto *s = reinterpret_cast<StreamHandle *>(stream);
    if (s->active)
        deactivateStream(stream, 0, 0);

    std::lock_guard<std::recursive_mutex> lock(_mutex);
    _streams[s->direction] = nullptr;
    delete s;
}

size_t Phenix1255Device::getStreamMTU(SoapySDR::Stream *) const
{
    return PHX_MAX_PACKET_SAMPLES;
}

int Phenix1255Device::activateStream(SoapySDR::Stream *stream, const int flags,
                                     const long long, const size_t)
{
    auto *s = reinterpret_cast<StreamHandle *>(stream);
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (flags & SOAPY_SDR_HAS_TIME)
        SoapySDR::log(SOAPY_SDR_WARNING,
                      "Phenix-1255: timed stream activation not supported, starting now");
    if (s->active)
        return 0;

    const phx_u8_req req{1, {0, 0, 0}};
    try {
        if (s->direction == SOAPY_SDR_RX) {
            _samples->rxStart();
            request(PHX_OP_STREAM_RX, &req, sizeof(req));
        } else {
            _samples->txStart();
            request(PHX_OP_STREAM_TX, &req, sizeof(req));
        }
    } catch (const std::exception &e) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "Phenix-1255: activateStream: %s", e.what());
        return SOAPY_SDR_STREAM_ERROR;
    }

    s->active = true;
    return 0;
}

int Phenix1255Device::deactivateStream(SoapySDR::Stream *stream, const int, const long long)
{
    auto *s = reinterpret_cast<StreamHandle *>(stream);
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (!s->active)
        return 0;
    s->active = false;

    const phx_u8_req req{0, {0, 0, 0}};
    try {
        if (s->direction == SOAPY_SDR_RX) {
            request(PHX_OP_STREAM_RX, &req, sizeof(req));
            _samples->rxStop();
        } else {
            request(PHX_OP_STREAM_TX, &req, sizeof(req));
            _samples->txStop();
        }
    } catch (const std::exception &e) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "Phenix-1255: deactivateStream: %s", e.what());
        return SOAPY_SDR_STREAM_ERROR;
    }
    return 0;
}

int Phenix1255Device::readStream(SoapySDR::Stream *stream, void *const *buffs,
                                 const size_t numElems, int &flags, long long &timeNs,
                                 const long timeoutUs)
{
    auto *s = reinterpret_cast<StreamHandle *>(stream);
    if (s->direction != SOAPY_SDR_RX)
        return SOAPY_SDR_NOT_SUPPORTED;
    if (!_control->connected())
        return SOAPY_SDR_STREAM_ERROR;
    return _samples->read(buffs[0], s->format, numElems, flags, timeNs, timeoutUs);
}

int Phenix1255Device::writeStream(SoapySDR::Stream *stream, const void *const *buffs,
                                  const size_t numElems, int &flags, const long long timeNs,
                                  const long timeoutUs)
{
    auto *s = reinterpret_cast<StreamHandle *>(stream);
    if (s->direction != SOAPY_SDR_TX)
        return SOAPY_SDR_NOT_SUPPORTED;
    if (!_control->connected())
        return SOAPY_SDR_STREAM_ERROR;
    return _samples->write(buffs[0], s->format, numElems, flags, timeNs, timeoutUs);
}

/*******************************************************************
 * Antennas
 ******************************************************************/

std::vector<std::string> Phenix1255Device::listAntennas(const int direction,
                                                        const size_t) const
{
    if (direction == SOAPY_SDR_RX)
        return {"RX", "LB"};
    return {"TX", "NONE"};
}

void Phenix1255Device::setAntenna(const int direction, const size_t channel,
                                  const std::string &name)
{
    checkChannel(channel);
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (direction == SOAPY_SDR_RX) {
        if (name != "RX" && name != "LB")
            throw std::invalid_argument("Phenix-1255: unknown RX antenna " + name);
        const phx_u8_req req{static_cast<uint8_t>(name == "LB" ? PHX_RX_INPUT_LOOPBACK
                                                               : PHX_RX_INPUT_ANTENNA),
                             {0, 0, 0}};
        request(PHX_OP_SET_RX_INPUT, &req, sizeof(req));
        _rxLoopback = (name == "LB");
    } else {
        if (name != "TX" && name != "NONE")
            throw std::invalid_argument("Phenix-1255: unknown TX antenna " + name);
        setTxOutput(name == "TX");
    }
}

std::string Phenix1255Device::getAntenna(const int direction, const size_t) const
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    if (direction == SOAPY_SDR_RX)
        return _rxLoopback ? "LB" : "RX";
    return _txOutput ? "TX" : "NONE";
}

void Phenix1255Device::setTxOutput(bool enable)
{
    const phx_u8_req req{static_cast<uint8_t>(enable ? 1 : 0), {0, 0, 0}};
    request(PHX_OP_SET_TX_OUTPUT, &req, sizeof(req));
    _txOutput = enable;
}

/*******************************************************************
 * Gains
 ******************************************************************/

std::vector<std::string> Phenix1255Device::listGains(const int direction, const size_t) const
{
    if (direction == SOAPY_SDR_RX)
        return {"LNA", "PGA"};
    return {"DAC", "MIXER"};
}

SoapySDR::Range Phenix1255Device::getGainRange(const int direction, const size_t) const
{
    if (direction == SOAPY_SDR_RX)
        return SoapySDR::Range(0.0, GAIN_RANGES[PHX_GAIN_LNA].maximum() +
                                        GAIN_RANGES[PHX_GAIN_PGA].maximum());
    return SoapySDR::Range(0.0, GAIN_RANGES[PHX_GAIN_DAC].maximum() +
                                    GAIN_RANGES[PHX_GAIN_MIXER].maximum());
}

SoapySDR::Range Phenix1255Device::getGainRange(const int direction, const size_t,
                                               const std::string &name) const
{
    return GAIN_RANGES[gainIndex(direction, name)];
}

void Phenix1255Device::applyGain(int element, double value)
{
    const phx_set_gain_req req{static_cast<uint8_t>(element), 0,
                               static_cast<int16_t>(std::lround(value))};
    const auto resp = request(PHX_OP_SET_GAIN, &req, sizeof(req));
    phx_set_gain_resp actual{};
    std::memcpy(&actual, resp.data(), std::min(resp.size(), sizeof(actual)));
    _gain[element] = actual.gain_db;
}

void Phenix1255Device::setGain(const int direction, const size_t channel,
                               const std::string &name, const double value)
{
    checkChannel(channel);
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    applyGain(gainIndex(direction, name), value);
}

void Phenix1255Device::setGain(const int direction, const size_t channel, const double value)
{
    checkChannel(channel);
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    if (direction == SOAPY_SDR_RX) {
        applyGain(PHX_GAIN_LNA, value - RX_PGA_TARGET);
        applyGain(PHX_GAIN_PGA, value - _gain[PHX_GAIN_LNA]);
    } else {
        applyGain(PHX_GAIN_DAC, value - TX_MIXER_TARGET);
        applyGain(PHX_GAIN_MIXER, value - _gain[PHX_GAIN_DAC]);
    }
}

double Phenix1255Device::getGain(const int direction, const size_t,
                                 const std::string &name) const
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _gain[gainIndex(direction, name)];
}

/*******************************************************************
 * Frequency
 ******************************************************************/

void Phenix1255Device::applyFrequency(int direction, double frequency)
{
    const phx_set_freq_req req{
        static_cast<uint8_t>(direction == SOAPY_SDR_TX ? PHX_DIR_TX : PHX_DIR_RX),
        {0, 0, 0},
        static_cast<uint32_t>(std::llround(frequency))};
    const auto resp = request(PHX_OP_SET_FREQ, &req, sizeof(req));
    phx_set_freq_resp actual{};
    std::memcpy(&actual, resp.data(), std::min(resp.size(), sizeof(actual)));
    // Exact synthesizer frequency
    _freq[direction == SOAPY_SDR_TX ? 1 : 0] =
        static_cast<double>(actual.frf) * _xtalHz / static_cast<double>(1u << 20);
}

void Phenix1255Device::setFrequency(const int direction, const size_t channel,
                                    const double frequency, const SoapySDR::Kwargs &)
{
    checkChannel(channel);
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    applyFrequency(direction, frequency);
}

double Phenix1255Device::getFrequency(const int direction, const size_t) const
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _freq[direction == SOAPY_SDR_TX ? 1 : 0];
}

SoapySDR::RangeList Phenix1255Device::getFrequencyRange(const int, const size_t) const
{
    return {SoapySDR::Range(_freqMinHz, _freqMaxHz)};
}

/*******************************************************************
 * Sample rate
 ******************************************************************/

std::vector<double> Phenix1255Device::listSampleRates(const int, const size_t) const
{
    return _rates;
}

SoapySDR::RangeList Phenix1255Device::getSampleRateRange(const int, const size_t) const
{
    SoapySDR::RangeList ranges;
    for (const double r : _rates)
        ranges.emplace_back(r, r);
    return ranges;
}

void Phenix1255Device::setSampleRate(const int, const size_t channel, const double rate)
{
    checkChannel(channel);
    std::lock_guard<std::recursive_mutex> lock(_mutex);

    // RX and TX share one rate
    const phx_set_rate_req req{static_cast<uint32_t>(std::llround(rate))};
    try {
        request(PHX_OP_SET_RATE, &req, sizeof(req));
    } catch (const DeviceError &e) {
        if (e.status == PHX_EBUSY)
            throw std::runtime_error("Phenix-1255: sample rate can only be changed while "
                                     "no stream is active");
        throw std::invalid_argument("Phenix-1255: unsupported sample rate " +
                                    std::to_string(rate));
    }
    _rate = req.rate;
    _samples->setSampleRate(_rate);
}

double Phenix1255Device::getSampleRate(const int, const size_t) const
{
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    return _rate;
}

/*******************************************************************
 * Time
 ******************************************************************/

bool Phenix1255Device::hasHardwareTime(const std::string &what) const
{
    return what.empty();
}

long long Phenix1255Device::getHardwareTime(const std::string &what) const
{
    if (!what.empty())
        throw std::invalid_argument("Phenix-1255: unknown time source " + what);
    // Estimated from the latest RX timestamp, the device is not queried
    const int64_t counter = _samples->estimatedCounter();
    return _samples->ticksToNs(counter < 0 ? 0 : counter);
}

/*******************************************************************
 * Registers
 ******************************************************************/

void Phenix1255Device::writeRegister(const unsigned addr, const unsigned value)
{
    uint8_t buf[sizeof(phx_reg_req) + 1];
    const phx_reg_req req{static_cast<uint8_t>(addr), 1, {0, 0}};
    std::memcpy(buf, &req, sizeof(req));
    buf[sizeof(req)] = static_cast<uint8_t>(value);
    request(PHX_OP_WRITE_REG, buf, sizeof(buf));
}

unsigned Phenix1255Device::readRegister(const unsigned addr) const
{
    const phx_reg_req req{static_cast<uint8_t>(addr), 1, {0, 0}};
    const auto resp = request(PHX_OP_READ_REG, &req, sizeof(req));
    return resp.empty() ? 0 : resp[0];
}

/*******************************************************************
 * Settings
 ******************************************************************/

SoapySDR::ArgInfoList Phenix1255Device::getSettingInfo() const
{
    SoapySDR::ArgInfo lead;
    lead.key = "lead_ms";
    lead.name = "TX lead time";
    lead.description = "How far ahead of the device time TX packets are sent";
    lead.type = SoapySDR::ArgInfo::FLOAT;
    lead.units = "ms";
    lead.value = std::to_string(DEFAULT_LEAD_MS);
    lead.range = SoapySDR::Range(1.0, 50.0);
    return {lead};
}

void Phenix1255Device::writeSetting(const std::string &key, const std::string &value)
{
    if (key == "lead_ms")
        _samples->setLeadTime(std::stod(value) / 1000.0);
    else
        throw std::invalid_argument("Phenix-1255: unknown setting " + key);
}

std::string Phenix1255Device::readSetting(const std::string &key) const
{
    if (key == "lead_ms")
        return std::to_string(_samples->leadTime() * 1000.0);
    throw std::invalid_argument("Phenix-1255: unknown setting " + key);
}

} // namespace phx
