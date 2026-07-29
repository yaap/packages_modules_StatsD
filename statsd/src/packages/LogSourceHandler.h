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

#pragma once

#include <gtest/gtest_prod.h>
#include <utils/RefBase.h>

#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "packages/UidMap.h"

namespace android {
namespace os {
namespace statsd {

class LogSourceHandler : public virtual RefBase {
public:
    LogSourceHandler(const std::vector<std::string>& allowedLogSources,
                     const std::set<int32_t>& allowlistedAtomIds, const sp<UidMap>& uidMap);

    virtual ~LogSourceHandler() = default;

    /**
     * Checks if the uid is allowed to log the atom.
     */
    bool checkLogCredentials(int32_t uid, int32_t atomId) const;

    /**
     * Callback for when UidMap is updated.
     */
    void onUidMapUpdated();

    /**
     * Callback for package changes inside UidMap.
     */
    void onAppChanged(const std::string& packageName);

    void dumpStates(int out) const;

private:
    /**
     * Resolves package names to UIDs using UidMap and updates mResolvedUids.
     */
    void updateResolvedUidsLocked();

    struct LogSources {
        std::vector<int32_t> aids;
        std::vector<std::string> pkgs;
    };

    static LogSources splitSources(const std::vector<std::string>& sources);

    /**
     * Private constructor used for delegation to initialize mAllowedAids and mAllowedPkgs (which
     * are const) from a single pass over allowedLogSources.
     */
    LogSourceHandler(const std::vector<std::string>& allowedLogSources,
                     const std::set<int32_t>& allowlistedAtomIds, const sp<UidMap>& uidMap,
                     LogSources&& sources);

    const std::set<int32_t> mAllowlistedAtomIds;

    // Derived from allowedLogSources in the constructor
    const std::vector<int32_t> mAllowedAids;
    const std::vector<std::string> mAllowedPkgs;

    const sp<UidMap> mUidMap;

    // Combined UIDs from mAllowedUids and resolved mAllowedPkgs.
    std::set<int32_t> mResolvedUids;

    mutable std::mutex mMutex;

    FRIEND_TEST(LogSourceHandlerTest, TestEmptyLogSource);
    FRIEND_TEST(LogSourceHandlerTest, TestAids);
    FRIEND_TEST(LogSourceHandlerTest, TestPackages);
    FRIEND_TEST(LogSourceHandlerTest, TestAllowlistedAtoms);
};

}  // namespace statsd
}  // namespace os
}  // namespace android
