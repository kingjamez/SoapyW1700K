// SPDX-License-Identifier: MIT
//
// The control half of the W1700K driver: everything SoapySDR can ask about or change on the radio
// except the stream. Changes are made by running the host's `w1700k` command (Command.hpp);
// questions are answered from the state its last reply reported.
#include <SoapySDR/Logger.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "SoapyW1700K.hpp"

namespace {

// Sample rates the radio offers: 40, 80, 160 and 320 Msps, which are its 20, 40, 80 and 160 MHz
// bandwidth classes sampled at twice the class. (The host also has "640/2" on the 6G band: the
// 320 MHz class captured at 640 Msps with every other sample streamed. Signals beyond the middle
// 320 MHz fold in, so it is not offered here; see host/w1700k/README.md in the w1700k-sdr project.)
const double kSampleRates[] = {40e6, 80e6, 160e6, 320e6};

// Widths of the analog filter in front of the converter. It is independent of the sample rate:
// the host keeps it across rate changes.
const double kFilterWidths[] = {20e6, 40e6, 80e6, 160e6, 320e6};

// The gain control: two steps in the receive path, each an integer number of steps above its
// minimum. There is no calibrated dB scale, so SoapySDR's "dB" here counts steps.
constexpr int kLnaSteps = 4;   // LNA: 0..4
constexpr int kLpfSteps = 9;   // LPF (the filter stage's gain): 0..9
constexpr int kTotalSteps = kLnaSteps + kLpfSteps;

// A requested sample rate further than this (as a fraction) from every offered rate is logged as
// not offered; the nearest offered rate is used either way.
constexpr double kRateTolerance = 0.01;

// The synthesizer's lock span is measured when the unit loads; its top edge was found to be up to
// a few MHz below the measured span (a tune a few MHz inside the edge failed to lock). The advertised
// range is inset by this much at both ends.
constexpr double kEdgeMargin = 5e6;

// The driver's one setting.
const char *const kFlattenKey = "flatten";

}  // namespace

// --- Identification ---------------------------------------------------------------------------

std::string SoapyW1700K::getDriverKey() const { return "w1700k"; }

std::string SoapyW1700K::getHardwareKey() const { return "W1700K"; }

SoapySDR::Kwargs SoapyW1700K::getHardwareInfo() const
{
    return {{"shm", shm_name_}, {"cmd", controller_.command()}};
}

// --- Channels ---------------------------------------------------------------------------------

size_t SoapyW1700K::getNumChannels(const int direction) const { return direction == SOAPY_SDR_RX ? 1 : 0; }

bool SoapyW1700K::getFullDuplex(const int, const size_t) const { return false; }

// --- Control helpers --------------------------------------------------------------------------

bool SoapyW1700K::change(const std::vector<std::string> &args)
{
    bool succeeded = true;
    try {
        controller_.run(args);
    } catch (const std::exception &e) {
        succeeded = false;
        SoapySDR::logf(SOAPY_SDR_ERROR, "W1700K: %s", e.what());
        // The failed command may still have changed the radio before it failed; learn how it stands.
        try {
            controller_.refresh();
        } catch (const std::exception &) {
        }
    }
    if (active_) publish_radio_state();  // a stream is running: its service must know the new rate and filter
    return succeeded;
}

int SoapyW1700K::band() const { return fixed_band_ >= 0 ? fixed_band_ : controller_.state().band; }

bool SoapyW1700K::select_band()
{
    if (fixed_band_ < 0 || controller_.state().band == fixed_band_) return true;
    change({"band", std::to_string(fixed_band_)});
    if (controller_.state().band == fixed_band_) return true;
    SoapySDR::logf(SOAPY_SDR_ERROR, "W1700K: could not switch the radio to %s; not changing it", w1700k::kBandNames[fixed_band_]);
    return false;
}

void SoapyW1700K::tuning_range(int b, double &lowest, double &highest) const
{
    const w1700k::RadioState state = controller_.state();
    lowest = state.lo_span_hz[b][0] + kEdgeMargin;
    highest = state.lo_span_hz[b][1] - kEdgeMargin;
}

void SoapyW1700K::publish_radio_state()
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (!ring_) return;
    const w1700k::RadioState state = controller_.state();
    w1700k_stream_store(&ring_->radio_sample_rate, uint64_t(std::llround(state.sample_rate)));
    w1700k_stream_store(&ring_->radio_capture_rate, uint64_t(std::llround(state.capture_rate)));
    w1700k_stream_store(&ring_->radio_filter_hz, uint64_t(std::llround(state.filter_hz)));
    w1700k_stream_store(&ring_->flatten, uint64_t(flatten_.load()));
}

// --- Antennas: the band -----------------------------------------------------------------------

std::vector<std::string> SoapyW1700K::listAntennas(const int, const size_t) const
{
    if (fixed_band_ >= 0) return {w1700k::kBandNames[fixed_band_]};
    return {std::begin(w1700k::kBandNames), std::end(w1700k::kBandNames)};
}

