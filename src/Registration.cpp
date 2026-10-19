// SPDX-License-Identifier: MIT
//
// How SoapySDR finds and opens W1700K devices.
#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Registry.hpp>

#include <sys/mman.h>

#include "Command.hpp"
#include "SoapyW1700K.hpp"

namespace {

// The devices a program may open: one per band while the stream service is running. A W1700K
// without its service cannot deliver samples, so it is not listed. (This only looks at the shared
// memory; it neither contacts the unit nor changes anything.)
//
// SoapySDR::Device::make("driver=w1700k") opens the first device enumerate() finds for those
// arguments and adds that device's arguments to them, so a request that does not name a band would
// get the first band's device. A program that wants the device with all three antennas asks for it
// with band=all, which is listed as a device of its own when asked for.
SoapySDR::KwargsList find(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList found;

    const auto shm_arg = args.find("shm");
    const std::string shm = shm_arg == args.end() || shm_arg->second.empty() ? W1700K_STREAM_NAME : shm_arg->second;
    size_t bytes = 0;
    w1700k_stream_header *ring = w1700k_stream_open(shm.c_str(), /*writable=*/0, &bytes, nullptr);
    if (!ring) return found;
    const bool alive = w1700k_stream_alive(ring);
    munmap(ring, bytes);
    if (!alive) return found;

    // A search names a band (band=N) to list that band only, or band=all for the all-antenna device.
    const auto band_arg = args.find("band");
    const std::string wanted = band_arg == args.end() ? "" : band_arg->second;
    if (wanted == "all") {
        SoapySDR::Kwargs device;
        device["driver"] = "w1700k";
        device["shm"] = shm;
        device["band"] = "all";
        device["label"] = "W1700K";
        found.push_back(device);
        return found;
    }
    for (int band = 0; band < w1700k::kBandCount; band++) {
        if (!wanted.empty() && wanted != std::to_string(band)) continue;
        SoapySDR::Kwargs device;
        device["driver"] = "w1700k";
        device["shm"] = shm;
        device["band"] = std::to_string(band);
        device["label"] = std::string("W1700K ") + w1700k::kBandNames[band];
        found.push_back(device);
    }
    return found;
}

SoapySDR::Device *make(const SoapySDR::Kwargs &args) { return new SoapyW1700K(args); }

SoapySDR::Registry registration("w1700k", &find, &make, SOAPY_SDR_ABI_VERSION);

SoapySDR::ModuleVersion version(W1700K_MODULE_VERSION);

}  // namespace
