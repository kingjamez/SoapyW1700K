// SPDX-License-Identifier: MIT
//
// The sample half of the W1700K driver: reading IQ from the shared memory of `w1700k-stream`.
//
// The service files the unit's UDP export into a ring of "slices" of 2048 complex samples
// (host/stream/README.md in the w1700k-sdr project describes it; include/w1700k_stream.h defines it). A run is one
// stream; its slices are numbered from 0, and the service publishes them in order. This file reads
// them in order, copies the samples out in the format the application asked for, and reports every
// discontinuity to the application as SOAPY_SDR_OVERFLOW so it can treat the samples as a gap.
//
// The ring is a seqlock: a reader checks a slice's tag, copies it, and checks the tag again. A tag
// that changed means the service reused the slot meanwhile (the reader fell a ring behind).
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>

#include "SoapyW1700K.hpp"

namespace {

// readStream waits for the service by polling its published position. A slice of 2048 samples is
// 6.4 us of signal at 320 Msps and 51 us at 40 Msps; the ring holds hundreds of milliseconds, so
// polling at this interval costs a little latency and no samples.
constexpr std::chrono::microseconds kPollInterval{100};

// The longest a readStream timeout is taken at. (Some callers pass an enormous value for "wait
// forever", which would overflow the clock's range if added to the current time.)
constexpr long kMaxTimeoutUs = 3600L * 1000 * 1000;

// How long activateStream waits for the first new slice before deciding the unit is not sending.
constexpr std::chrono::milliseconds kStartPatience{1500};

// A reader this close to a full ring behind the service is about to be overwritten; it is moved
// forward instead (to a quarter ring behind), and the gap reported.
constexpr uint64_t kGuardSlices = 64;

// What a slot of the ring holds, judged from its tag.
enum class Slot {
    Samples,      // a slice of samples (raw or corrected)
    Event,        // a firmware event on the unit's ring: no samples, and the samples either side are contiguous
    Lost,         // a completion that never arrived: the samples are missing
    Overwritten,  // the slot holds another slice of the ring: the service lapped the reader
};

Slot classify(uint64_t tag, uint64_t run, uint64_t index)
{
    // Every kind of slot carries (run, index) in its tag, so a slot left over from an earlier lap
    // of the ring is recognised whatever kind it was.
    if ((tag & ~W1700K_STREAM_TAG_KIND) != w1700k_stream_tag(run, index)) return Slot::Overwritten;
    if (tag & W1700K_STREAM_TAG_EVENT) return Slot::Event;
    if (tag & W1700K_STREAM_TAG_LOST) return Slot::Lost;
    return Slot::Samples;
}

}  // namespace

// --- Format negotiation -----------------------------------------------------------------------

std::vector<std::string> SoapyW1700K::getStreamFormats(const int, const size_t) const
{
    return {SOAPY_SDR_CS16, SOAPY_SDR_CF32};
}

std::string SoapyW1700K::getNativeStreamFormat(const int, const size_t, double &fullScale) const
{
    // The radio's samples are 14 bits, signed: full scale is 2^13. (With "flatten" the correction
    // can exceed it; the service keeps the headroom of 16 bits.)
    fullScale = W1700K_STREAM_FULL_SCALE;
    return SOAPY_SDR_CS16;
}

SoapySDR::ArgInfoList SoapyW1700K::getStreamArgsInfo(const int, const size_t) const { return {}; }

size_t SoapyW1700K::getStreamMTU(SoapySDR::Stream *) const { return W1700K_STREAM_SLICE; }

// --- The shared memory ------------------------------------------------------------------------

bool SoapyW1700K::map_stream()
{
    // Writable: the module writes the service's control words (publish_radio_state).
    size_t bytes = 0;
    uint64_t inode = 0;
    w1700k_stream_header *fresh = w1700k_stream_open(shm_name_.c_str(), /*writable=*/1, &bytes, &inode);

    if (ring_ && fresh && inode == ring_inode_) {  // the object we already have
        munmap(fresh, bytes);
        return w1700k_stream_alive(ring_);
    }
    if (!fresh) return ring_ && w1700k_stream_alive(ring_);

    // A service that was restarted has created a new object; the old mapping is of a dead one.
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (ring_) munmap(ring_, ring_bytes_);
    ring_ = fresh;
    ring_bytes_ = bytes;
    ring_inode_ = inode;
    return w1700k_stream_alive(ring_);
}

