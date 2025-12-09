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
#include "socket_test_helpers.h"

#include <StatsdLoggingControl.h>
#include <android-base/properties.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "storage/StorageManager.h"

namespace android {
namespace os {
namespace statsd {

void cleanupSocketTestFiles(const std::string& fileName, const std::string& versionProperty) {
    std::error_code ec;
    std::filesystem::remove(fileName, ec);
    android::base::SetProperty(versionProperty, "");
}

void verifyAtomsInUseFileContent(const std::string& fileName, const std::vector<int32_t>& atoms) {
    EXPECT_NE(android::base::GetProperty(kTestVersionProperty, ""), "");

    std::string buffer;
    ASSERT_TRUE(StorageManager::readFileToString(fileName.c_str(), &buffer));

    ASSERT_EQ(buffer.size(),
              sizeof(FileHeader) + sizeof(BlockHeader) + sizeof(int32_t) * atoms.size());

    const char* ptr = buffer.data();
    const FileHeader* fileHeader = reinterpret_cast<const FileHeader*>(ptr);
    EXPECT_EQ(fileHeader->magic_number, kMagicNumber);
    EXPECT_EQ(fileHeader->version, kFormatVersion1);
    ptr += sizeof(FileHeader);
    const BlockHeader* blockHeader = reinterpret_cast<const BlockHeader*>(ptr);
    const int32_t atomIdsCount = blockHeader->atomIdsCount;
    ptr += sizeof(BlockHeader);
    const int32_t* atomIdsArray = reinterpret_cast<const int32_t*>(ptr);
    std::vector<int32_t> atomIds(atomIdsArray, atomIdsArray + atomIdsCount);
    std::sort(atomIds.begin(), atomIds.end());
    std::vector<int32_t> expectedAtoms = atoms;
    std::sort(expectedAtoms.begin(), expectedAtoms.end());
    EXPECT_EQ(atomIds, expectedAtoms);
}

void verifyAtomsInUseListAbsence(const std::string& fileName, const std::string& versionProperty) {
    EXPECT_FALSE(std::filesystem::exists(fileName));
    EXPECT_EQ(android::base::GetProperty(versionProperty, ""), "");
}

}  // namespace statsd
}  // namespace os
}  // namespace android