// Captures the audio one program (and its child processes) plays, like OBS "Application Audio
// Capture": WASAPI process loopback, Windows 10 2004 or newer. It can also capture everything
// the computer plays except the DAW itself. The capture thread writes into a ring that the
// plug-in's audio thread reads.
#pragma once

#include "ssbus/ring.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hearaside {

struct AppInfo {
    std::string exe;    // "chrome.exe" - what the plug-in remembers
    std::string name;   // "chrome" - what the list shows
};

class AppCapture {
public:
    enum class State { Idle, Starting, Running, NotRunning, Failed };   // values are shared with the Hub (SourceHeader::capture)

    AppCapture();
    ~AppCapture();

    // Programs that currently have an audio session on any output device (not this process).
    static std::vector<AppInfo> listAudioApps();
    // Top process of that program (the browser process, not its audio helper), 0 = not running.
    static uint32_t findRootProcess(const std::string& exe);

    // Message thread. excludeTree: capture everything except pid's process tree (pid = the DAW).
    void start(uint32_t pid, uint32_t sampleRate, bool excludeTree = false);
    void stop();
    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    uint32_t pid() const noexcept { return pid_; }
    bool targetAlive() const noexcept;   // the captured process still runs (cheap, no process snapshot)
    uint32_t packetFrames() const noexcept { return packetFrames_.load(std::memory_order_relaxed); }   // largest delivery per wake-up
    bool shortPeriod() const noexcept { return shortPeriod_.load(std::memory_order_relaxed); }         // Windows low-latency packets

    // audio thread
    const ssbus::ChannelRing& ring() const noexcept { return *ring_; }
    const std::atomic<uint64_t>& writePos() const noexcept { return writePos_; }

private:
    void run(uint32_t pid, uint32_t sampleRate, bool excludeTree);

    std::unique_ptr<ssbus::ChannelRing> ring_;
    std::atomic<uint64_t> writePos_{ 0 };
    std::atomic<State> state_{ State::Idle };
    std::atomic<bool> quit_{ false }, shortPeriod_{ false };
    std::atomic<uint32_t> packetFrames_{ 480 };
    std::thread thread_;
    uint32_t pid_ = 0;
    void* process_ = nullptr;   // HANDLE of the captured process (SYNCHRONIZE)
};

} // namespace hearaside
