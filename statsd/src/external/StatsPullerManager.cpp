/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define STATSD_DEBUG false
#include "Log.h"

#include "StatsPullerManager.h"

#include <com_android_os_statsd_flags.h>
#include <cutils/log.h>
#include <math.h>
#include <stdint.h>

#include <algorithm>
#include <atomic>
#include <iostream>

#include "../StatsService.h"
#include "../logd/LogEvent.h"
#include "../stats_log_util.h"
#include "../statscompanion_util.h"
#include "StatsCallbackPuller.h"
#include "TrainInfoPuller.h"
#include "statslog_statsd.h"

using std::shared_ptr;
using std::vector;

namespace flags = com::android::os::statsd::flags;

namespace android {
namespace os {
namespace statsd {

// Values smaller than this may require to update the alarm.
const int64_t NO_ALARM_UPDATE = INT64_MAX;
// Use 3 threads to avoid overwhelming system server binder threads
const int32_t PULLER_THREAD_COUNT = 3;

static PullErrorCode pullImpl(const PullerKey& key, const sp<StatsPuller>& puller,
                              const int64_t eventTimeNs, vector<shared_ptr<LogEvent>>* data) {
    VLOG("Initiating pulling %d", key.atomTag);
    PullErrorCode status = puller->Pull(eventTimeNs, data);
    VLOG("pulled %zu items", data->size());
    if (status != PULL_SUCCESS) {
        StatsdStats::getInstance().notePullFailed(key.atomTag);
    }
    return status;
}

StatsPullerManager::StatsPullerManager()
    : mAllPullAtomInfo({
              // TrainInfo.
              {{.uid = AID_STATSD, .atomTag = util::TRAIN_INFO}, new TrainInfoPuller()},
      }),
      mNextPullTimeNs(NO_ALARM_UPDATE) {
}

bool StatsPullerManager::Pull(int tagId, const ConfigKey& configKey, const int64_t eventTimeNs,
                              vector<shared_ptr<LogEvent>>* data) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    return PullLocked(tagId, configKey, eventTimeNs, data);
}

bool StatsPullerManager::Pull(int tagId, const vector<int32_t>& uids, const int64_t eventTimeNs,
                              vector<std::shared_ptr<LogEvent>>* data) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    return PullLocked(tagId, uids, eventTimeNs, data);
}

bool StatsPullerManager::PullLocked(int tagId, const ConfigKey& configKey,
                                    const int64_t eventTimeNs, vector<shared_ptr<LogEvent>>* data) {
    vector<int32_t> uids;
    if (!getPullerUidsLocked(tagId, configKey, uids)) {
        return false;
    }
    return PullLocked(tagId, uids, eventTimeNs, data);
}

bool StatsPullerManager::PullLocked(int tagId, const vector<int32_t>& uids,
                                    const int64_t eventTimeNs, vector<shared_ptr<LogEvent>>* data) {
    VLOG("Initiating pulling %d", tagId);
    for (int32_t uid : uids) {
        PullerKey key = {.uid = uid, .atomTag = tagId};
        auto pullerIt = mAllPullAtomInfo.find(key);
        if (pullerIt != mAllPullAtomInfo.end()) {
            PullErrorCode status = PULL_SUCCESS;
            if (flags::parallel_pulls()) {
                status = pullImpl(key, pullerIt->second, eventTimeNs, data);
            } else {
                status = pullerIt->second->Pull(eventTimeNs, data);
                VLOG("pulled %zu items", data->size());
                if (status != PULL_SUCCESS) {
                    StatsdStats::getInstance().notePullFailed(tagId);
                }
            }
            // If we received a dead object exception, it means the client process has died.
            // We can remove the puller from the map.
            if (status == PULL_DEAD_OBJECT) {
                StatsdStats::getInstance().notePullerCallbackRegistrationChanged(
                        tagId,
                        /*registered=*/false);
                mAllPullAtomInfo.erase(pullerIt);
            }
            return status == PULL_SUCCESS;
        }
    }
    StatsdStats::getInstance().notePullerNotFound(tagId);
    ALOGW("StatsPullerManager: Unknown tagId %d", tagId);
    return false;  // Return early since we don't know what to pull.
}

bool StatsPullerManager::PullerForMatcherExists(int tagId) const {
    // Pulled atoms might be registered after we parse the config, so just make sure the id is in
    // an appropriate range.
    return isVendorPulledAtom(tagId) || isPulledAtom(tagId);
}

void StatsPullerManager::updateAlarmLocked() {
    if (mNextPullTimeNs == NO_ALARM_UPDATE) {
        VLOG("No need to set alarms. Skipping");
        return;
    }

    // TODO(b/151045771): do not hold a lock while making a binder call
    if (mStatsCompanionService != nullptr) {
        mStatsCompanionService->setPullingAlarm(mNextPullTimeNs / 1000000);
    } else {
        VLOG("StatsCompanionService not available. Alarm not set.");
    }
    return;
}

void StatsPullerManager::SetStatsCompanionService(
        const shared_ptr<IStatsCompanionService>& statsCompanionService) {
    std::lock_guard<std::mutex> _l(mLock);
    shared_ptr<IStatsCompanionService> tmpForLock = mStatsCompanionService;
    mStatsCompanionService = statsCompanionService;
    for (const auto& pulledAtom : mAllPullAtomInfo) {
        pulledAtom.second->SetStatsCompanionService(statsCompanionService);
    }
    if (mStatsCompanionService != nullptr) {
        updateAlarmLocked();
    }
}

void StatsPullerManager::RegisterReceiver(int tagId, const ConfigKey& configKey,
                                          const wp<PullDataReceiver>& receiver,
                                          int64_t nextPullTimeNs, int64_t intervalNs) {
    std::lock_guard<std::mutex> _l(mLock);
    auto& receivers = mReceivers[{.atomTag = tagId, .configKey = configKey}];
    for (auto it = receivers.begin(); it != receivers.end(); it++) {
        if (it->receiver == receiver) {
            VLOG("Receiver already registered of %d", (int)receivers.size());
            return;
        }
    }
    ReceiverInfo receiverInfo;
    receiverInfo.receiver = receiver;

    // Round it to the nearest minutes. This is the limit of alarm manager.
    // In practice, we should always have larger buckets.
    int64_t roundedIntervalNs = intervalNs / NS_PER_SEC / 60 * NS_PER_SEC * 60;
    // Scheduled pulling should be at least 1 min apart.
    // This can be lower in cts tests, in which case we round it to 1 min.
    if (roundedIntervalNs < 60 * (int64_t)NS_PER_SEC) {
        roundedIntervalNs = 60 * (int64_t)NS_PER_SEC;
    }

    receiverInfo.intervalNs = roundedIntervalNs;
    receiverInfo.nextPullTimeNs = nextPullTimeNs;
    receivers.push_back(receiverInfo);

    // There is only one alarm for all pulled events. So only set it to the smallest denom.
    if (nextPullTimeNs < mNextPullTimeNs) {
        VLOG("Updating next pull time %lld", (long long)mNextPullTimeNs);
        mNextPullTimeNs = nextPullTimeNs;
        updateAlarmLocked();
    }
    VLOG("Puller for tagId %d registered of %d", tagId, (int)receivers.size());
}

void StatsPullerManager::UnRegisterReceiver(int tagId, const ConfigKey& configKey,
                                            const wp<PullDataReceiver>& receiver) {
    std::lock_guard<std::mutex> _l(mLock);
    auto receiversIt = mReceivers.find({.atomTag = tagId, .configKey = configKey});
    if (receiversIt == mReceivers.end()) {
        VLOG("Unknown pull code or no receivers: %d", tagId);
        return;
    }
    std::list<ReceiverInfo>& receivers = receiversIt->second;
    for (auto it = receivers.begin(); it != receivers.end(); it++) {
        if (receiver == it->receiver) {
            receivers.erase(it);
            VLOG("Puller for tagId %d unregistered of %d", tagId, (int)receivers.size());
            return;
        }
    }
}

void StatsPullerManager::RegisterPullUidProvider(const ConfigKey& configKey,
                                                 const wp<PullUidProvider>& provider) {
    std::lock_guard<std::mutex> _l(mLock);
    mPullUidProviders[configKey] = provider;
}

void StatsPullerManager::UnregisterPullUidProvider(const ConfigKey& configKey,
                                                   const wp<PullUidProvider>& provider) {
    std::lock_guard<std::mutex> _l(mLock);
    const auto& it = mPullUidProviders.find(configKey);
    if (it != mPullUidProviders.end() && it->second == provider) {
        mPullUidProviders.erase(it);
    }
}

static void processPullerQueue(ThreadSafeQueue<StatsPullerManager::PullerParams>& pullerQueue,
                               std::queue<StatsPullerManager::PulledInfo>& pulledData,
                               const int64_t wallClockNs, const int64_t elapsedTimeNs,
                               std::atomic_int& pendingThreads,
                               std::condition_variable& mainThreadCondition,
                               std::mutex& mainThreadConditionLock) {
    std::optional<StatsPullerManager::PullerParams> queueResult = pullerQueue.pop();
    while (queueResult.has_value()) {
        const StatsPullerManager::PullerParams pullerParams = queueResult.value();
        vector<shared_ptr<LogEvent>> data;
        PullErrorCode pullErrorCode =
                pullImpl(pullerParams.key, pullerParams.puller, elapsedTimeNs, &data);

        if (pullErrorCode != PULL_SUCCESS) {
            VLOG("pull failed at %lld, will try again later", (long long)elapsedTimeNs);
        }

        // Convention is to mark pull atom timestamp at request time.
        // If we pull at t0, puller starts at t1, finishes at t2, and send back
        // at t3, we mark t0 as its timestamp, which should correspond to its
        // triggering event, such as condition change at t0.
        // Here the triggering event is alarm fired from AlarmManager.
        // In ValueMetricProducer and GaugeMetricProducer we do same thing
        // when pull on condition change, etc.
        for (auto& event : data) {
            event->setElapsedTimestampNs(elapsedTimeNs);
            event->setLogdWallClockTimestampNs(wallClockNs);
        }

        StatsPullerManager::PulledInfo pulledInfo;
        pulledInfo.pullErrorCode = pullErrorCode;
        pulledInfo.pullerKey = pullerParams.key;
        pulledInfo.receiverInfo = std::move(pullerParams.receivers);
        pulledInfo.data = std::move(data);
        mainThreadConditionLock.lock();
        pulledData.push(pulledInfo);
        mainThreadConditionLock.unlock();
        mainThreadCondition.notify_one();

        queueResult = pullerQueue.pop();
    }
    pendingThreads--;
    mainThreadCondition.notify_one();
}

void StatsPullerManager::OnAlarmFired(int64_t elapsedTimeNs) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    int64_t wallClockNs = getWallClockNs();

