// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <sys/syscall.h>

static constexpr int kWorkerCount = 6;
static constexpr int kHotWorkerCount = 2;

static std::atomic<bool> gKeepRunning(true);
static std::atomic<pid_t> gWorkerTids[kWorkerCount];
static volatile uint64_t gSinks[kWorkerCount] = {0};

static void signalHandler(int) {
    gKeepRunning.store(false);
}

static uint64_t nowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL) +
           static_cast<uint64_t>(ts.tv_nsec);
}

static pid_t getThreadId() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

static void cpuBurnWorker(int workerId, uint64_t seed) {
    gWorkerTids[workerId].store(getThreadId());

    uint64_t x = seed;
    volatile uint64_t localSink = 0;

    while(gKeepRunning.load()) {
        int iterations = (workerId < kHotWorkerCount) ? 100000 : 1000;
        for(int i = 0; i < iterations; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            localSink += x * 2654435761ULL;
        }

        // Keep four non-hot threads alive but low-duty so the top two dominate.
        if(workerId >= kHotWorkerCount) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    gSinks[workerId] = localSink;
}

int main(int argc, char** argv) {
    int durationSec = 0;
    if(argc > 1) {
        durationSec = std::atoi(argv[1]);
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    const pid_t pid = getpid();

    std::thread workers[kWorkerCount];
    for(int i = 0; i < kWorkerCount; i++) {
        gWorkerTids[i].store(0);
        workers[i] = std::thread(cpuBurnWorker, i, 0x123456789abcdefULL + (0x111111111111111ULL * i));
    }

    bool allStarted = false;
    while(!allStarted) {
        allStarted = true;
        for(int i = 0; i < kWorkerCount; i++) {
            if(gWorkerTids[i].load() == 0) {
                allStarted = false;
                break;
            }
        }
        if(!allStarted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    std::cout << "DoubleThreadCpuBurn started\n"
              << "PID: " << pid << "\n";
    for(int i = 0; i < kWorkerCount; i++) {
        std::cout << "Worker TID " << i << (i < kHotWorkerCount ? " (hot): " : " (low-duty): ")
                  << gWorkerTids[i].load() << "\n";
    }
    std::cout << "Duration: " << (durationSec > 0 ? std::to_string(durationSec) + " seconds" : "until SIGINT/SIGTERM") << "\n"
              << std::flush;

    const uint64_t startNs = nowNs();
    const uint64_t durationNs = static_cast<uint64_t>(durationSec) * 1000000000ULL;

    while(gKeepRunning.load()) {
        if(durationSec > 0 && nowNs() - startNs >= durationNs) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    gKeepRunning.store(false);

    for(int i = 0; i < kWorkerCount; i++) {
        workers[i].join();
    }

    std::cout << "DoubleThreadCpuBurn exiting";
    for(int i = 0; i < kWorkerCount; i++) {
        std::cout << ", sink" << i << "=" << gSinks[i];
    }
    std::cout << "\n";
    return 0;
}
