/*
 * Copyright (C) 2019, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "StateManager.h"

#include <private/android_filesystem_config.h>
#include <statslog_statsd.h>

#include <set>

namespace android {
namespace os {
namespace statsd {

using std::set;
using std::string;

StateManager::StateManager() {
}

StateManager& StateManager::getInstance() {
    static StateManager sStateManager;
    return sStateManager;
}

void StateManager::clear() {
    mStateTrackers.clear();
}

void StateManager::onLogEvent(const LogEvent& event) {
    const int tagId = event.GetTagId();
    if (tagId == util::STATS_SOCKET_LOSS_REPORTED) {
        // Hard coded logic to handle socket loss info to highlight metric corruption reason
        const std::optional<SocketLossInfo>& lossInfo = toSocketLossInfo(event);
        if (lossInfo) {
            onLogEventLost(*lossInfo);
        }
    } else {
        auto stateTrackersForEvent = mStateTrackers.find(tagId);
        if (stateTrackersForEvent != mStateTrackers.end()) {
            stateTrackersForEvent->second->onLogEvent(event);
        }
    }
}

void StateManager::onLogEventLost(const SocketLossInfo& socketLossInfo) {
    // socketLossInfo stores atomId per UID - to eliminate duplicates using set
    set<int32_t> uniqueLostAtomIds(socketLossInfo.atomIds.begin(), socketLossInfo.atomIds.end());

    // pass lost atom id to all relevant metrics
    for (const auto lostAtomId : uniqueLostAtomIds) {
        notifyStateTrackersAboutLostAtom(lostAtomId, DATA_CORRUPTED_SOCKET_LOSS);
    }
}

bool StateManager::notifyStateTrackersAboutLostAtom(int32_t lostAtomId,
                                                    DataCorruptedReason reason) {
    auto stateTrackersIt = mStateTrackers.find(lostAtomId);
    if (stateTrackersIt != mStateTrackers.end()) {
        stateTrackersIt->second->onLogEventLost(reason);
        return true;
    }
    return false;
}

void StateManager::registerListener(const int32_t atomId, const wp<StateListener>& listener) {
    // Check if state tracker already exists.
    if (mStateTrackers.find(atomId) == mStateTrackers.end()) {
        mStateTrackers[atomId] = new StateTracker(atomId);
    }
    mStateTrackers[atomId]->registerListener(listener);
}

void StateManager::unregisterListener(const int32_t atomId, const wp<StateListener>& listener) {
    std::unique_lock<std::mutex> lock(mMutex);

    // Hold the sp<> until the lock is released so that ~StateTracker() is
    // not called while the lock is held.
    sp<StateTracker> toRemove;

    // Unregister listener from correct StateTracker
    auto it = mStateTrackers.find(atomId);
    if (it != mStateTrackers.end()) {
        it->second->unregisterListener(listener);

        // Remove the StateTracker if it has no listeners
        if (it->second->getListenersCount() == 0) {
            toRemove = it->second;
            mStateTrackers.erase(it);
        }
    } else {
        ALOGE("StateManager cannot unregister listener, StateTracker for atom %d does not exist",
              atomId);
    }
    lock.unlock();
}

FieldValue StateManager::getStateValue(const int32_t atomId,
                                       const HashableDimensionKey& key) const {
    auto it = mStateTrackers.find(atomId);
    if (it != mStateTrackers.end()) {
        return it->second->getStateValue(key);
    }
    ALOGE("StateManager cannot get state value, no StateTracker for atom %d", atomId);
    return FieldValue(Field(atomId, 0), StateTracker::kStateUnknown);
}

void StateManager::addAllAtomIds(AtomsInUseChangeListener::AtomIdSet& allIds) const {
    for (const auto& stateTracker : mStateTrackers) {
        allIds.insert(stateTracker.first);
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android
