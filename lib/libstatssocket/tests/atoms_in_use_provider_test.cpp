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

#include "atoms_in_use_provider.h"

#include <StatsdLoggingControl.h>
#include <android-base/properties.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

// The implementation of the templated AtomsInUseProvider is in the .cpp file.
// Including it here to allow for template instantiation with MockClock.
#include "../atoms_in_use_provider.cpp"

#ifdef __ANDROID__

namespace {

const std::string kTestFileName = "/data/local/tmp/atoms_in_use_provider_test.bin";
const std::string kTestVersionProperty = "debug.statsd.atoms_in_use_provider_test.version";
const int64_t kCacheTtlNanos = 100 * 1000 * 1000;  // 100ms

// Mock clock to control time in tests.
struct MockClock {
    static int64_t sTime;
    static int64_t getTimeNs() {
        return sTime;
    }
    static void advance(int64_t deltaNs) {
        sTime += deltaNs;
    }
};
int64_t MockClock::sTime = 0;

void cleanup() {
    std::error_code ec;
    std::filesystem::remove(kTestFileName, ec);
    android::base::SetProperty(kTestVersionProperty, "");
}

}  // anonymous namespace

using namespace android::os::statsd;
using android::base::StringPrintf;

class AtomsInUseProviderTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanup();
        MockClock::sTime = 0;
    }

    void TearDown() override {
        cleanup();
    }

    bool createAtomsFile(const std::vector<int32_t>& atomIds) {
        const int32_t atomListSize = static_cast<int32_t>(atomIds.size());
        return createAtomsFile(atomIds, atomListSize);
    }

    bool createAtomsFile(const std::vector<int32_t>& atomIds, int headerAtomIdsCount) {
        const int32_t atomListSize = static_cast<int32_t>(atomIds.size());

        // populate the buffer to be written into the file
        const int32_t bufferSize =
                sizeof(FileHeader) + sizeof(BlockHeader) + sizeof(int32_t) * atomListSize;
        std::string buffer;
        buffer.resize(bufferSize);

        char* ptr = buffer.data();
        FileHeader* fileHeader = reinterpret_cast<FileHeader*>(ptr);
        fileHeader->magic_number = kMagicNumber;
        fileHeader->version = kFormatVersion1;
        ptr += sizeof(FileHeader);

        BlockHeader* blockHeader = reinterpret_cast<BlockHeader*>(ptr);
        blockHeader->atomIdsCount = headerAtomIdsCount;
        ptr += sizeof(BlockHeader);

        memcpy(ptr, atomIds.data(), sizeof(int32_t) * atomListSize);

        // create new staging file & removing past version if any
        const std::string stagingFilePath = StringPrintf("%s.tmp", kTestFileName.c_str());
        std::error_code ec;
        std::filesystem::remove(stagingFilePath, ec);

        std::ofstream stagingFile(stagingFilePath.c_str(), std::ios::out | std::ios::binary);
        if (!stagingFile.is_open()) {
            return false;
        }

        stagingFile.write(reinterpret_cast<const char*>(buffer.data()), bufferSize);
        if (stagingFile.fail()) {
            return false;
        }

        stagingFile.flush();
        if (stagingFile.fail()) {
            return false;
        }
        stagingFile.close();

        // rename to predefined file
        std::filesystem::rename(stagingFilePath, kTestFileName, ec);
        if (ec.value() != 0) {
            return false;
        }

        // update file access permissions to be globally read
        std::filesystem::permissions(
                kTestFileName,
                std::filesystem::perms::group_read | std::filesystem::perms::others_read,
                std::filesystem::perm_options::add, ec);
        if (ec.value() != 0) {
            return false;
        }
        return true;
    }

    void setVersionProperty(int64_t version) {
        ASSERT_TRUE(android::base::SetProperty(kTestVersionProperty, std::to_string(version)));
    }

    void setVersionProperty(const std::string& version) {
        ASSERT_TRUE(android::base::SetProperty(kTestVersionProperty, version));
    }
};

TEST_F(AtomsInUseProviderTest, TestNoProperty) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    // With no property, all atoms should be considered in use.
    EXPECT_TRUE(provider.isAtomInUse(123));
    EXPECT_TRUE(provider.isAtomInUse(456));
}

TEST_F(AtomsInUseProviderTest, TestNoFile) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    // With property but no file, all atoms should be considered in use.
    setVersionProperty(1);

    EXPECT_TRUE(provider.isAtomInUse(123));
    EXPECT_TRUE(provider.isAtomInUse(456));
}

TEST_F(AtomsInUseProviderTest, TestBasicFiltering) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    const std::vector<int32_t> atoms = {10, 20, 30};
    EXPECT_TRUE(createAtomsFile(atoms));
    setVersionProperty(1);

    // First call, cache should be populated.
    EXPECT_TRUE(provider.isAtomInUse(10));
    EXPECT_TRUE(provider.isAtomInUse(20));
    EXPECT_TRUE(provider.isAtomInUse(30));
    EXPECT_FALSE(provider.isAtomInUse(40));
    EXPECT_FALSE(provider.isAtomInUse(1));
}

