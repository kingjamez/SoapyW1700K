// SPDX-License-Identifier: MIT
//
// SoapySDR driver for the W1700K, a receive-only wideband SDR.
//
// Two independent paths connect this module to the radio:
//
//   * Samples: the host's `w1700k-stream` service receives the unit's UDP export and publishes it
//     in POSIX shared memory (include/w1700k_stream.h, a copy of the w1700k-sdr project's header). This module maps that memory and
//     copies samples out of it; see Streaming.cpp. It never talks to the unit for samples.
//
//   * Control: band, frequency, sample rate, filter and gain are changed by running the host's
//     `w1700k` command, which controls the unit over ssh; see Command.hpp and Settings.cpp.
//
// Device arguments (all optional):
//
//   band=0|1|2|all    fix the device to one band (2.4G, 5G, 6G), or all three antennas; see "Bands" below
//   flatten=true      have the stream service correct the decimation filter's roll-off
//   shm=NAME          the stream service's shared-memory object, default /w1700k-stream
//   cmd=PATH          the `w1700k` command; default $W1700K_CMD, else the path compiled in
//
// Bands. The radio has three RF front ends. SoapySDR has one frequency range per channel and none
// per antenna, so this module offers each band as a device of its own ("W1700K 2.4G", "W1700K 5G",
// "W1700K 6G"), with that band's tuning range as the device's range and its name as the only
// antenna. A program that never asks for ranges (SDR++) then shows the right limits. With band=all
// the device has all three antennas, and its range follows the antenna selected. (A request that
// names no band is completed by SoapySDR with the first listed device's, so it gets band 0: name
// band=all to get all antennas; see Registration.cpp.)
//
// Threading. An application calls the control functions from one thread (its UI) and readStream
// from another. Control state is kept in w1700k::Controller, which locks it. The mapping of the
// stream service's memory (`ring_`) and the reader's position change only with `read_mutex_` held,
// which readStream also holds while it reads, so closeStream, a remap or the destructor can never
// unmap memory a read is in. SoapySDR leaves it to the application to serialise stream calls with
// each other; this adds only that safety.
//
// Errors. Two kinds, handled differently. The radio failing to do what was asked (the `w1700k`
// command fails, the unit does not lock) is logged and not thrown: SDR++ and other hosts do not
// catch exceptions from the setters, and the getters keep reporting the radio's real state. Misuse
// of the API (an antenna, gain or setting that does not exist, a transmit stream) throws.
#pragma once

#include <SoapySDR/Device.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "Command.hpp"
#include "w1700k_stream.h"

class SoapyW1700K : public SoapySDR::Device {
public:
    // Throws std::runtime_error if the arguments are invalid or the `w1700k` command cannot be run.
    explicit SoapyW1700K(const SoapySDR::Kwargs &args);
    ~SoapyW1700K() override;

    // --- Identification (Settings.cpp) ---
    std::string getDriverKey() const override;
    std::string getHardwareKey() const override;
    SoapySDR::Kwargs getHardwareInfo() const override;

    // --- Channels (Settings.cpp): one receive channel, no transmit ---
    size_t getNumChannels(const int direction) const override;
    bool getFullDuplex(const int direction, const size_t channel) const override;

    // --- Antennas: the band (Settings.cpp) ---
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const override;
    void setAntenna(const int direction, const size_t channel, const std::string &name) override;
    std::string getAntenna(const int direction, const size_t channel) const override;

