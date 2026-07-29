/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "packages/LogSourceHandler.h"

#include <private/android_filesystem_config.h>

#include <algorithm>

namespace android {
namespace os {
namespace statsd {

LogSourceHandler::LogSourceHandler(const std::vector<std::string>& allowedLogSources,
                                   const std::set<int32_t>& allowlistedAtomIds,
                                   const sp<UidMap>& uidMap)
    : LogSourceHandler(allowedLogSources, allowlistedAtomIds, uidMap,
                       splitSources(allowedLogSources)) {
}

LogSourceHandler::LogSourceHandler(const std::vector<std::string>& allowedLogSources,
                                   const std::set<int32_t>& allowlistedAtomIds,
                                   const sp<UidMap>& uidMap, LogSources&& sources)
    : mAllowlistedAtomIds(allowlistedAtomIds),
      mAllowedAids(std::move(sources.aids)),
      mAllowedPkgs(std::move(sources.pkgs)),
      mUidMap(uidMap) {
    updateResolvedUidsLocked();
}

LogSourceHandler::LogSources LogSourceHandler::splitSources(
        const std::vector<std::string>& sources) {
    LogSources result;
    for (const auto& source : sources) {
        auto it = UidMap::sAidToUidMapping.find(source);
        if (it != UidMap::sAidToUidMapping.end()) {
            result.aids.push_back(it->second);
        } else {
            result.pkgs.push_back(source);
        }
    }
    return result;
}

bool LogSourceHandler::checkLogCredentials(int32_t uid, int32_t atomId) const {
    if (uid == AID_ROOT || (uid >= AID_SYSTEM && uid < AID_SHELL)) {
        // enable atoms logged from pre-installed Android system services
        return true;
    }

    if (mAllowlistedAtomIds.find(atomId) != mAllowlistedAtomIds.end()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (mResolvedUids.find(uid) != mResolvedUids.end()) {
        return true;
    }

    return false;
}

void LogSourceHandler::onUidMapUpdated() {
    std::lock_guard<std::mutex> lock(mMutex);
    updateResolvedUidsLocked();
}

void LogSourceHandler::updateResolvedUidsLocked() {
    mResolvedUids.clear();
    mResolvedUids.insert(mAllowedAids.begin(), mAllowedAids.end());

    for (const auto& pkg : mAllowedPkgs) {
        auto uids = mUidMap->getAppUid(pkg);
        mResolvedUids.insert(uids.begin(), uids.end());
    }

    if (STATSD_DEBUG) {
        for (const int32_t uid : mResolvedUids) {
            VLOG("Allowed uid %d", uid);
        }
    }
}

void LogSourceHandler::onAppChanged(const std::string& packageName) {
    if (std::find(mAllowedPkgs.begin(), mAllowedPkgs.end(), packageName) != mAllowedPkgs.end()) {
        // We will re-initialize the whole list because we don't want to keep the multi mapping of
        // UID<->pkg to reduce the memory usage.
        std::lock_guard<std::mutex> lock(mMutex);
        updateResolvedUidsLocked();
    }
}

void LogSourceHandler::dumpStates(int out) const {
    std::lock_guard<std::mutex> lock(mMutex);
    for (const auto& source : mResolvedUids) {
        dprintf(out, "%d ", source);
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android