    int64_t minNextPullTimeNs = NO_ALARM_UPDATE;
    if (flags::parallel_pulls()) {
        ThreadSafeQueue<PullerParams> pullerQueue;
        std::queue<PulledInfo> pulledData;
        initPullerQueue(pullerQueue, pulledData, elapsedTimeNs, minNextPullTimeNs);
        std::mutex mainThreadConditionLock;
        std::condition_variable waitForPullerThreadsCondition;
        vector<thread> pullerThreads;
        std::atomic_int pendingThreads = PULLER_THREAD_COUNT;
        pullerThreads.reserve(PULLER_THREAD_COUNT);
        // Spawn multiple threads to simultaneously pull all necessary pullers. These pullers push
        // the pulled data to a queue for the main thread to process.
        for (int i = 0; i < PULLER_THREAD_COUNT; ++i) {
            pullerThreads.emplace_back(
                    processPullerQueue, std::ref(pullerQueue), std::ref(pulledData), wallClockNs,
                    elapsedTimeNs, std::ref(pendingThreads),
                    std::ref(waitForPullerThreadsCondition), std::ref(mainThreadConditionLock));
        }

        // Process all pull results on the main thread without waiting for the puller threads
        // to finish.
        while (true) {
            std::unique_lock<std::mutex> lock(mainThreadConditionLock);
            waitForPullerThreadsCondition.wait(lock, [&pulledData, &pendingThreads]() -> bool {
                return pendingThreads == 0 || !pulledData.empty();
            });
            if (!pulledData.empty()) {
                const PulledInfo pullResultInfo = std::move(pulledData.front());
                pulledData.pop();
                const PullErrorCode pullErrorCode = pullResultInfo.pullErrorCode;
                const vector<ReceiverInfo*>& receiverInfos = pullResultInfo.receiverInfo;
                const vector<shared_ptr<LogEvent>>& data = pullResultInfo.data;
                for (const auto& receiverInfo : receiverInfos) {
                    sp<PullDataReceiver> receiverPtr = receiverInfo->receiver.promote();
                    if (receiverPtr != nullptr) {
                        PullResult pullResult = pullErrorCode == PULL_SUCCESS
                                                        ? PullResult::PULL_RESULT_SUCCESS
                                                        : PullResult::PULL_RESULT_FAIL;
                        receiverPtr->onDataPulled(data, pullResult, elapsedTimeNs);
                        // We may have just come out of a coma, compute next pull time.
                        int numBucketsAhead = (elapsedTimeNs - receiverInfo->nextPullTimeNs) /
                                              receiverInfo->intervalNs;
                        receiverInfo->nextPullTimeNs +=
                                (numBucketsAhead + 1) * receiverInfo->intervalNs;
                        minNextPullTimeNs = min(receiverInfo->nextPullTimeNs, minNextPullTimeNs);
                    } else {
                        VLOG("receiver already gone.");
                    }
                }
                if (pullErrorCode == PULL_DEAD_OBJECT) {
                    mAllPullAtomInfo.erase(pullResultInfo.pullerKey);
                }
                // else if is used here for the edge case of all threads being completed but
                // there are remaining pulled results in the queue to process.
            } else if (pendingThreads == 0) {
                break;
            }
        }

        for (thread& pullerThread : pullerThreads) {
            pullerThread.join();
        }

    } else {
        onAlarmFiredSynchronous(elapsedTimeNs, wallClockNs, minNextPullTimeNs);
    }
    VLOG("mNextPullTimeNs: %lld updated to %lld", (long long)mNextPullTimeNs,
         (long long)minNextPullTimeNs);
    mNextPullTimeNs = minNextPullTimeNs;
    updateAlarmLocked();
}

