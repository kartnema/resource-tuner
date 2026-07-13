// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <mutex>
#include <memory>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>

#include "Logger.h"
#include "AuxRoutines.h"
#include "BoostManager.h"
#include "CgroupController.h"
#include "UrmPlatformAL.h"
#include "SignalInternal.h"

// Configurations
// Timer Interval
#define CLASSIFY_TICK_MS        500

// DT Rotation Interval
#define DT_ROTATE_INTERVAL_MS   1000

// Boost Cluster
#define BOOST_CLUSTER_COUNT  6
static int boostCluster[] = {12,13,14,15,16,17};

// Preferred DT Cores
#define DT_FAVORED_CORE_COUNT  2
static int favoredCoresDT[] = {13, 16};

#define ST_ENTER_THRESH 80
#define ST_RETAIN_THRESH 70
#define DT_ENTER_THRESH 85
#define DT_RETAIN_THRESH 75
#define CLASS_TRANSITION_CONFIRM_TICKS 3

#define URM_INVALID_HANDLE -1
static int64_t restuneHandle = URM_INVALID_HANDLE;

// Cgroup paths
static const char *focusedCgroup =
    "/sys/fs/cgroup/thread-root/nonhot";
static const char *boostCgroup =
    "/sys/fs/cgroup/thread-root/hot";
static const char *systemSliceCgroup =
    "/sys/fs/cgroup/system.slice";
static const char *userSliceCgroup =
    "/sys/fs/cgroup/user.slice";

// State
static AppTrackerCtx gAppTracker;
static BoostManagerCtx gBoostMgr;
static enum WorkloadType prevClass = APP_IDLE;
static enum WorkloadType pendingClass = APP_IDLE;
static int pendingClassCount = 0;
static std::thread gWorkloadDetectionThread;
static std::atomic<bool> gWorkloadDetectionTimerRunning(false);

#define URM_SIG_ST_DETECTED 0x00800010
#define URM_SIG_DT_DETECTED 0x00800011

#define WQ_DIR_PATH     "/sys/devices/virtual/workqueue/"
#define IRQ_DIR_PATH    "/proc/irq/"

static uint32_t getSigCodeForWorkload(enum WorkloadType type) {
    switch (type) {
        case APP_ST: 
            return URM_SIG_ST_DETECTED;
        case APP_DT: 
            return URM_SIG_DT_DETECTED;
        case APP_IDLE:
        case APP_MT:
            return 0;
    }

    return 0;
}

static bool classificationTransitionConfirmed(enum WorkloadType rawClass) {
    if(rawClass == prevClass) {
        pendingClass = rawClass;
        pendingClassCount = 0;
        return false;
    }

    if(rawClass != pendingClass) {
        pendingClass = rawClass;
        pendingClassCount = 1;
    } else {
        pendingClassCount++;
    }

    return pendingClassCount >= CLASS_TRANSITION_CONFIRM_TICKS;
}

static void workloadDetectionTick(void) {
    struct classify_result result;
    classifyCgroups(&gAppTracker, focusedCgroup, boostCgroup, &result);

    bool stateChanged = classificationTransitionConfirmed(result.type);

    LOGE("BOOST_DETECTION",
         "classification completed, raw result is: " + std::to_string(result.type) +
         ", stable result is: " + std::to_string(prevClass) +
         ", pending count is: " + std::to_string(pendingClassCount));

    if(stateChanged) {
        /* Apply boost placement policy only after confirmed classification transition */
        LOGE("BOOST_DETECTION", "classification transition confirmed, applying boost");
        boostManagerApply(&gBoostMgr, &result);

        // Untune previous signal
        if (restuneHandle != URM_INVALID_HANDLE) {
            // untuneSignal(restuneHandle);
            restuneHandle = URM_INVALID_HANDLE;
        }

        // Tune new signal for ST or DT
        LOGE("BOOST_DETECTION", "calling getSigCodeForWorkload");
        uint32_t sigCode = getSigCodeForWorkload(result.type);
        if(sigCode != 0) {
            restuneHandle = acquireSignal(
                sigCode,
                DEFAULT_SIGNAL_TYPE,
                getpid(),
                getpid()
            );
        }

        prevClass = result.type;
        pendingClass = prevClass;
        pendingClassCount = 0;
    }

    /* Rotate only stable ST; DT keeps fixed placement on favored cores. */
    if(!stateChanged && prevClass == APP_ST) {
        boost_manager_rotate_tick(&gBoostMgr);
    }
}

static void workloadDetectionTimerLoop() {
    while(gWorkloadDetectionTimerRunning.load()) {
        workloadDetectionTick();
        std::this_thread::sleep_for(std::chrono::milliseconds(CLASSIFY_TICK_MS));
    }
}

