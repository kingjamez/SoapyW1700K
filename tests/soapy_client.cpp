// SPDX-License-Identifier: MIT
// Test client for the SoapySDR module: drives it the way an application does and prints
// what it saw as JSON lines, for tests/test_soapy.py to check.
//
//   soapy_client enumerate [DEVICE-ARGS]
//   soapy_client info   DEVICE-ARGS               antennas, frequency range, rates
//   soapy_client tune   DEVICE-ARGS HZ            setFrequency (the module must not throw), then getFrequency
//   soapy_client stream DEVICE-ARGS CS16|CF32 SECONDS
//
// `stream` expects the slice pattern test_soapy.py sends: in slice s, sample n is I = s % 8192,
// Q = n % 2048 (raw 14-bit values): one ramp, continuous across slices.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void print_list(const char *key, const std::vector<std::string> &items)
{
    std::printf("\"%s\": [", key);
    for (size_t i = 0; i < items.size(); i++) std::printf("%s\"%s\"", i ? ", " : "", items[i].c_str());
    std::printf("]");
}

int stream(SoapySDR::Device *device, const std::string &format, double seconds)
{
    const bool cf32 = format == "CF32";
    SoapySDR::Stream *rx = device->setupStream(SOAPY_SDR_RX, cf32 ? SOAPY_SDR_CF32 : SOAPY_SDR_CS16);
    const int activated = device->activateStream(rx);
    if (activated != 0) {
        std::printf("{\"activate\": %d}\n", activated);
        return 1;
    }

    // 3000 samples is deliberately not a multiple of the slice (2048): reads end inside slices.
    const size_t block = 3000;
    std::vector<int16_t> cs16(2 * block);
    std::vector<float> cf(2 * block);
    void *buffers[1] = {cf32 ? static_cast<void *>(cf.data()) : static_cast<void *>(cs16.data())};

    long long samples = 0, overflows = 0, timeouts = 0, errors = 0, bad_q = 0, jumps = 0, boundaries = 0;
    int last_i = -1, last_q = -1;  // the last sample seen; -1 after a gap, where continuity is not checked
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
        int flags = 0;
        long long time_ns = 0;
        const int n = device->readStream(rx, buffers, block, flags, time_ns, 100000);
        if (n == SOAPY_SDR_OVERFLOW) { overflows++; last_i = last_q = -1; continue; }
        if (n == SOAPY_SDR_TIMEOUT) { timeouts++; continue; }
        if (n < 0) { errors++; break; }
        for (int k = 0; k < n; k++) {
            int i, q;
            if (cf32) {
                i = int(std::lround(cf[2 * k] * 8192));
                q = int(std::lround(cf[2 * k + 1] * 8192));
            } else {
                i = cs16[2 * k];
                q = cs16[2 * k + 1];
            }
            if (last_q >= 0) {
                if (q != (last_q + 1) % 2048) bad_q++;  // the ramp is continuous across slices
                if (q == 0) {                           // a slice boundary: the slice number steps by one
                    if (i == (last_i + 1) % 8192) boundaries++;
                    else jumps++;
                } else if (i != last_i) {
                    bad_q++;                            // I is constant within a slice
                }
            }
            last_i = i;
            last_q = q;
        }
        samples += n;
    }
    device->deactivateStream(rx);
    device->closeStream(rx);
    std::printf("{\"samples\": %lld, \"overflows\": %lld, \"timeouts\": %lld, \"errors\": %lld, \"jumps\": %lld, "
                "\"boundaries\": %lld, \"bad_q\": %lld}\n",
                samples, overflows, timeouts, errors, jumps, boundaries, bad_q);
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const std::string mode = argv[1];
    if (mode == "enumerate") {
        for (const auto &found : SoapySDR::Device::enumerate(argc > 2 ? argv[2] : "driver=w1700k"))
            std::printf("{\"label\": \"%s\", \"band\": \"%s\"}\n", found.at("label").c_str(), found.at("band").c_str());
        return 0;
    }
    if (argc < 3) return 2;
    SoapySDR::Device *device = SoapySDR::Device::make(argv[2]);
    int status = 0;
    if (mode == "info") {
        std::printf("{");
        print_list("antennas", device->listAntennas(SOAPY_SDR_RX, 0));
        const auto range = device->getFrequencyRange(SOAPY_SDR_RX, 0).at(0);
        std::printf(", \"range\": [%.0f, %.0f], \"antenna\": \"%s\", \"rates\": [", range.minimum(), range.maximum(),
                    device->getAntenna(SOAPY_SDR_RX, 0).c_str());
        const auto rates = device->listSampleRates(SOAPY_SDR_RX, 0);
        for (size_t i = 0; i < rates.size(); i++) std::printf("%s%.0f", i ? ", " : "", rates[i]);
        std::printf("]}\n");
    } else if (mode == "tune" && argc == 4) {
        device->setFrequency(SOAPY_SDR_RX, 0, std::atof(argv[3]));
        std::printf("{\"frequency\": %.0f}\n", device->getFrequency(SOAPY_SDR_RX, 0));
    } else if (mode == "stream" && argc == 5) {
        status = stream(device, argv[3], std::atof(argv[4]));
    } else {
        status = 2;
    }
    SoapySDR::Device::unmake(device);
    return status;
}
