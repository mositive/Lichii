// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "common/common_types.h"

// Experiment switches that can change while a game runs, for A/B measurements in one scene.
// EDEN_NX2_LIVE_FILE=<path> names a text file of NAME=0 or NAME=1 lines; a background thread
// re-reads it twice a second. A switch missing from the file (or with no file) uses its
// environment variable as before, so a launcher's settings still apply.
namespace VideoCommon::Nx2Live {

struct State {
    std::mutex mutex;
    std::map<std::string, bool> values;
    std::atomic<u64> generation{1};
    std::filesystem::file_time_type stamp{};
    std::string path;
    bool started{};
};

inline State& GetState() {
    static State state;
    return state;
}

inline void Reload(State& state) {
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(state.path, error);
    if (error || stamp == state.stamp) {
        return;
    }
    std::map<std::string, bool> values;
    std::ifstream file{state.path};
    for (std::string line; std::getline(file, line);) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq + 1 >= line.size()) {
            continue;
        }
        values[line.substr(0, eq)] = line[eq + 1] == '1';
    }
    std::scoped_lock lock{state.mutex};
    state.values = std::move(values);
    state.stamp = stamp;
    state.generation.fetch_add(1, std::memory_order_release);
}

inline void StartOnce() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (state.started) {
        return;
    }
    state.started = true;
    const char* path = std::getenv("EDEN_NX2_LIVE_FILE");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    state.path = path;
    std::thread{[&state] {
        for (;;) {
            Reload(state);
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
        }
    }}.detach();
}

/// One switch, kept at its call site: Get() is an atomic load unless the file changed.
class Flag {
public:
    explicit Flag(const char* name_) : name{name_} {
        const char* value = std::getenv(name_);
        env_value = value != nullptr && value[0] == '1';
        value_cache.store(env_value, std::memory_order_relaxed);
        StartOnce();
    }

    [[nodiscard]] bool Get() {
        State& state = GetState();
        const u64 generation = state.generation.load(std::memory_order_acquire);
        if (generation != seen.load(std::memory_order_relaxed)) {
            std::scoped_lock lock{state.mutex};
            const auto it = state.values.find(name);
            value_cache.store(it != state.values.end() ? it->second : env_value,
                              std::memory_order_relaxed);
            seen.store(generation, std::memory_order_relaxed);
        }
        return value_cache.load(std::memory_order_relaxed);
    }

private:
    std::string name;
    bool env_value{};
    std::atomic<bool> value_cache{};
    std::atomic<u64> seen{};
};

} // namespace VideoCommon::Nx2Live
