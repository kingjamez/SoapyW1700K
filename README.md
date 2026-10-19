# SoapyW1700K

A [SoapySDR](https://github.com/pothosware/SoapySDR) module for the W1700K software-defined radio,
so that SDR++, GNU Radio, `SoapySDRUtil`, GQRX-style tools and anything else built on SoapySDR can
use it (`driver=w1700k`).

The module is the radio's SoapySDR front end only. It needs the rest of the
[w1700k-sdr](https://github.com/kingjamez/w1700k-sdr) project running:

| piece | where | what it does |
|---|---|---|
| firmware on the unit | w1700k-sdr | turns the W1700K into a receive-only SDR that exports IQ over 10 GbE |
| `w1700k-stream` | w1700k-sdr, `host/stream` | receives the export and serves IQ to every program on the host through shared memory |
| `w1700k` | w1700k-sdr, `host/bin/w1700k` | controls the radio (band, LO, rate, filter, gain, start) over ssh |
| **this module** | here | reads the IQ from `w1700k-stream`, controls the radio through `w1700k` |

Follow the w1700k-sdr README to get the unit running and `w1700k-stream` serving first.

## Install

```sh
sudo apt install cmake g++ libsoapysdr-dev nlohmann-json3-dev
sudo make install                      # SoapySDR's module directory: every SoapySDR program finds it
SoapySDRUtil --find="driver=w1700k"    # needs w1700k-stream running
```

CMake works too: `cmake -S . -B build && cmake --build build && sudo cmake --install build`.

The module runs the `w1700k` command found, in this order, in the `cmd=` device argument,
`$W1700K_CMD`, the path it was built with (`make W1700K_CMD=/path/to/w1700k`, or
`cmake -DW1700K_CMD=...`; `w1700k` looked up in PATH by default), and `PATH`.

### Without root

`make install-user` installs into `~/.local/lib/SoapySDR/modules<ABI>`, which SoapySDR does **not**
search by default. Programs started from a shell need
`export SOAPY_SDR_PLUGIN_PATH=$HOME/.local/lib/SoapySDR/modules0.8` (put it in `~/.bashrc`), and
programs started from the desktop's app launcher do not read `~/.bashrc`: give the launcher's
`Exec=` line the variable, `Exec=env SOAPY_SDR_PLUGIN_PATH=/home/you/.local/lib/SoapySDR/modules0.8 sdrpp`
(write the home directory out; `.desktop` files do not expand `$HOME`). A system install avoids
all of this. If the W1700K is missing from a program's device list, this is the first thing to check:
`env -i HOME=$HOME PATH=/usr/bin:/bin SoapySDRUtil --find` shows what a launcher sees.

## Devices

Enumeration offers one device per band while `w1700k-stream` is running: **W1700K 2.4G**,
**W1700K 5G** and **W1700K 6G** (`band=0|1|2`). SoapySDR has one frequency range per channel and
none per antenna, so each band is a device of its own, with that band's tuning range as its range
and its name as its only antenna. A program that never asks for ranges (SDR++) then shows the right
band and limits, and the module puts the unit on the device's band before it tunes. `band=all` is
the device with all three antennas, whose range follows the antenna selected; a program that opens
by arguments (GNU Radio, scripts) should ask for it. A request that names no band
(`driver=w1700k` alone) is completed by SoapySDR with the first listed device's arguments, which is
the 2.4G device.

Device arguments: `band=0|1|2|all`, `flatten=true`, `shm=/w1700k-stream`, `cmd=/path/to/w1700k`.

| SoapySDR | W1700K |
|---|---|
| antenna `2.4G`, `5G`, `6G` | the band captured |
| frequency | the band's LO (a tune takes a few hundred ms) |
| sample rate 40, 80, 160, 320 Msps | the radio's rate (a running stream restarts at the new rate) |
| bandwidth 20, 40, 80, 160, 320 MHz; 0 | the analog filter; 0 is the filter as wide as the sample rate |
| gain `LNA` 0..4, `LPF` 0..9; gain mode | forced steps (not dB); AGC |
| setting `flatten` | the stream service's roll-off correction (about one more CPU core) |

- **Tuning range.** The unit measures the span each band's synthesizer locks over when it loads
  (it differs between units by tens of MHz). The module advertises that span inset by 5 MHz at both
  ends, because the top edge was found to be up to a few MHz lower than measured, and clamps a
  request outside it to the edge, with a warning in the log.
- **Gain.** With AGC on, gain settings are remembered and applied when AGC is turned off: the
  radio's gain command turns AGC off, and SDR++ sets every gain after the gain mode.
- **Formats.** CS16 (native, the radio's 14-bit samples, full scale 8192) and CF32 (full scale
  is 1.0; with `flatten` the correction can exceed it). Reads of any length are accepted.
- **Gaps.** No hardware time. A lost completion, a new run (the unit's stream restarted) or
  falling a ring behind is reported as `SOAPY_SDR_OVERFLOW`; samples read before it are returned first.
- **Errors.** A command to the radio that fails is logged (with the command's own explanation) and
  not thrown, because SDR++ does not catch exceptions from setters; the getters keep reporting the
  radio's real state. Misuse of the API (an antenna or gain that does not exist, a transmit stream)
  throws.

## SDR++

SDR++'s `--autostart` option can crash SDR++ itself with any source that delivers samples as fast
as this one: samples arrive before the first frame is laid out, the waterfall's height is still 0,
and its `memmove` gets a negative size. Press the play button instead.

## Things to know

- There is one radio and one state, on the unit. Two programs, or two of the three devices, open
  at once fight over the band, rate and LO.
- While a stream is active the module writes the radio's sample rate, capture rate, filter width
  and the `flatten` setting into the stream service's control words. They are shared by every
  consumer of the service, so the last writer wins.
- `deactivateStream` leaves the unit streaming (other programs may be reading the service).
- `include/w1700k_stream.h` is a copy of the service's interface header from w1700k-sdr; keep it in
  step with the service you run (it carries a layout version, which the module checks).

## Layout and tests

| file | |
|---|---|
| `src/SoapyW1700K.hpp` | the device class, and the design notes: paths, threading, errors |
| `src/Settings.cpp` | band, frequency, rate, filter, gain, settings |
| `src/Streaming.cpp` | the stream: shared-memory ring, seqlock reads, gap reporting |
| `src/Command.cpp` | running `w1700k` (timeout, process group), parsing its reply |
| `src/Registration.cpp` | enumeration and creation |

`W1700K_STREAM_BIN=/path/to/w1700k-stream python3 -m unittest discover -s tests` builds the module
and runs it, through a small SoapySDR client, against a real `w1700k-stream` fed a fake export and a
fake `w1700k` command. The service takes up to a minute to start (it plans its FFTs), so the tests
do too. Without the service binary they are skipped.

## Licence

MIT, see `LICENSE`.