    // --- Gain: the LNA and LPF steps, or AGC (Settings.cpp) ---
    bool hasGainMode(const int direction, const size_t channel) const override;
    void setGainMode(const int direction, const size_t channel, const bool automatic) override;
    bool getGainMode(const int direction, const size_t channel) const override;
    std::vector<std::string> listGains(const int direction, const size_t channel) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel, const std::string &name) const override;
    void setGain(const int direction, const size_t channel, const double value) override;
    void setGain(const int direction, const size_t channel, const std::string &name, const double value) override;
    double getGain(const int direction, const size_t channel) const override;
    double getGain(const int direction, const size_t channel, const std::string &name) const override;

    // --- Frequency: the active band's LO (Settings.cpp) ---
    std::vector<std::string> listFrequencies(const int direction, const size_t channel) const override;
    void setFrequency(const int direction, const size_t channel, const double frequency,
                      const SoapySDR::Kwargs &args) override;
    void setFrequency(const int direction, const size_t channel, const std::string &name, const double frequency,
                      const SoapySDR::Kwargs &args) override;
    double getFrequency(const int direction, const size_t channel) const override;
    double getFrequency(const int direction, const size_t channel, const std::string &name) const override;
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const override;
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel,
                                          const std::string &name) const override;

    // --- Sample rate and bandwidth (Settings.cpp) ---
    void setSampleRate(const int direction, const size_t channel, const double rate) override;
    double getSampleRate(const int direction, const size_t channel) const override;
    std::vector<double> listSampleRates(const int direction, const size_t channel) const override;
    void setBandwidth(const int direction, const size_t channel, const double bandwidth) override;
    double getBandwidth(const int direction, const size_t channel) const override;
    std::vector<double> listBandwidths(const int direction, const size_t channel) const override;

    // --- Driver settings (Settings.cpp): "flatten" ---
    SoapySDR::ArgInfoList getSettingInfo() const override;
    void writeSetting(const std::string &key, const std::string &value) override;
    std::string readSetting(const std::string &key) const override;

    // --- Streaming (Streaming.cpp) ---
    std::vector<std::string> getStreamFormats(const int direction, const size_t channel) const override;
    std::string getNativeStreamFormat(const int direction, const size_t channel, double &fullScale) const override;
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction, const size_t channel) const override;
    SoapySDR::Stream *setupStream(const int direction, const std::string &format, const std::vector<size_t> &channels,
                                  const SoapySDR::Kwargs &args) override;
    void closeStream(SoapySDR::Stream *stream) override;
    size_t getStreamMTU(SoapySDR::Stream *stream) const override;
    int activateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs,
                       const size_t numElems) override;
    int deactivateStream(SoapySDR::Stream *stream, const int flags, const long long timeNs) override;
    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags, long long &timeNs,
                   const long timeoutUs) override;

private:
    // --- Control (Settings.cpp) ---

    // Runs a `w1700k` command; a failure is logged, not thrown (see "Errors" above). Returns
    // whether it succeeded.
    bool change(const std::vector<std::string> &args);

    // The band this device is about: the one it is fixed to, else the unit's active band.
    int band() const;

    // A device fixed to a band puts the unit on it before any change that applies to the active
    // band (tune, filter, gain). Returns false, after logging, if the unit could not be put on the
    // band: the change must then not be sent, it would apply to another band. Callers hold
    // settings_mutex_.
    bool select_band();

    // Sets both gain steps (clamped to their ranges). With AGC on they are only remembered, and
    // applied when AGC is turned off: SDR++ sets the gains after setting the gain mode, and the
    // radio's gain command turns AGC off.
    void set_gain_steps(int lna, int lpf);

    // The tuning range of band `b`, as this module advertises and enforces it.
    void tuning_range(int b, double &lowest, double &highest) const;

    // Writes the radio's state, and the flatten setting, into the stream service's control words.
    // The service needs them to know when its roll-off correction applies. They are shared by every
    // consumer of the service, so the last writer wins.
    void publish_radio_state();

    // --- Streaming (Streaming.cpp) ---

    // Makes `ring_` the stream service's current shared memory: opens it, or replaces a mapping
    // whose service has gone (a restarted service creates a new object). Returns whether a live
    // service is mapped. Callers hold read_mutex_.
    bool map_stream();
    void unmap_stream();

    // Waits (up to `patience`) for the service's published position to advance; true if it did.
    bool stream_advances(std::chrono::milliseconds patience);

    // The receive-stream handle SoapySDR hands back to the application. It only has to be a
    // unique, valid pointer: there is one stream.
    struct RxStream {};

    w1700k::Controller controller_;
    const std::string shm_name_;
    int fixed_band_ = -1;                  // -1: all three antennas
    std::atomic<bool> flatten_;
    std::mutex settings_mutex_;            // makes "select the band, then change it" one step
    int wanted_lna_ = -1, wanted_lpf_ = -1;  // gain steps asked for; -1: none yet (settings_mutex_)

    RxStream rx_stream_;
    std::mutex read_mutex_;                // held by readStream, and by whatever changes ring_ or the position
    std::mutex ring_mutex_;                // held while ring_ changes and while the control words are written
    w1700k_stream_header *ring_ = nullptr; // the service's shared memory; changes with both mutexes held
    size_t ring_bytes_ = 0;
    uint64_t ring_inode_ = 0;              // of the shared-memory object mapped: a new service has a new one
    std::atomic<bool> active_{false};      // between activateStream and deactivateStream
    bool want_cf32_ = false;               // the format the stream was set up with (else CS16)

    // The reader's position (read_mutex_).
    uint64_t run_ = 0;                     // the stream (run) being read
    uint64_t next_slice_ = 0;              // the next slice of that run
    uint64_t slice_offset_ = 0;            // samples of that slice already delivered
};
