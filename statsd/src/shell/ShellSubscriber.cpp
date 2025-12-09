/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "ShellSubscriber.h"

#include <android-base/file.h>
#include <inttypes.h>
#include <utils/Timers.h>

#include "guardrail/StatsdStats.h"
#include "stats_log_util.h"
#include "utils/api_tracing.h"

namespace android {
namespace os {
namespace statsd {

using aidl::android::os::IStatsSubscriptionCallback;

using namespace std::chrono_literals;
using std::shared_ptr;
using std::unique_ptr;
using std::vector;

ShellSubscriber::~ShellSubscriber() {
    {
        std::lock_guard lock(mMutex);
        mClientSet.clear();
        updateAtomIdsInUseLocked();
    }
    mThreadSleepCV.notify_one();
    if (mThread.joinable()) {
        mThread.join();
    }
}

bool ShellSubscriber::startNewSubscription(int in, int out, int64_t timeoutSec) {
    std::lock_guard lock(mMutex);
    VLOG("ShellSubscriber: new subscription has come in");
    if (mClientSet.size() >= kMaxSubscriptions) {
        ALOGE("ShellSubscriber: cannot have another active subscription. Current Subscriptions: "
              "%zu. Limit: %zu",
              mClientSet.size(), kMaxSubscriptions);
        return false;
    }

    return startNewSubscriptionLocked(ShellSubscriberClient::create(
            in, out, timeoutSec, getElapsedRealtimeSec(), mUidMap, mPullerMgr));
}

bool ShellSubscriber::startNewSubscription(const vector<uint8_t>& subscriptionConfig,
                                           const shared_ptr<IStatsSubscriptionCallback>& callback) {
    std::lock_guard lock(mMutex);
    VLOG("ShellSubscriber: new subscription has come in");
    if (mClientSet.size() >= kMaxSubscriptions) {
        ALOGE("ShellSubscriber: cannot have another active subscription. Current Subscriptions: "
              "%zu. Limit: %zu",
              mClientSet.size(), kMaxSubscriptions);
        return false;
    }

    return startNewSubscriptionLocked(ShellSubscriberClient::create(
            subscriptionConfig, callback, getElapsedRealtimeSec(), mUidMap, mPullerMgr));
}

bool ShellSubscriber::startNewSubscriptionLocked(unique_ptr<ShellSubscriberClient> client) {
    if (client == nullptr) return false;

    // Add new valid client to the client set
    mClientSet.insert(std::move(client));
    updateAtomIdsInUseLocked();

    // Only spawn one thread to manage pulling atoms and sending
    // heartbeats.
    if (!mThreadAlive) {
        mThreadAlive = true;
        if (mThread.joinable()) {
            mThread.join();
        }
        mThread = std::thread([this] { pullAndSendHeartbeats(); });
    } else {
        // If pullAndSendHeartbeats() thread is sleeping, force a wake-up to trigger the initial
        // pull for the newly added subscription.
        mShouldWakeupThread = true;
        mThreadSleepCV.notify_one();
    }

    return true;
}

// Sends heartbeat signals and sleeps between doing work
void ShellSubscriber::pullAndSendHeartbeats() {
    VLOG("ShellSubscriber: helper thread starting");
    std::unique_lock<std::mutex> lock(mMutex);
    while (true) {
        StatsdStats::getInstance().noteSubscriptionPullThreadWakeup();
        mShouldWakeupThread = false;
        int64_t sleepTimeMs = 24 * 60 * 60 * 1000;  // 24 hours.
        const int64_t nowNanos = getElapsedRealtimeNs();
        const int64_t nowMillis = nanoseconds_to_milliseconds(nowNanos);
        const int64_t nowSecs = nanoseconds_to_seconds(nowNanos);
        for (auto clientIt = mClientSet.begin(); clientIt != mClientSet.end();) {
            int64_t subscriptionSleepMs =
                    (*clientIt)->pullAndSendHeartbeatsIfNeeded(nowSecs, nowMillis, nowNanos);
            sleepTimeMs = std::min(sleepTimeMs, subscriptionSleepMs);
            if ((*clientIt)->isAlive()) {
                ++clientIt;
            } else {
                VLOG("ShellSubscriber: removing client!");
                (*clientIt)->onUnsubscribe();
                clientIt = mClientSet.erase(clientIt);
                updateAtomIdsInUseLocked();
            }
        }
        if (mClientSet.empty()) {
            mThreadAlive = false;
            VLOG("ShellSubscriber: helper thread done!");
            return;
        }
        VLOG("ShellSubscriber: helper thread sleeping for %" PRId64 "ms", sleepTimeMs);
        mThreadSleepCV.wait_for(lock, sleepTimeMs * 1ms,
                                [this] { return mClientSet.empty() || mShouldWakeupThread; });
    }
}

void ShellSubscriber::onLogEvent(const LogEvent& event) {
    ATRACE_CALL();
    // Skip if event is skipped
    if (event.isParsedHeaderOnly()) {
        return;
    }
    // Skip RestrictedLogEvents
    if (event.isRestricted()) {
        return;
    }
    std::lock_guard lock(mMutex);
    for (auto clientIt = mClientSet.begin(); clientIt != mClientSet.end();) {
        (*clientIt)->onLogEvent(event);
        if ((*clientIt)->isAlive()) {
            ++clientIt;
        } else {
            VLOG("ShellSubscriber: removing client!");

            (*clientIt)->onUnsubscribe();
            clientIt = mClientSet.erase(clientIt);
            updateAtomIdsInUseLocked();
        }
    }
}

void ShellSubscriber::flushSubscription(const shared_ptr<IStatsSubscriptionCallback>& callback) {
    std::lock_guard lock(mMutex);

    // TODO(b/268822860): Consider storing callback clients in a map keyed by
    // IStatsSubscriptionCallback to avoid this linear search.
    for (auto clientIt = mClientSet.begin(); clientIt != mClientSet.end(); ++clientIt) {
        if ((*clientIt)->hasCallback(callback)) {
            if ((*clientIt)->isAlive()) {
                (*clientIt)->flush();
            } else {
                VLOG("ShellSubscriber: removing client!");

                (*clientIt)->onUnsubscribe();

                // Erasing a value moves the iterator to the next value. The update expression also
                // moves the iterator, skipping a value. This is fine because we do an early return
                // before next iteration of the loop.
                clientIt = mClientSet.erase(clientIt);
                updateAtomIdsInUseLocked();
            }
            return;
        }
    }
}

void ShellSubscriber::unsubscribe(const shared_ptr<IStatsSubscriptionCallback>& callback) {
    std::lock_guard lock(mMutex);

    // TODO(b/268822860): Consider storing callback clients in a map keyed by
    // IStatsSubscriptionCallback to avoid this linear search.
    for (auto clientIt = mClientSet.begin(); clientIt != mClientSet.end(); ++clientIt) {
        if ((*clientIt)->hasCallback(callback)) {
            VLOG("ShellSubscriber: removing client!");

            (*clientIt)->onUnsubscribe();

            // Erasing a value moves the iterator to the next value. The update expression also
            // moves the iterator, skipping a value. This is fine because we do an early return
            // before next iteration of the loop.
            clientIt = mClientSet.erase(clientIt);
            updateAtomIdsInUseLocked();
            return;
        }
    }
}

void ShellSubscriber::updateAtomIdsInUseLocked() const {
    AtomsInUseChangeListener::AtomIdSet allAtomIds;
    for (const auto& client : mClientSet) {
        client->addAllAtomIds(allAtomIds);
    }
    VLOG("ShellSubscriber: Updating allAtomIds done. Total atoms %d", (int)allAtomIds.size());
    mAtomsInUseChangeListener->setAtomIds(std::move(allAtomIds), this);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
