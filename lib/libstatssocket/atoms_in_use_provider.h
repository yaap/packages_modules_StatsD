
/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "utils.h"

#define LOGGING_CONTROL_API_VERSION 36

template <typename Clock>
class AtomsInUseProvider {
public:
    AtomsInUseProvider(std::string fileName, std::string versionPropertyName,
                       int64_t cacheTtlNanos);

    // TODO: consider marking const to emphasize logic constness for thread-safety
    bool isAtomInUse(int32_t atomId);

private:
    bool isAtomInUseLocked(int32_t atomId, int64_t nowNs);

    void updateCacheIfNeededLocked(int64_t nowNs);

    bool isAtomListAccessAllowed() const;

    bool isSyncNeededLocked(int64_t& newVersion);

    bool syncAtomsList();

    const std::string mFileName;
    const std::string mVersionPropertyName;

    CooldownTimer mCacheCooldownTimer;

    std::mutex mMutex;

    int64_t mListVersion = 0;

    std::unordered_set<int32_t> mAtomsInUseCached;
};
