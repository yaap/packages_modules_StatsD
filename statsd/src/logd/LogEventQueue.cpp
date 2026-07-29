/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "LogEventQueue.h"

#include <com_android_os_statsd_flags.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "utils/api_tracing.h"

namespace android {
namespace os {
namespace statsd {

using std::unique_ptr;
using namespace std::chrono_literals;

namespace flags = com::android::os::statsd::flags;

namespace {

// Cooldown duration for the Perfetto trigger, one day
constexpr int64_t K_TRIGGER_COOLDOWN_NS = 24LL * 60 * 60 * 1000000000;
// Bucket size for the queue size histogram
constexpr size_t kQueueSizeBucketSize = 5000;
constexpr char kQueueSizeCounterName[] = "Statsd::EventQueueSizeBucket";
constexpr int32_t kTriggerPerfettoQueueSize = 20000;

int64_t getNowTimeNs() {
    return getElapsedRealtimeNs();
}

void runTriggerPerfettoImpl() {
    ATRACE_CALL();
    ALOGI("Triggering Perfetto for statsd queue overflow");

    pid_t pid = fork();

    if (pid < 0) {
        ALOGE("Fork failed: %m");
        return;
    }

    if (pid == 0) {
        // --- CHILD ---
        const char* args[] = {"/system/bin/trigger_perfetto", "android.os.statsd.queue-overflow",
                              NULL};
        execv(args[0], const_cast<char**>(args));
        // execv only returns on error
        ALOGE("execv trigger_perfetto failed: %m");
        _exit(127);
    }

    // --- PARENT ---
    // Wait for the child so we don't create a zombie. This part is blocking,
    // but this function will be called asynchronously, so it's ok to wait.
    int status;
    if (TEMP_FAILURE_RETRY(waitpid(pid, &status, 0)) < 0) {
        ALOGE("Failed to waitpid for trigger perfetto %m");
    } else {
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0) {
                ALOGE("trigger_perfetto exited with code %d", WEXITSTATUS(status));
            } else {
                ALOGI("trigger_perfetto completed successfully");
            }
        } else if (WIFSIGNALED(status)) {
            ALOGW("trigger_perfetto terminated by signal %d", WTERMSIG(status));
        }
    }
}

void executePerfettoTriggerAsync() {
    std::thread([]() { runTriggerPerfettoImpl(); }).detach();
}

}  // namespace

RateLimitedAsyncTrigger LogEventQueue::sRateLimitedPerfettoTrigger(K_TRIGGER_COOLDOWN_NS,
                                                                   getNowTimeNs,
                                                                   executePerfettoTriggerAsync);

unique_ptr<LogEvent> LogEventQueue::waitPop() {
    std::unique_lock<std::mutex> lock(mMutex);

    while (mQueue.empty()) {
        if (flags::use_wait_for()) {
            mCondition.wait_for(lock, 2s, [this] { return !this->mQueue.empty(); });
        } else {
            mCondition.wait(lock, [this] { return !this->mQueue.empty(); });
        }
    }

    unique_ptr<LogEvent> item = std::move(mQueue.front());
    mQueue.pop();

    int bucket = mQueue.size() / kQueueSizeBucketSize;
    if (bucket != mLastReportedBucket) {
        ATRACE_INT(kQueueSizeCounterName, bucket);
        mLastReportedBucket = bucket;
    }
    return item;
}

LogEventQueue::Result LogEventQueue::push(unique_ptr<LogEvent> item) {
    Result result;
    {
        std::lock_guard lock(mMutex);
        if (mQueue.size() < mQueueLimit) {
            mQueue.push(std::move(item));
            result.success = true;
            if (mIsOverflowing) {
                //  Overflow just ended
                ATRACE_END();
                ATRACE_INT("Statsd::EventQueueOverflowEnded", mOverflowLostCount);
                mIsOverflowing = false;
                mOverflowLostCount = 0;
            }
        } else {
            // safe operation as queue must not be empty.
            result.oldestTimestampNs = mQueue.front()->GetElapsedTimestampNs();
            result.success = false;

            if (!mIsOverflowing) {
                //  Overflow just started
                ATRACE_BEGIN("Statsd::QueueOverflow");
                mIsOverflowing = true;
                mOverflowLostCount = 0;
            }
            mOverflowLostCount++;
        }

        result.size = mQueue.size();
        int bucket = result.size / kQueueSizeBucketSize;
        if (bucket != mLastReportedBucket) {
            ATRACE_INT(kQueueSizeCounterName, bucket);
            mLastReportedBucket = bucket;
        }

        if (flags::trigger_perfetto()) {
            if (result.size > kTriggerPerfettoQueueSize) {
                sRateLimitedPerfettoTrigger.trigger();
            }
        }
    }

    mCondition.notify_one();
    return result;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