TEST_F(AtomsInUseProviderTest, TestCacheTtl) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    const std::vector<int32_t> atoms1 = {10, 20, 30};
    EXPECT_TRUE(createAtomsFile(atoms1));
    setVersionProperty(1);

    // Populate cache.
    EXPECT_TRUE(provider.isAtomInUse(10));
    EXPECT_FALSE(provider.isAtomInUse(40));

    // Update file and property, but don't advance time yet.
    const std::vector<int32_t> atoms2 = {40, 50, 60};
    EXPECT_TRUE(createAtomsFile(atoms2));
    setVersionProperty(2);

    // Should still use cached values because TTL has not expired.
    EXPECT_TRUE(provider.isAtomInUse(10));
    EXPECT_FALSE(provider.isAtomInUse(40));

    // Advance time past TTL.
    MockClock::advance(kCacheTtlNanos + 1);

    // Now it should re-read and use new values.
    EXPECT_FALSE(provider.isAtomInUse(10));
    EXPECT_TRUE(provider.isAtomInUse(40));
    EXPECT_TRUE(provider.isAtomInUse(50));
    EXPECT_FALSE(provider.isAtomInUse(1));
}

TEST_F(AtomsInUseProviderTest, TestNoVersionChange) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    const std::vector<int32_t> atoms1 = {10, 20, 30};
    EXPECT_TRUE(createAtomsFile(atoms1));
    setVersionProperty(1);

    // Populate cache.
    EXPECT_TRUE(provider.isAtomInUse(10));
    EXPECT_FALSE(provider.isAtomInUse(40));

    // Advance time past TTL.
    MockClock::advance(kCacheTtlNanos + 1);

    // Update file, but NOT the version property.
    const std::vector<int32_t> atoms2 = {40, 50, 60};
    EXPECT_TRUE(createAtomsFile(atoms2));

    // Should still use old values because version hasn't changed.
    EXPECT_TRUE(provider.isAtomInUse(10));
    EXPECT_FALSE(provider.isAtomInUse(40));
}

TEST_F(AtomsInUseProviderTest, TestListReset) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    const std::vector<int32_t> atoms = {10, 20, 30};
    EXPECT_TRUE(createAtomsFile(atoms));
    setVersionProperty(1);

    // Populate cache.
    EXPECT_FALSE(provider.isAtomInUse(40));

    // Reset by removing file and clearing property.
    std::filesystem::remove(kTestFileName);
    setVersionProperty("");

    // Advance time past TTL.
    MockClock::advance(kCacheTtlNanos + 1);

    // Should default to true now.
    EXPECT_TRUE(provider.isAtomInUse(40));
    EXPECT_TRUE(provider.isAtomInUse(10));
}

TEST_F(AtomsInUseProviderTest, TestEmptyList) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);
    const std::vector<int32_t> emptyAtoms = {};
    EXPECT_TRUE(createAtomsFile(emptyAtoms));
    setVersionProperty(1);

    // An empty cache means all atoms are allowed.
    EXPECT_TRUE(provider.isAtomInUse(1));
    EXPECT_TRUE(provider.isAtomInUse(100));
}

TEST_F(AtomsInUseProviderTest, TestCorruptFileInvalidFormat) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);

    // Create a corrupt file - wrong format
    std::ofstream file(kTestFileName, std::ios::binary);
    ASSERT_TRUE(file.is_open());
    int32_t size = 5;
    file.write(reinterpret_cast<const char*>(&size), sizeof(size));
    std::vector<int32_t> atoms = {1, 2, 3};
    file.write(reinterpret_cast<const char*>(atoms.data()), atoms.size() * sizeof(int32_t));
    file.flush();
    ASSERT_FALSE(file.fail());  // Check for write errors before closing
    file.close();               // Close the file after flushing

    setVersionProperty(1);

    // Sync should fail, so it should default to true (allow all).
    EXPECT_TRUE(provider.isAtomInUse(1));
    EXPECT_TRUE(provider.isAtomInUse(100));
}

TEST_F(AtomsInUseProviderTest, TestCorruptFileInvalidAtomCount) {
    AtomsInUseProvider<MockClock> provider(kTestFileName, kTestVersionProperty, kCacheTtlNanos);

    // Create a corrupted file (size header says 5, but only 3 atoms provided).
    std::vector<int32_t> atoms = {1, 2, 3};
    createAtomsFile(atoms, 5);

    setVersionProperty(1);

    // Sync should fail, so it should default to true (allow all).
    EXPECT_TRUE(provider.isAtomInUse(1));
    EXPECT_TRUE(provider.isAtomInUse(100));
}

// Explicitly instantiate the template for the MockClock.
template class AtomsInUseProvider<MockClock>;

#endif  // __ANDROID__