static void setCgroupCpuset(const char *cgPath, const char *cpuset) {
    std::string cpusetPath = std::string(cgPath) + "/cpuset.cpus";
    std::ofstream file(cpusetPath, std::ios::out | std::ios::trunc);
    if(file.is_open()) {
        file << cpuset;
        file.close();
    }
}

// cpumask to hex
static std::string cpuMaskToHex(uint64_t mask) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llx",
             (unsigned long long)mask);
    return std::string(buf);
}

static void workqueueApplier() {
    uint64_t mask = 0;
    int32_t cores[] = {0, 1, 2, 3, 4, 5};
    for(int32_t id: cores) {
        mask |= ((uint64_t)1 << id);
    }

    std::string maskStr = cpuMaskToHex(mask);

    DIR* dir = opendir(WQ_DIR_PATH);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue; // skip . and ..
        std::string cpumaskFile = std::string(WQ_DIR_PATH) + entry->d_name + "/cpumask";
        if(AuxRoutines::fileExists(cpumaskFile)) {
            AuxRoutines::writeToFile(cpumaskFile, maskStr);
        }
    }
    closedir(dir);
}

static void irqAffinityApplierCallback() {
    uint64_t mask = 0;
    int32_t cores[] = {0, 1, 2, 3, 4, 5};
    for(int32_t id: cores) {
        mask |= ((uint64_t)1 << id);
    }

    std::string maskStr = cpuMaskToHex(mask);

    DIR* dir = opendir(IRQ_DIR_PATH);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // numeric directories only
        bool numeric = true;
        for (const char* p = entry->d_name; *p; ++p) {
            if (!std::isdigit(static_cast<unsigned char>(*p))) { numeric = false; break; }
        }
        if (!numeric) continue;

        std::string smpFile = std::string(IRQ_DIR_PATH) + entry->d_name + "/smp_affinity";
        if(AuxRoutines::fileExists(smpFile)) {
            AuxRoutines::writeToFile(smpFile, maskStr);
        }
    }
    closedir(dir);
}

void initWLDetection() {
    if(gWorkloadDetectionTimerRunning.load()) {
        return;
    }

    // initialize App Tracker
    initTracker(
        &gAppTracker,
        CLASSIFY_TICK_MS, // window_ms / timer-duration
        ST_ENTER_THRESH,  // thresh for being classifier as ST
        ST_RETAIN_THRESH, // thresh for retaining ST classification
        DT_ENTER_THRESH,  // thresh for being classified as DT
        DT_RETAIN_THRESH  // thresh for retaining DT classification
    ); 

    // Initialize boost manager
    BoostConfig bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.boostCgroupPath        = boostCgroup;
    bcfg.focusedCgroupPath      = focusedCgroup;
    bcfg.favoredCoresDT[0]      = favoredCoresDT[0];
    bcfg.favoredCoresDT[1]      = favoredCoresDT[1];
    bcfg.dt_favored_core_count  = DT_FAVORED_CORE_COUNT;
    bcfg.boostClusterCores[0]   = boostCluster[0];
    bcfg.boostClusterCores[1]   = boostCluster[1];
    bcfg.boostClusterCores[2]   = boostCluster[2];
    bcfg.boostClusterCores[3]   = boostCluster[3];
    bcfg.boostClusterCores[4]   = boostCluster[4];
    bcfg.boostClusterCores[5]   = boostCluster[5];
    bcfg.boostClusterCoreCount  = BOOST_CLUSTER_COUNT;
    bcfg.dt_rotate_interval_ms  = DT_ROTATE_INTERVAL_MS;
    bcfg.tick_interval_ms       = CLASSIFY_TICK_MS;

    initBoostManager(&gBoostMgr, &bcfg);

    /* Keep non-URM system/user work off the boost cores */
    setCgroupCpuset(systemSliceCgroup, "0-5");
    setCgroupCpuset(userSliceCgroup, "0-5");

    // Affine workqueues on M
    workqueueApplier();

    // Affine IRQs on M
    irqAffinityApplierCallback();

    /* Start periodic workload detection timer */
    if(!gWorkloadDetectionTimerRunning.load()) {
        gWorkloadDetectionTimerRunning.store(true);
        gWorkloadDetectionThread = std::thread(workloadDetectionTimerLoop);
    }
}

void tearWLDetection() {
    // Stop periodic workload detection timer
    gWorkloadDetectionTimerRunning.store(false);
    if(gWorkloadDetectionThread.joinable()) {
        gWorkloadDetectionThread.join();
    }

    // Untune any held signal
    if(restuneHandle != URM_INVALID_HANDLE) {
        // untuneSignal(restuneHandle);
        restuneHandle = URM_INVALID_HANDLE;
    }

    /* Boost reset: move threads back to focused-group, remove isolation */
    boostManagerReset(&gBoostMgr);

    /* Cleanup classifier */
    tearTracker(&gAppTracker);
}