void SoapyW1700K::setAntenna(const int, const size_t, const std::string &name)
{
    for (int b = 0; b < w1700k::kBandCount; b++) {
        if (name != w1700k::kBandNames[b]) continue;
        if (fixed_band_ >= 0 && b != fixed_band_) break;  // this device has only its own band
        std::lock_guard<std::mutex> lock(settings_mutex_);
        change({"band", std::to_string(b)});
        return;
    }
    throw std::runtime_error("W1700K: no antenna '" + name + "'");
}

std::string SoapyW1700K::getAntenna(const int, const size_t) const { return w1700k::kBandNames[band()]; }

// --- Gain -------------------------------------------------------------------------------------

bool SoapyW1700K::hasGainMode(const int, const size_t) const { return true; }

void SoapyW1700K::setGainMode(const int, const size_t, const bool automatic)
{
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (!select_band()) return;
    if (automatic) {
        change({"gain", "--agc"});
        return;
    }
    // Leaving AGC: apply the steps last asked for, else the ones the unit last reported, else
    // maximum (the radio's own default when AGC is first turned off).
    const w1700k::RadioState state = controller_.state();
    const int lna = wanted_lna_ >= 0 ? wanted_lna_ : state.lna >= 0 ? state.lna : kLnaSteps;
    const int lpf = wanted_lpf_ >= 0 ? wanted_lpf_ : state.lpf >= 0 ? state.lpf : kLpfSteps;
    change({"gain", "--lna", std::to_string(lna), "--lpf", std::to_string(lpf)});
}

bool SoapyW1700K::getGainMode(const int, const size_t) const { return controller_.state().agc; }

std::vector<std::string> SoapyW1700K::listGains(const int, const size_t) const { return {"LNA", "LPF"}; }

SoapySDR::Range SoapyW1700K::getGainRange(const int, const size_t) const
{
    return SoapySDR::Range(0, kTotalSteps, 1);
}

SoapySDR::Range SoapyW1700K::getGainRange(const int, const size_t, const std::string &name) const
{
    if (name == "LNA") return SoapySDR::Range(0, kLnaSteps, 1);
    if (name == "LPF") return SoapySDR::Range(0, kLpfSteps, 1);
    throw std::runtime_error("W1700K: no gain '" + name + "'");
}

void SoapyW1700K::set_gain_steps(int lna, int lpf)
{
    lna = std::min(kLnaSteps, std::max(0, lna));
    lpf = std::min(kLpfSteps, std::max(0, lpf));
    std::lock_guard<std::mutex> lock(settings_mutex_);
    wanted_lna_ = lna;
    wanted_lpf_ = lpf;
    // With AGC on there is nothing to set: the radio's gain command would turn AGC off, and SDR++
    // sets every gain after the gain mode, whatever the mode is. setGainMode(false) applies them.
    if (controller_.state().agc) return;
    if (!select_band()) return;
    change({"gain", "--lna", std::to_string(lna), "--lpf", std::to_string(lpf)});
}

void SoapyW1700K::setGain(const int, const size_t, const double value)
{
    // The overall gain is spread over both steps in proportion to their ranges.
    const double fraction = std::min(1.0, std::max(0.0, value / kTotalSteps));
    set_gain_steps(int(std::lround(fraction * kLnaSteps)), int(std::lround(fraction * kLpfSteps)));
}

void SoapyW1700K::setGain(const int, const size_t, const std::string &name, const double value)
{
    // One command sets both steps, so the other keeps what was last asked for, else what the unit
    // last reported, else maximum. An application that sets both gains in turn sends two commands;
    // the second leaves the radio as asked.
    const w1700k::RadioState state = controller_.state();
    int lna, lpf;
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        lna = wanted_lna_ >= 0 ? wanted_lna_ : state.lna >= 0 ? state.lna : kLnaSteps;
        lpf = wanted_lpf_ >= 0 ? wanted_lpf_ : state.lpf >= 0 ? state.lpf : kLpfSteps;
    }
    if (name == "LNA") set_gain_steps(int(std::lround(value)), lpf);
    else if (name == "LPF") set_gain_steps(lna, int(std::lround(value)));
    else throw std::runtime_error("W1700K: no gain '" + name + "'");
}

double SoapyW1700K::getGain(const int, const size_t) const
{
    const w1700k::RadioState state = controller_.state();
    return std::max(0, state.lna) + std::max(0, state.lpf);
}

double SoapyW1700K::getGain(const int, const size_t, const std::string &name) const
{
    const w1700k::RadioState state = controller_.state();
    if (name == "LNA") return std::max(0, state.lna);
    if (name == "LPF") return std::max(0, state.lpf);
    throw std::runtime_error("W1700K: no gain '" + name + "'");
}

// --- Frequency --------------------------------------------------------------------------------

std::vector<std::string> SoapyW1700K::listFrequencies(const int, const size_t) const { return {"RF"}; }

