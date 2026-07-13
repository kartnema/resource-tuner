// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <unistd.h>

static std::atomic<bool> gKeepRunning(true);

static void signalHandler(int) {
    gKeepRunning.store(false);
}

static uint64_t nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL) +
           static_cast<uint64_t>(ts.tv_nsec);
}

int main(int argc, char** argv) {
    int durationSec = 0;
    if(argc > 1) {
        durationSec = std::atoi(argv[1]);
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const pid_t pid = getpid();
    const pid_t tid = getpid(); // Single-threaded program, so PID == main TID on Linux.

    std::cout << "SingleThreadCpuBurn started\n"
              << "PID: " << pid << "\n"
              << "TID: " << tid << "\n"
              << "Duration: " << (durationSec > 0 ? std::to_string(durationSec) + " seconds" : "until SIGINT/SIGTERM") << "\n"
              << std::flush;

    const uint64_t startNs = nowNs();
    const uint64_t durationNs = static_cast<uint64_t>(durationSec) * 1000000000ULL;

    volatile uint64_t sink = 0;
    uint64_t x = 0x123456789abcdefULL;

    while(gKeepRunning.load()) {
        // CPU-intensive integer work. No sleeps, no I/O in the hot loop.
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        sink += x * 2654435761ULL;

        if(durationSec > 0 && nowNs() - startNs >= durationNs) {
            break;
        }
    }

    std::cout << "SingleThreadCpuBurn exiting, sink=" << sink << "\n";
    return 0;
}
