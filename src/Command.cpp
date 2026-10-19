// SPDX-License-Identifier: MIT
#include "Command.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace w1700k {

const char *const kBandNames[kBandCount] = {"2.4G", "5G", "6G"};

bool is_true(const std::string &text) { return text == "true" || text == "1" || text == "yes" || text == "on"; }

namespace {

using json = nlohmann::json;

// How long one `w1700k` command may take. A tune or a rate change takes well under a second; the
// command's own ssh timeouts end a dead unit in about ten. This only stops a hang.
constexpr std::chrono::seconds kCommandTimeout{30};

// Reads whatever is available on `fd` into `into`. Returns false at end of file.
bool drain(int fd, std::string &into)
{
    char buffer[4096];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n > 0) {
            into.append(buffer, size_t(n));
            return true;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}

// The last non-empty line of a command's stderr: where `w1700k` prints why it failed.
std::string last_line(const std::string &text)
{
    const size_t end = text.find_last_not_of(" \r\n");
    if (end == std::string::npos) return "";
    const size_t newline = text.find_last_of('\n', end);
    const size_t from = newline == std::string::npos ? 0 : newline + 1;
    return text.substr(from, end + 1 - from);
}

}  // namespace

int run_process(const std::vector<std::string> &argv, std::chrono::seconds timeout, std::string &out,
                std::string &err)
{
    out.clear();
    err.clear();
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC)) return -1;
    if (pipe2(err_pipe, O_CLOEXEC)) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return -1;
    }

    // The child gets the write ends as stdout and stderr and /dev/null as stdin. The pipes were
    // created close-on-exec, so no other descriptor of ours leaks into it (dup2 clears the flag on
    // the copy).
    // The child leads a process group of its own, so that a timeout can kill whatever it started
    // (ssh) along with it.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2);

    std::vector<char *> args;
    for (const std::string &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, args[0], &actions, &attributes, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (spawned) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        err = std::string("cannot run ") + argv[0];
        return -1;
    }

    // Collect both streams until the child closes them, or kill it at the deadline.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    struct pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    bool timed_out = false;
    while (fds[0].fd >= 0 || fds[1].fd >= 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            timed_out = true;
            break;
        }
        const int ready = poll(fds, 2, int(left.count()));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        for (int i = 0; i < 2; i++) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (!drain(fds[i].fd, i == 0 ? out : err)) {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
    }
    for (struct pollfd &f : fds)
        if (f.fd >= 0) close(f.fd);
    if (timed_out) kill(-pid, SIGKILL);

    // The child normally exits as it closes its pipes. One that closed them and keeps running is
    // waited for only until the same deadline.
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= deadline && !timed_out) {
            timed_out = true;
            kill(-pid, SIGKILL);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (timed_out) {
        err = "timed out after " + std::to_string(timeout.count()) + " s";
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void parse_reply(const std::string &json_text, RadioState &state)
{
    const json reply = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (!reply.is_object()) throw std::runtime_error("the w1700k command's reply is not JSON");
    if (reply.contains("error") && reply["error"].is_string()) throw std::runtime_error(reply["error"].get<std::string>());

    // Work on a copy, so a reply with a malformed field leaves the state as it was.
    RadioState next = state;
    try {
        const json radio = reply.value("radio", json::object());
        const json stream = reply.value("stream", json::object());
        const json rf = reply.value("rf", json::object());

        next.band = radio.value("band", next.band);
        if (next.band < 0 || next.band >= kBandCount) throw std::runtime_error("band out of range");
        next.lo_hz = radio.value("lo_hz", next.lo_hz);
        next.sample_rate = radio.value("sample_rate", next.sample_rate);
        next.capture_rate = radio.value("capture_rate", next.capture_rate);
        next.streaming = stream.value("running", next.streaming);
        next.filter_hz = rf.value("filter_mhz", next.filter_hz / 1e6) * 1e6;
        next.agc = rf.value("agc", next.agc);
        if (rf.contains("lna")) next.lna = rf["lna"].is_number() ? rf["lna"].get<int>() : -1;
        if (rf.contains("lpf")) next.lpf = rf["lpf"].is_number() ? rf["lpf"].get<int>() : -1;

        // The lock span of every band; a host that predates `lo_ranges_hz` reports the active band's only.
        if (rf.contains("lo_ranges_hz") && rf["lo_ranges_hz"].is_object()) {
            for (int b = 0; b < kBandCount; b++) {
                const std::string key = std::to_string(b);
                if (!rf["lo_ranges_hz"].contains(key)) continue;
                const json &span = rf["lo_ranges_hz"][key];
                if (span.is_array() && span.size() == 2) {
                    next.lo_span_hz[b][0] = span[0].get<double>();
                    next.lo_span_hz[b][1] = span[1].get<double>();
                }
            }
        } else if (rf.contains("lo_range_hz") && rf["lo_range_hz"].is_array() && rf["lo_range_hz"].size() == 2) {
            next.lo_span_hz[next.band][0] = rf["lo_range_hz"][0].get<double>();
            next.lo_span_hz[next.band][1] = rf["lo_range_hz"][1].get<double>();
        }
    } catch (const json::exception &e) {
        throw std::runtime_error(std::string("unexpected reply from the w1700k command: ") + e.what());
    }
    state = next;
}

void Controller::run(const std::vector<std::string> &args)
{
    std::lock_guard<std::mutex> one_at_a_time(run_mutex_);
    std::vector<std::string> argv{command_};
    argv.insert(argv.end(), args.begin(), args.end());

    std::string out, err;
    const int status = run_process(argv, kCommandTimeout, out, err);
    if (status != 0) {
        const std::string why = last_line(err);
        throw std::runtime_error("'" + command_ + " " + args.front() + "' failed" +
                                 (status > 0 ? " (exit " + std::to_string(status) + ")" : "") +
                                 (why.empty() ? "" : ": " + why));
    }
    RadioState next = state();
    parse_reply(out, next);
    std::lock_guard<std::mutex> publish(state_mutex_);
    state_ = next;
}

void Controller::refresh() { run({"status", "--rf"}); }

RadioState Controller::state() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return state_;
}

}  // namespace w1700k