void SoapyW1700K::setFrequency(const int, const size_t, const double frequency, const SoapySDR::Kwargs &)
{
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (!select_band()) return;
    // The radio tunes only inside the band's lock span. A request outside it is moved to the edge
    // rather than refused, so that a program that does not know the span still ends up tuned.
    double lowest, highest;
    tuning_range(band(), lowest, highest);
    const double lo = std::min(highest, std::max(lowest, frequency));
    if (lo != frequency)
        SoapySDR::logf(SOAPY_SDR_WARNING, "W1700K: %.3f MHz is outside antenna %s (%.3f to %.3f MHz); using %.3f MHz",
                       frequency / 1e6, w1700k::kBandNames[band()], lowest / 1e6, highest / 1e6, lo / 1e6);
    change({"tune", std::to_string(std::llround(lo))});
}

void SoapyW1700K::setFrequency(const int direction, const size_t channel, const std::string &, const double frequency,
                               const SoapySDR::Kwargs &args)
{
    setFrequency(direction, channel, frequency, args);  // the one component, "RF", is the LO
}

double SoapyW1700K::getFrequency(const int, const size_t) const { return controller_.state().lo_hz; }

double SoapyW1700K::getFrequency(const int direction, const size_t channel, const std::string &) const
{
    return getFrequency(direction, channel);
}

SoapySDR::RangeList SoapyW1700K::getFrequencyRange(const int, const size_t) const
{
    double lowest, highest;
    tuning_range(band(), lowest, highest);
    return {SoapySDR::Range(lowest, highest)};
}

SoapySDR::RangeList SoapyW1700K::getFrequencyRange(const int direction, const size_t channel,
                                                   const std::string &) const
{
    return getFrequencyRange(direction, channel);
}

// --- Sample rate and bandwidth ----------------------------------------------------------------

void SoapyW1700K::setSampleRate(const int, const size_t, const double rate)
{
    double nearest = kSampleRates[0];
    for (double offered : kSampleRates)
        if (std::fabs(offered - rate) < std::fabs(nearest - rate)) nearest = offered;
    if (std::fabs(nearest - rate) > kRateTolerance * nearest)
        SoapySDR::logf(SOAPY_SDR_WARNING, "W1700K: %.3f Msps is not offered; using %.0f Msps", rate / 1e6,
                       nearest / 1e6);
    std::lock_guard<std::mutex> lock(settings_mutex_);
    // `band B --rate R` is the host's way to change the rate; it also selects band B, which is why
    // it names the band this device is about. A running stream restarts at the new rate.
    change({"band", std::to_string(band()), "--rate", std::to_string(long(nearest / 1e6))});
}

double SoapyW1700K::getSampleRate(const int, const size_t) const { return controller_.state().sample_rate; }

std::vector<double> SoapyW1700K::listSampleRates(const int, const size_t) const
{
    return {std::begin(kSampleRates), std::end(kSampleRates)};
}

void SoapyW1700K::setBandwidth(const int, const size_t, const double bandwidth)
{
    // SoapySDR: a bandwidth of zero means "automatic". Automatic here is the filter as wide as the
    // sample rate is fast, in MHz (320 MHz at 320 Msps): what the radio is loaded with.
    const double wanted = bandwidth > 0 ? bandwidth : controller_.state().sample_rate;
    double width = kFilterWidths[std::size(kFilterWidths) - 1];
    for (double offered : kFilterWidths)
        if (offered >= wanted) {
            width = offered;  // the narrowest filter that passes what was asked
            break;
        }
    std::lock_guard<std::mutex> lock(settings_mutex_);
    if (!select_band()) return;
    change({"filter", std::to_string(long(width / 1e6))});
}

double SoapyW1700K::getBandwidth(const int, const size_t) const { return controller_.state().filter_hz; }

std::vector<double> SoapyW1700K::listBandwidths(const int, const size_t) const
{
    return {std::begin(kFilterWidths), std::end(kFilterWidths)};
}

// --- Driver settings --------------------------------------------------------------------------

SoapySDR::ArgInfoList SoapyW1700K::getSettingInfo() const
{
    SoapySDR::ArgInfo flatten;
    flatten.key = kFlattenKey;
    flatten.name = "Flatten";
    flatten.description = "Correct the decimation filter's roll-off at the band edges, in the samples "
                          "(costs about one more CPU core in w1700k-stream)";
    flatten.type = SoapySDR::ArgInfo::BOOL;
    flatten.value = "false";
    return {flatten};
}

void SoapyW1700K::writeSetting(const std::string &key, const std::string &value)
{
    if (key != kFlattenKey) throw std::runtime_error("W1700K: no setting '" + key + "'");
    flatten_ = w1700k::is_true(value);
    publish_radio_state();
}

std::string SoapyW1700K::readSetting(const std::string &key) const
{
    if (key != kFlattenKey) throw std::runtime_error("W1700K: no setting '" + key + "'");
    return flatten_ ? "true" : "false";
}