void SoapyW1700K::unmap_stream()
{
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (ring_) munmap(ring_, ring_bytes_);
    ring_ = nullptr;
}

bool SoapyW1700K::stream_advances(std::chrono::milliseconds patience)
{
    uint64_t run, before;
    w1700k_stream_position(ring_, &run, &before);
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        uint64_t newest;
        w1700k_stream_position(ring_, &run, &newest);
        if (newest != before) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// --- Stream life cycle ------------------------------------------------------------------------

SoapySDR::Stream *SoapyW1700K::setupStream(const int direction, const std::string &format,
                                           const std::vector<size_t> &channels, const SoapySDR::Kwargs &)
{
    if (direction != SOAPY_SDR_RX) throw std::runtime_error("W1700K: receive only");
    if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0))
        throw std::runtime_error("W1700K: there is one receive channel, 0");
    if (format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32)
        throw std::runtime_error("W1700K: stream format " + format + " is not offered (CS16, CF32)");

    std::lock_guard<std::mutex> reading(read_mutex_);
    if (active_) throw std::runtime_error("W1700K: the stream is active; deactivate it before setting it up again");
    if (!map_stream())
        throw std::runtime_error("W1700K: the stream service is not running (no live shared memory '" + shm_name_ +
                                 "'); start w1700k-stream");
    want_cf32_ = format == SOAPY_SDR_CF32;
    return reinterpret_cast<SoapySDR::Stream *>(&rx_stream_);
}

void SoapyW1700K::closeStream(SoapySDR::Stream *stream)
{
    if (stream != reinterpret_cast<SoapySDR::Stream *>(&rx_stream_)) return;
    active_ = false;
    std::lock_guard<std::mutex> reading(read_mutex_);  // wait for a read in progress
    unmap_stream();
}

int SoapyW1700K::activateStream(SoapySDR::Stream *stream, const int flags, const long long, const size_t)
{
    if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;  // no timed or burst streaming: the radio has no hardware time
    if (stream != reinterpret_cast<SoapySDR::Stream *>(&rx_stream_)) return SOAPY_SDR_STREAM_ERROR;
    std::lock_guard<std::mutex> reading(read_mutex_);  // no read is running while the position is set
    active_ = false;
    if (!map_stream()) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "W1700K: the stream service is not running");
        return SOAPY_SDR_STREAM_ERROR;
    }
    try {
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            controller_.refresh();  // another program may have changed the radio since we last looked
            if (!select_band()) return SOAPY_SDR_STREAM_ERROR;
            if (!controller_.state().streaming) controller_.run({"start"});
        }
        active_ = true;  // from here a rate or filter change is published to the service
        publish_radio_state();

        // The unit's state can say "streaming" while no data arrives (seen after another program
        // stopped the stream). If the ring does not move, ask for a start once more.
        if (!stream_advances(std::chrono::milliseconds(300))) {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            controller_.run({"start"});
            if (!stream_advances(kStartPatience)) {
                SoapySDR::logf(SOAPY_SDR_ERROR, "W1700K: no samples from the unit; is the stream's 10 GbE link up?");
                active_ = false;
                return SOAPY_SDR_STREAM_ERROR;
            }
        }
    } catch (const std::exception &e) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "W1700K: %s", e.what());
        active_ = false;
        return SOAPY_SDR_STREAM_ERROR;
    }

    // Start at the live edge: the oldest data in the ring is stale by up to a fraction of a second.
    uint64_t run, newest;
    w1700k_stream_position(ring_, &run, &newest);
    run_ = run;
    next_slice_ = newest;
    slice_offset_ = 0;
    active_ = true;
    return 0;
}

int SoapyW1700K::deactivateStream(SoapySDR::Stream *stream, const int flags, const long long)
{
    if (flags != 0) return SOAPY_SDR_NOT_SUPPORTED;
    if (stream != reinterpret_cast<SoapySDR::Stream *>(&rx_stream_)) return SOAPY_SDR_STREAM_ERROR;
    active_ = false;  // the unit keeps streaming: other programs may be reading the same ring
    return 0;
}

