# SPDX-License-Identifier: MIT
"""The SoapySDR module against a real w1700k-stream (from the w1700k-sdr project) fed a fake export over
loopback, and a fake `w1700k` command: enumeration, per-band devices and their tuning ranges, the band
being selected before a tune, failures logged rather than thrown, and the stream delivered bit-exact, in
order, with a lost slice reported as an overflow.

Needs the stream service's binary: set W1700K_STREAM_BIN to it, or have `w1700k-stream` in PATH; the
tests are skipped without it. The service takes up to a minute to start (it plans its FFTs)."""
import json
import os
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import threading
import time
import unittest

from stream_shm import SLICE, Shm

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_DIR = os.path.join(ROOT, "build")
CLIENT_SOURCE = os.path.join(ROOT, "tests", "soapy_client.cpp")
STREAM_BINARY = os.environ.get("W1700K_STREAM_BIN") or shutil.which("w1700k-stream")
MAGIC = 0x31524453

# A unit's reply to `w1700k status --rf`: the radio on band 1 (5G), with the lock span of each band.
STATE = {
    "radio": {"loaded": True, "band": 1, "sample_rate": 320000000, "capture_rate": 320000000, "lo_hz": 5250000000},
    "stream": {"running": True},
    "rf": {"lo_hz": 5250000000, "agc": True, "lna": None, "lpf": None, "filter_mhz": 320,
           "lo_range_hz": [4679125000, 7545562194],
           "lo_ranges_hz": {"0": [2070000000, 2729703125], "1": [4679125000, 7545562194],
                            "2": [4661118712, 7519756072]}},
    "error": None,
}

# A stand-in for the `w1700k` command: logs its arguments, applies the few commands the module sends to a
# state file, prints the state. FAKE_FAIL=<command> makes that command fail the way the real one does.
FAKE_COMMAND = r'''#!/usr/bin/env python3
import json, os, sys
state_path, log_path = os.environ["FAKE_STATE"], os.environ["FAKE_LOG"]
args = sys.argv[1:]
with open(log_path, "a") as log:
    log.write(" ".join(args) + "\n")
if os.environ.get("FAKE_FAIL") == args[0]:
    sys.stderr.write("error: band 1 synthesizer unlocked: the VCO did not lock\n")
    sys.exit(1)
state = json.load(open(state_path))
if args[0] == "tune":
    state["radio"]["lo_hz"] = state["rf"]["lo_hz"] = int(args[1])
elif args[0] == "band":
    state["radio"]["band"] = int(args[1])
    if "--rate" in args:
        state["radio"]["sample_rate"] = state["radio"]["capture_rate"] = int(args[args.index("--rate") + 1]) * 1000000
elif args[0] == "start":
    state["stream"]["running"] = True
json.dump(state, open(state_path, "w"))
print(json.dumps(state))
'''


def build_problem():
    for tool in ("make", os.environ.get("CXX", "c++")):
        if not shutil.which(tool):
            return "no " + tool
    probe = subprocess.run([os.environ.get("CXX", "c++"), "-x", "c++", "-fsyntax-only", "-"],
                           input="#include <SoapySDR/Device.hpp>\n#include <nlohmann/json.hpp>\n", text=True,
                           capture_output=True)
    if probe.returncode:
        return "SoapySDR or nlohmann-json headers missing"
    if not STREAM_BINARY or not os.path.exists(STREAM_BINARY):
        return "w1700k-stream not found: set W1700K_STREAM_BIN"
    return None


def free_udp_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