void StatsPullerManager::onAlarmFiredSynchronous(const int64_t elapsedTimeNs,
                                                 const int64_t wallClockNs,
                                                 int64_t& minNextPullTimeNs) {
    vector<pair<const ReceiverKey*, vector<ReceiverInfo*>>> needToPull;
    for (auto& pair : mReceivers) {
        vector<ReceiverInfo*> receivers;
        if (pair.second.size() != 0) {
            for (ReceiverInfo& receiverInfo : pair.second) {
                // If pullNecessary and enough time has passed for the next bucket, then add
                // receiver to the list that will pull on this alarm.
                // If pullNecessary is false, check if next pull time needs to be updated.
                sp<PullDataReceiver> receiverPtr = receiverInfo.receiver.promote();
                if (receiverInfo.nextPullTimeNs <= elapsedTimeNs && receiverPtr != nullptr &&
                    receiverPtr->isPullNeeded()) {
                    receivers.push_back(&receiverInfo);
                } else {
                    if (receiverInfo.nextPullTimeNs <= elapsedTimeNs) {
                        receiverPtr->onDataPulled({}, PullResult::PULL_NOT_NEEDED, elapsedTimeNs);
                        int numBucketsAhead = (elapsedTimeNs - receiverInfo.nextPullTimeNs) /
                                              receiverInfo.intervalNs;
                        receiverInfo.nextPullTimeNs +=
                                (numBucketsAhead + 1) * receiverInfo.intervalNs;
                    }
                    minNextPullTimeNs = min(receiverInfo.nextPullTimeNs, minNextPullTimeNs);
                }
            }
            if (receivers.size() > 0) {
                needToPull.push_back(make_pair(&pair.first, receivers));
            }
        }
    }
    for (const auto& pullInfo : needToPull) {
        vector<shared_ptr<LogEvent>> data;
        PullResult pullResult =
                PullLocked(pullInfo.first->atomTag, pullInfo.first->configKey, elapsedTimeNs, &data)
                        ? PullResult::PULL_RESULT_SUCCESS
                        : PullResult::PULL_RESULT_FAIL;
        if (pullResult == PullResult::PULL_RESULT_FAIL) {
            VLOG("pull failed at %lld, will try again later", (long long)elapsedTimeNs);
        }

        // Convention is to mark pull atom timestamp at request time.
        // If we pull at t0, puller starts at t1, finishes at t2, and send back
        // at t3, we mark t0 as its timestamp, which should correspond to its
        // triggering event, such as condition change at t0.
        // Here the triggering event is alarm fired from AlarmManager.
        // In ValueMetricProducer and GaugeMetricProducer we do same thing
        // when pull on condition change, etc.
        for (auto& event : data) {
            event->setElapsedTimestampNs(elapsedTimeNs);
            event->setLogdWallClockTimestampNs(wallClockNs);
        }

        for (const auto& receiverInfo : pullInfo.second) {
            sp<PullDataReceiver> receiverPtr = receiverInfo->receiver.promote();
            if (receiverPtr != nullptr) {
                receiverPtr->onDataPulled(data, pullResult, elapsedTimeNs);
                // We may have just come out of a coma, compute next pull time.
                int numBucketsAhead =
                        (elapsedTimeNs - receiverInfo->nextPullTimeNs) / receiverInfo->intervalNs;
                receiverInfo->nextPullTimeNs += (numBucketsAhead + 1) * receiverInfo->intervalNs;
                minNextPullTimeNs = min(receiverInfo->nextPullTimeNs, minNextPullTimeNs);
            } else {
                VLOG("receiver already gone.");
            }
        }
    }
}

