// SPDX-License-Identifier: MIT
#include <SoapySDR/Logger.hpp>

#include <cstdlib>
#include <stdexcept>
#include <unistd.h>

#include "SoapyW1700K.hpp"

namespace {

// The value of a device argument, or `fallback` if it is absent or empty.
std::string argument(const SoapySDR::Kwargs &args, const char *key, const std::string &fallback)
{
    const auto found = args.find(key);
    return found == args.end() || found->second.empty() ? fallback : found->second;
}

// The `w1700k` command: the cmd= argument, else $W1700K_CMD, else the path the module was built with
// (W1700K_CMD is defined by the build; `w1700k`, looked up in PATH, by default), else `w1700k` from
// PATH if that path does not exist on this machine.
std::string command_path(const SoapySDR::Kwargs &args)
{
    const char *environment = std::getenv("W1700K_CMD");
    if (environment && *environment) return argument(args, "cmd", environment);
    if (args.count("cmd") && !args.at("cmd").empty()) return args.at("cmd");
    const std::string compiled = W1700K_CMD;
    if (compiled.find('/') == std::string::npos || access(compiled.c_str(), X_OK) == 0) return compiled;
    return "w1700k";
}

}  // namespace

SoapyW1700K::SoapyW1700K(const SoapySDR::Kwargs &args)
    : controller_(command_path(args)),
      shm_name_(argument(args, "shm", W1700K_STREAM_NAME)),
      flatten_(w1700k::is_true(argument(args, "flatten", "false")))
{
    // band=0|1|2 fixes the device to a band; band=all (or no band=) leaves all three antennas.
    const std::string fixed = argument(args, "band", "all");
    if (fixed == "0" || fixed == "1" || fixed == "2") fixed_band_ = fixed[0] - '0';
    else if (fixed != "all") throw std::runtime_error("W1700K: band= is 0, 1, 2 or all, not '" + fixed + "'");

    // Learn the radio's state now, so that the first questions are answered from it. If the command
    // cannot be run this throws, and the application reports that the device cannot be opened.
    controller_.refresh();
    const w1700k::RadioState state = controller_.state();
    SoapySDR::logf(SOAPY_SDR_INFO, "W1700K: band %s, LO %.6f MHz, %.0f Msps", w1700k::kBandNames[band()],
                   state.lo_hz / 1e6, state.sample_rate / 1e6);
}

SoapyW1700K::~SoapyW1700K()
{
    active_ = false;
    std::lock_guard<std::mutex> reading(read_mutex_);  // wait for a read in progress
    unmap_stream();
}