// --- Reading ----------------------------------------------------------------------------------

int SoapyW1700K::readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems, int &flags,
                            long long &timeNs, const long timeoutUs)
{
    flags = 0;   // no end-of-burst, no hardware time
    timeNs = 0;
    std::lock_guard<std::mutex> reading(read_mutex_);
    if (stream != reinterpret_cast<SoapySDR::Stream *>(&rx_stream_) || !active_ || !ring_ || !buffs || !buffs[0])
        return SOAPY_SDR_STREAM_ERROR;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(std::min(std::max(timeoutUs, 0L), kMaxTimeoutUs));
    int16_t *const out_cs16 = static_cast<int16_t *>(buffs[0]);
    float *const out_cf32 = static_cast<float *>(buffs[0]);
    size_t done = 0;  // samples delivered into the buffer so far

    // A gap in the stream is reported with SOAPY_SDR_OVERFLOW. Samples already copied are
    // returned first, so the application sees them in order, and the gap on its next call.
    // `resume` is where the reader continues afterwards.
    auto gap = [&](uint64_t resume_run, uint64_t resume_slice) {
        if (done) return int(done);
        run_ = resume_run;
        next_slice_ = resume_slice;
        slice_offset_ = 0;
        return int(SOAPY_SDR_OVERFLOW);
    };

    for (;;) {
        uint64_t run, newest;
        w1700k_stream_position(ring_, &run, &newest);

        // A new run: the unit's stream restarted (a rate change, say). Nothing of the old run follows.
        if (run != run_) return gap(run, 0);

        // Too far behind: the oldest slices are about to be overwritten.
        if (newest > next_slice_ && newest - next_slice_ > ring_->ring_slices - kGuardSlices)
            return gap(run_, newest - ring_->ring_slices / 4);

        while (next_slice_ < newest && done < numElems) {
            uint64_t *tags = w1700k_stream_tags(ring_);
            const size_t slot = size_t(next_slice_ & (ring_->ring_slices - 1));
            const uint64_t tag = w1700k_stream_load(&tags[slot]);

            switch (classify(tag, run_, next_slice_)) {
            case Slot::Overwritten:
                return gap(run_, newest);
            case Slot::Lost:
                if (done) return int(done);
                next_slice_++;
                slice_offset_ = 0;
                return SOAPY_SDR_OVERFLOW;
            case Slot::Event:
                next_slice_++;  // no samples, and the stream is contiguous across it
                continue;
            case Slot::Samples:
                break;
            }

            // Copy as much of the slice as the buffer holds. A slice can span two calls.
            const int16_t *source = w1700k_stream_slice(ring_, next_slice_) + 2 * slice_offset_;
            const size_t count = std::min<size_t>(W1700K_STREAM_SLICE - slice_offset_, numElems - done);
            if (want_cf32_) {
                const float scale = 1.0f / W1700K_STREAM_FULL_SCALE;
                for (size_t i = 0; i < 2 * count; i++) out_cf32[2 * done + i] = float(source[i]) * scale;
            } else {
                std::memcpy(out_cs16 + 2 * done, source, count * 2 * sizeof(int16_t));
            }

            // The seqlock's second look: if the tag changed, the service reused the slot while we
            // copied, and what we copied is mixed. Those samples are discarded (done is not advanced).
            // The fence keeps the loads of the samples above from being satisfied after this tag load
            // (an acquire load alone only orders what comes after it).
            std::atomic_thread_fence(std::memory_order_acquire);
            if (w1700k_stream_load(&tags[slot]) != tag) return gap(run_, newest);

            done += count;
            slice_offset_ += count;
            if (slice_offset_ == W1700K_STREAM_SLICE) {
                slice_offset_ = 0;
                next_slice_++;
            }
        }

        if (done == numElems) return int(done);
        if (std::chrono::steady_clock::now() >= deadline) {
            if (done) return int(done);  // a short read at the deadline is still data
            // Nothing arrived. A service that is not even beating is an error, not a quiet radio.
            return w1700k_stream_alive(ring_) ? SOAPY_SDR_TIMEOUT : SOAPY_SDR_STREAM_ERROR;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}