bool StatsPullerManager::getPullerUidsLocked(const int tagId, const ConfigKey& configKey,
                                             vector<int32_t>& uids) {
    const auto& uidProviderIt = mPullUidProviders.find(configKey);
    if (uidProviderIt == mPullUidProviders.end()) {
        ALOGE("Error pulling tag %d. No pull uid provider for config key %s", tagId,
              configKey.ToString().c_str());
        StatsdStats::getInstance().notePullUidProviderNotFound(tagId);
        return false;
    }
    sp<PullUidProvider> pullUidProvider = uidProviderIt->second.promote();
    if (pullUidProvider == nullptr) {
        ALOGE("Error pulling tag %d, pull uid provider for config %s is gone.", tagId,
              configKey.ToString().c_str());
        StatsdStats::getInstance().notePullUidProviderNotFound(tagId);
        return false;
    }
    uids = pullUidProvider->getPullAtomUids(tagId);
    return true;
}

void StatsPullerManager::initPullerQueue(ThreadSafeQueue<PullerParams>& pullerQueue,
                                         std::queue<PulledInfo>& pulledData,
                                         const int64_t elapsedTimeNs, int64_t& minNextPullTimeNs) {
    for (auto& pair : mReceivers) {
        vector<ReceiverInfo*> receivers;
        if (pair.second.size() != 0) {
            for (ReceiverInfo& receiverInfo : pair.second) {
                // If pullNecessary and enough time has passed for the next bucket, then add
                // receiver to the list that will pull on this alarm.
                // If pullNecessary is false, check if next pull time needs to be updated.
                sp<PullDataReceiver> receiverPtr = receiverInfo.receiver.promote();
                if (receiverInfo.nextPullTimeNs <= elapsedTimeNs && receiverPtr != nullptr &&
                    receiverPtr->isPullNeeded()) {
                    receivers.push_back(&receiverInfo);
                } else {
                    if (receiverInfo.nextPullTimeNs <= elapsedTimeNs) {
                        receiverPtr->onDataPulled({}, PullResult::PULL_NOT_NEEDED, elapsedTimeNs);
                        int numBucketsAhead = (elapsedTimeNs - receiverInfo.nextPullTimeNs) /
                                              receiverInfo.intervalNs;
                        receiverInfo.nextPullTimeNs +=
                                (numBucketsAhead + 1) * receiverInfo.intervalNs;
                    }
                    minNextPullTimeNs = min(receiverInfo.nextPullTimeNs, minNextPullTimeNs);
                }
            }
            if (receivers.size() > 0) {
                bool foundPuller = false;
                int tagId = pair.first.atomTag;
                vector<int32_t> uids;
                if (getPullerUidsLocked(tagId, pair.first.configKey, uids)) {
                    for (int32_t uid : uids) {
                        PullerKey key = {.uid = uid, .atomTag = tagId};
                        auto pullerIt = mAllPullAtomInfo.find(key);
                        if (pullerIt != mAllPullAtomInfo.end()) {
                            PullerParams params;
                            params.key = key;
                            params.puller = pullerIt->second;
                            params.receivers = std::move(receivers);
                            pullerQueue.push(params);
                            foundPuller = true;
                            break;
                        }
                    }
                    if (!foundPuller) {
                        StatsdStats::getInstance().notePullerNotFound(tagId);
                        ALOGW("StatsPullerManager: Unknown tagId %d", tagId);
                    }
                }
                if (!foundPuller) {
                    PulledInfo pulledInfo;
                    pulledInfo.pullErrorCode = PullErrorCode::PULL_FAIL;
                    pulledInfo.receiverInfo = std::move(receivers);
                    pulledData.push(pulledInfo);
                }
            }
        }
    }
}

