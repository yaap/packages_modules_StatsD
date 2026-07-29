
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

#define STATSSOCKET_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "atoms_in_use_provider.h"

#include <StatsdLoggingControl.h>
#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <cerrno>
#include <cinttypes>
#include <filesystem>
#include <string>
#include <vector>

using namespace android::os::statsd;

template <typename Clock>
AtomsInUseProvider<Clock>::AtomsInUseProvider(std::string fileName, std::string versionPropertyName,
                                              int64_t cacheTtlNanos)
    : mFileName(std::move(fileName)),
      mVersionPropertyName(std::move(versionPropertyName)),
      mCacheCooldownTimer(cacheTtlNanos) {
}

template <typename Clock>
bool AtomsInUseProvider<Clock>::isAtomInUse(int32_t atomId) {
    const int64_t nowNs = Clock::getTimeNs();
    // TODO (b/407064406): should be fast but thread safe - without mutex on each access
    // use atomics to see if there was update to enabled list
    // only then take mutex to sync/otherwise return cached value
    // consider that isAtomEnabled can be invoked from different threads
    // also locking should not be held during atom in use cache update - so
    // atoms can be logged by other threads except the one which triggered the
    // cache update
    std::lock_guard<std::mutex> lock(mMutex);
    return isAtomInUseLocked(atomId, nowNs);
}

template <typename Clock>
bool AtomsInUseProvider<Clock>::isAtomInUseLocked(int32_t atomId, int64_t nowNs) {
    updateCacheIfNeededLocked(nowNs);

    // if cache is empty it usually means config was not set or did not read properly
    // by default all atoms are enabled in this case
    if (mAtomsInUseCached.size() == 0) {
        return true;
    }

    const bool atomEnabled = mAtomsInUseCached.find(atomId) != mAtomsInUseCached.end();
    VLOG("AtomsInUseProvider::isAtomEnabled %d == %d from %d", atomId, atomEnabled,
         (int)mAtomsInUseCached.size());
    return atomEnabled;
}

template <typename Clock>
void AtomsInUseProvider<Clock>::updateCacheIfNeededLocked(int64_t nowNs) {
    if (!mCacheCooldownTimer.isExpired(nowNs)) {
        VLOG("updateCacheIfNeededLocked: cooldown timer not expired");
        return;
    }
    // whatever will go wrong below - keep delay before retry
    mCacheCooldownTimer.start(nowNs);

    // check access first & reset cache if it cannot be updated
    if (!isAtomListAccessAllowed()) {
        mListVersion = 0;
        mAtomsInUseCached.clear();
        return;
    }

    int64_t newVersion = 0;
    if (!isSyncNeededLocked(newVersion)) {
        VLOG("updateCacheIfNeededLocked: no sync needed");
        return;
    }

    // if list was removed - need to clear cache
    if (newVersion == 0) {
        VLOG("updateCacheIfNeededLocked: list not defined");
        mListVersion = 0;
        mAtomsInUseCached.clear();
        return;
    }

    // or populate with new version
    if (syncAtomsList()) {
        mListVersion = newVersion;
    } else {
        VLOG("updateCacheIfNeededLocked: sync failed");
        // if something went wrong - by default all atoms are in use
        mAtomsInUseCached.clear();
    }
}

template <typename Clock>
bool AtomsInUseProvider<Clock>::isAtomListAccessAllowed() const {
    static const std::string dirName = android::base::Dirname(mFileName);
    if (access(dirName.c_str(), X_OK) != 0) {
        // no access to the file -- return early.
        // access to the file control by selinux policies per application or per domain
        // the access rules can be changed in runtime and do not require application restart
        // It is not sufficient to perform this check once,
        // however that is a good option to consider.
        VLOG("isAtomListAccessAllowed: not allowed");
        return false;
    }
    return true;
}

template <typename Clock>
bool AtomsInUseProvider<Clock>::isSyncNeededLocked(int64_t& newVersion) {
    // check if there is a new list published
    const std::string value = android::base::GetProperty(mVersionPropertyName, "");
    if (value.empty()) {
        VLOG("isSyncNeededLocked: list was removed or not defined");
        return mListVersion > 0;
    }

    newVersion = atoll(value.c_str());
    VLOG("isSyncNeededLocked: newVersion %" PRId64 " vs mListVersion %" PRId64, newVersion,
         mListVersion);
    // test if new version is available
    return newVersion != mListVersion;
}

template <typename Clock>
bool AtomsInUseProvider<Clock>::syncAtomsList() {
    std::string buffer;
    if (!android::base::ReadFileToString(mFileName.c_str(), &buffer)) {
        VLOG("syncAtomsList: Error reading %s: %s", mFileName.c_str(), std::strerror(errno));
        return false;
    }

    if (buffer.size() < sizeof(FileHeader) + sizeof(BlockHeader) + sizeof(int32_t)) {
        VLOG("syncAtomsList: invalid file size");
        return false;
    }

    const char* ptr = buffer.data();
    const FileHeader* fileHeader = reinterpret_cast<const FileHeader*>(ptr);
    if (fileHeader->magic_number != kMagicNumber) {
        VLOG("syncAtomsList: invalid file header magic number");
        return false;
    }

    if (fileHeader->version != kFormatVersion1) {
        VLOG("syncAtomsList: invalid file header version");
        return false;
    }

    ptr += sizeof(FileHeader);
    const BlockHeader* blockHeader = reinterpret_cast<const BlockHeader*>(ptr);
    const int32_t atomIdsCount = blockHeader->atomIdsCount;

    if (atomIdsCount < 1) {
        VLOG("syncAtomsList: invalid file content");
        return false;
    }

    if (buffer.size() !=
        sizeof(FileHeader) + sizeof(BlockHeader) + sizeof(int32_t) * atomIdsCount) {
        VLOG("syncAtomsList: invalid file size");
        return false;
    }

    ptr += sizeof(BlockHeader);

    const int32_t* atomIdsArray = reinterpret_cast<const int32_t*>(ptr);
    mAtomsInUseCached = {atomIdsArray, atomIdsArray + atomIdsCount};
    return true;
}

template class AtomsInUseProvider<RealTimeClock>;
