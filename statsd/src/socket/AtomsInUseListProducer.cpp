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

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "socket/AtomsInUseListProducer.h"

#include <StatsdLoggingControl.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>

#include <filesystem>
#include <fstream>

#include "guardrail/StatsdStats.h"
#include "storage/StorageManager.h"
#include "utils/api_tracing.h"

namespace android {
namespace os {
namespace statsd {

using android::base::StringPrintf;
using std::string;

AtomsInUseListProducer::AtomsInUseListProducer(string fileName, string versionPropertyName)
    : mFileName(fileName), mVersionPropertyName(versionPropertyName) {
}

AtomsInUseListProducer::~AtomsInUseListProducer() {
    reset();
}

bool AtomsInUseListProducer::setAtomsIds(const std::vector<int32_t>& atomIds) {
    /**
     * the flow sequence for enable/update logging control:
     * - create/update file
     * - set/update property
     *
     * to disable logging control:
     * - remove property
     * - remove file
     */
    ATRACE_CALL();
    TIME_CALL_DEBUG();

    ALOGW("setAtoms for %d ids", (int)atomIds.size());

    if (atomIds.empty()) {
        reset();
        return true;
    }

    // TODO: consider update atom list back to back during short period of time.
    // Alternative is to have a worker thread with a queue + throttling to prevent
    // too frequent I/O & file re-writes
    return createAtomIdsFile(atomIds) && increaseVersionProperty();
}

// removes the file & removes system property version, this will allow clients
// to log any atom, effectively disables logging control
void AtomsInUseListProducer::reset() const {
    /**
     * the reader expectation is: if version property valid, there must be file ready to be
     * consumed, otherwise no expectations about file existence or its content. so property needs to
     * be removed first to not violate the reader expectations in regards to resetting (read
     * disabling) logging control
     */
    removeVersionProperty();
    StorageManager::deleteFile(mFileName.c_str());
}

bool AtomsInUseListProducer::createAtomIdsFile(const std::vector<int32_t>& atomIds) {
    // to eliminate excessive memory usage and large file creation due to mallformed
    // configuration limit the atoms in use list size to 4k ids
    const int32_t atomListSize = static_cast<int32_t>(atomIds.size());
    if (atomListSize > kMaxAtomIdsInList) {
        ALOGW("createAtomIdsFile: atomIds list size %d exceeds limit %d", atomListSize,
              kMaxAtomIdsInList);
        StatsdStats::getInstance().noteIllegalState(
                COUNTER_TYPE_ERROR_LOGGING_CONTROL_ATOMS_IN_USE_SIZE_EXCEEDED);
        return false;
    }

    // create new file staging file removing past version if any
    const string stagingFilePath =
            StringPrintf("%s.%" PRId64 ".tmp", mFileName.c_str(), mListVersion);
    StorageManager::deleteFile(stagingFilePath.c_str());

    // populate the buffer to be written into the file
    const int32_t bufferSize =
            sizeof(FileHeader) + sizeof(BlockHeader) + sizeof(int32_t) * atomListSize;
    string buffer;
    buffer.resize(bufferSize);

    char* ptr = buffer.data();
    FileHeader* fileHeader = reinterpret_cast<FileHeader*>(ptr);
    fileHeader->magic_number = kMagicNumber;
    fileHeader->version = kFormatVersion1;
    ptr += sizeof(FileHeader);

    BlockHeader* blockHeader = reinterpret_cast<BlockHeader*>(ptr);
    blockHeader->atomIdsCount = atomListSize;
    ptr += sizeof(BlockHeader);

    memcpy(ptr, atomIds.data(), sizeof(int32_t) * atomListSize);

    std::ofstream stagingFile(stagingFilePath.c_str(), std::ios::out | std::ios::binary);
    if (!stagingFile.is_open()) {
        ALOGW("createAtomIdsFile: cannot create staging file %s error %d", stagingFilePath.c_str(),
              errno);
        return false;
    }

    stagingFile.write(reinterpret_cast<const char*>(buffer.data()), bufferSize);
    if (stagingFile.fail()) {
        ALOGW("createAtomIdsFile: cannot write staging file %s error %d", stagingFilePath.c_str(),
              errno);
        return false;
    }

    stagingFile.flush();
    if (stagingFile.fail()) {
        ALOGW("createAtomIdsFile: cannot write staging file %s error %d", stagingFilePath.c_str(),
              errno);
        return false;
    }
    stagingFile.close();

    // rename to predefined file
    std::error_code ec;
    std::filesystem::rename(stagingFilePath, mFileName, ec);
    if (ec.value() != 0) {
        ALOGW("createAtomIdsFile: cannot rename staging to prod file (%s)", ec.message().c_str());
        return false;
    }

    // update file access permissions to be globally read
    std::filesystem::permissions(
            mFileName, std::filesystem::perms::group_read | std::filesystem::perms::others_read,
            std::filesystem::perm_options::add, ec);
    if (ec.value() != 0) {
        ALOGW("createAtomIdsFile: error changing file permissions (%s).", ec.message().c_str());
        return false;
    }
    return true;
}

bool AtomsInUseListProducer::increaseVersionProperty() {
    // bump the list version
    mListVersion = getElapsedRealtimeNs();
    if (!base::SetProperty(mVersionPropertyName, std::to_string(mListVersion))) {
        ALOGW("increaseVersionProperty failed");
        return false;
    }
    return true;
}

bool AtomsInUseListProducer::removeVersionProperty() const {
    return base::SetProperty(mVersionPropertyName, "");
}

}  // namespace statsd
}  // namespace os
}  // namespace android