int StatsPullerManager::ForceClearPullerCache() {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    int totalCleared = 0;
    for (const auto& pulledAtom : mAllPullAtomInfo) {
        totalCleared += pulledAtom.second->ForceClearCache();
    }
    return totalCleared;
}

int StatsPullerManager::ClearPullerCacheIfNecessary(int64_t timestampNs) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    int totalCleared = 0;
    for (const auto& pulledAtom : mAllPullAtomInfo) {
        totalCleared += pulledAtom.second->ClearCacheIfNecessary(timestampNs);
    }
    return totalCleared;
}

void StatsPullerManager::RegisterPullAtomCallback(const int uid, const int32_t atomTag,
                                                  const int64_t coolDownNs, const int64_t timeoutNs,
                                                  const vector<int32_t>& additiveFields,
                                                  const shared_ptr<IPullAtomCallback>& callback) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    VLOG("RegisterPullerCallback: adding puller for tag %d", atomTag);

    if (callback == nullptr) {
        ALOGW("SetPullAtomCallback called with null callback for atom %d.", atomTag);
        return;
    }

    int64_t actualCoolDownNs = coolDownNs < kMinCoolDownNs ? kMinCoolDownNs : coolDownNs;
    int64_t actualTimeoutNs = timeoutNs > kMaxTimeoutNs ? kMaxTimeoutNs : timeoutNs;

    sp<StatsCallbackPuller> puller = new StatsCallbackPuller(atomTag, callback, actualCoolDownNs,
                                                             actualTimeoutNs, additiveFields);
    PullerKey key = {.uid = uid, .atomTag = atomTag};
    auto it = mAllPullAtomInfo.find(key);
    if (it != mAllPullAtomInfo.end()) {
        StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag,
                                                                         /*registered=*/false);
    }
    mAllPullAtomInfo[key] = puller;
    StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag, /*registered=*/true);
}

void StatsPullerManager::UnregisterPullAtomCallback(const int uid, const int32_t atomTag) {
    ATRACE_CALL();
    std::lock_guard<std::mutex> _l(mLock);
    PullerKey key = {.uid = uid, .atomTag = atomTag};
    if (mAllPullAtomInfo.find(key) != mAllPullAtomInfo.end()) {
        StatsdStats::getInstance().notePullerCallbackRegistrationChanged(atomTag,
                                                                         /*registered=*/false);
        mAllPullAtomInfo.erase(key);
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android