class Sender(threading.Thread):
    """Sends the export's slices without end, at about 4000 a second: slice s is I = s % 8192 and Q = the
    sample's index in the slice, so a reader can check order and continuity. With `lose_every` a slice
    of every that many is never sent (a lost slice, wherever a reader happens to start); `event` is
    sent as a firmware event."""

    def __init__(self, port, lose_every=None, event=None):
        super().__init__(daemon=True)
        self.port, self.lose_every, self.event = port, lose_every, event
        self.stop_flag = threading.Event()
        self.sent = 0

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        seq = 0
        while not self.stop_flag.is_set():
            if not (self.lose_every and seq % self.lose_every == self.lose_every // 2):
                if seq == self.event:
                    payload = b"\x00" * 64
                else:
                    payload = struct.pack("<%dI" % SLICE, *[((seq % 8192) << 16) | n for n in range(SLICE)])
                sock.sendto(struct.pack("<IHHQ", MAGIC, 0, len(payload), seq) + payload, ("127.0.0.1", self.port))
            seq += 1
            self.sent = seq
            if seq % 16 == 0:
                time.sleep(0.004)
        sock.close()


@unittest.skipIf(build_problem(), build_problem())
class SoapyModuleTest(unittest.TestCase):
    counter = 0

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make", "-C", ROOT], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError("build failed:\n" + result.stdout + result.stderr)
        cls.tmp = tempfile.mkdtemp(prefix="soapy-test-")
        cls.client = os.path.join(cls.tmp, "soapy_client")
        result = subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-O1", CLIENT_SOURCE,
                                 "-lSoapySDR", "-o", cls.client], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError("client build failed:\n" + result.stderr)
        cls.fake = os.path.join(cls.tmp, "w1700k")
        with open(cls.fake, "w") as f:
            f.write(FAKE_COMMAND)
        os.chmod(cls.fake, os.stat(cls.fake).st_mode | stat.S_IXUSR)

        # One service for all the tests: it takes a while to start (it plans its FFTs, about a minute on
        # a busy machine). A new test's sender starts a new run, which the service handles by itself.
        cls.name = "/w1700k-soapy-test-%d" % os.getpid()
        cls.port = free_udp_port()
        cls.service = subprocess.Popen([STREAM_BINARY, "--bind", "127.0.0.1", "--port", str(cls.port), "--name", cls.name,
                                        "--ring-mib", "16", "--workers", "1", "--rx-threads", "2"],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 300
        while True:
            try:
                cls.shm = Shm(cls.name)
                break
            except (OSError, AssertionError):
                if time.monotonic() > deadline or cls.service.poll() is not None:
                    cls.service.kill()
                    raise AssertionError("w1700k-stream did not start")
                time.sleep(0.2)

    @classmethod
    def tearDownClass(cls):
        cls.shm.close()
        cls.service.send_signal(signal.SIGINT)
        cls.service.wait(30)
        cls.service.stderr.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        SoapyModuleTest.counter += 1
        self.state_path = os.path.join(self.tmp, "state%d.json" % self.counter)
        self.log_path = os.path.join(self.tmp, "log%d.txt" % self.counter)
        with open(self.state_path, "w") as f:
            json.dump(STATE, f)
        open(self.log_path, "w").close()
        self.sender = None

    def tearDown(self):
        if self.sender:
            self.sender.stop_flag.set()
            self.sender.join()

    def start_sender(self, **kwargs):
        """Starts sending, and returns once the service publishes the new stream."""
        run, newest = self.shm.position()
        self.sender = Sender(self.port, **kwargs)
        self.sender.start()
        deadline = time.monotonic() + 15
        while True:
            now_run, now_newest = self.shm.position()
            if now_newest >= 200 and (now_run != run or now_newest > newest + 200):
                return
            self.assertLess(time.monotonic(), deadline, "the service published nothing")
            time.sleep(0.02)

    def run_client(self, *args, fail=None, timeout=60):
        env = dict(os.environ, SOAPY_SDR_PATH="/nonexistent", SOAPY_SDR_PLUGIN_PATH=MODULE_DIR,
                   W1700K_CMD=self.fake, FAKE_STATE=self.state_path, FAKE_LOG=self.log_path)
        env.pop("FAKE_FAIL", None)
        if fail:
            env["FAKE_FAIL"] = fail
        return subprocess.run([self.client, *args], env=env, capture_output=True, text=True, timeout=timeout)

    def device(self, band=None):
        # No band means all three antennas, which has to be asked for: SoapySDR completes a request
        # that names no band with the first listed device's.
        return "driver=w1700k,shm=%s,band=%s" % (self.name, "all" if band is None else band)

    def commands(self):
        with open(self.log_path) as f:
            return [line.split() for line in f.read().splitlines()]

    def test_one_device_per_band_while_the_service_runs(self):
        self.start_sender()
        result = self.run_client("enumerate", "driver=w1700k,shm=" + self.name)
        self.assertEqual(result.returncode, 0, result.stderr)
        found = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([(d["band"], d["label"]) for d in found],
                         [("0", "W1700K 2.4G"), ("1", "W1700K 5G"), ("2", "W1700K 6G")])
        # A search for one band lists that band only.
        result = self.run_client("enumerate", "driver=w1700k,band=2,shm=" + self.name)
        self.assertEqual([json.loads(line)["label"] for line in result.stdout.splitlines()], ["W1700K 6G"])
        # band=all is the device with every antenna, listed only when asked for.
        result = self.run_client("enumerate", "driver=w1700k,band=all,shm=" + self.name)
        self.assertEqual([json.loads(line)["label"] for line in result.stdout.splitlines()], ["W1700K"])

    def test_no_devices_without_the_service(self):
        result = self.run_client("enumerate", "driver=w1700k,shm=/w1700k-no-such-service")
        self.assertEqual((result.returncode, result.stdout), (0, ""))

    def test_a_band_device_has_its_antenna_and_its_range_inset_from_the_lock_span(self):
        self.start_sender()
        for band, name, span in ((0, "2.4G", (2070000000, 2729703125)), (1, "5G", (4679125000, 7545562194)),
                                 (2, "6G", (4661118712, 7519756072))):
            info = json.loads(self.run_client("info", self.device(band)).stdout)
            self.assertEqual((info["antennas"], info["antenna"]), ([name], name))
            self.assertEqual(info["range"], [span[0] + 5e6, span[1] - 5e6])
            self.assertEqual(info["rates"], [40e6, 80e6, 160e6, 320e6])
        info = json.loads(self.run_client("info", self.device()).stdout)
        self.assertEqual(info["antennas"], ["2.4G", "5G", "6G"])
        self.assertEqual(info["antenna"], "5G")  # the unit is on band 1

    def test_tune_is_clamped_to_the_bands_range_and_selects_the_band_first(self):
        self.start_sender()
        # Band 2's device, unit on band 1: the band is selected, then the (clamped) tune is sent.
        result = self.run_client("tune", self.device(2), "9000000000")
        self.assertEqual(json.loads(result.stdout)["frequency"], 7519756072 - 5e6)
        self.assertIn("outside antenna 6G", result.stderr)
        commands = self.commands()
        self.assertEqual([c[:2] for c in commands if c[0] in ("band", "tune")],
                         [["band", "2"], ["tune", str(7519756072 - 5000000)]])
        # In range: untouched.
        result = self.run_client("tune", self.device(2), "6000000000")
        self.assertEqual(json.loads(result.stdout)["frequency"], 6e9)
        # A device with every antenna follows the unit's band, which the steps above left on band 2.
        result = self.run_client("tune", self.device(), "1000000000")
        self.assertEqual(json.loads(result.stdout)["frequency"], 4661118712 + 5e6)

    def test_a_failed_command_is_logged_and_not_thrown(self):
        self.start_sender()
        result = self.run_client("tune", self.device(1), "6000000000", fail="tune")
        self.assertEqual(result.returncode, 0, "an exception escaped: " + result.stderr)
        self.assertIn("synthesizer unlocked", result.stderr)  # the command's own explanation
        self.assertEqual(json.loads(result.stdout)["frequency"], 5250000000)  # the radio did not move

    def test_the_stream_is_in_order_and_bit_exact_in_both_formats(self):
        self.start_sender(event=3000)
        for fmt in ("CS16", "CF32"):
            result = self.run_client("stream", self.device(1), fmt, "1.5")
            self.assertEqual(result.returncode, 0, result.stderr)
            seen = json.loads(result.stdout)
            self.assertGreater(seen["samples"], 1_000_000, fmt)
            self.assertEqual((seen["overflows"], seen["errors"], seen["jumps"], seen["bad_q"]), (0, 0, 0, 0), fmt)
            self.assertGreater(seen["boundaries"], 100, fmt)

    def test_a_lost_slice_is_an_overflow_and_the_samples_before_it_arrive(self):
        # One slice of every 2000 is never sent, so some are lost whenever the reader starts (it starts
        # at the live edge). Each is reported once; no jump in the slice numbers goes unreported.
        self.start_sender(lose_every=2000)
        result = self.run_client("stream", self.device(1), "CS16", "2.5")
        seen = json.loads(result.stdout)
        self.assertEqual((seen["errors"], seen["bad_q"]), (0, 0))
        # How many fall in the reading window depends on how fast a busy machine sends: at least one.
        self.assertGreaterEqual(seen["overflows"], 1)
        self.assertLessEqual(seen["overflows"], 8)
        self.assertEqual(seen["jumps"], 0, "no jump goes unreported")

    def test_the_module_writes_the_radio_state_for_the_services_correction(self):
        self.start_sender()
        self.run_client("stream", self.device(1), "CS16", "0.3")
        self.assertEqual((self.shm.get("radio_sample_rate"), self.shm.get("radio_capture_rate"),
                          self.shm.get("radio_filter_hz"), self.shm.get("flatten")),
                         (320000000, 320000000, 320000000, 0))
        self.run_client("stream", self.device(1) + ",flatten=true", "CS16", "0.3")
        self.assertEqual(self.shm.get("flatten"), 1)


if __name__ == "__main__":
    unittest.main()
