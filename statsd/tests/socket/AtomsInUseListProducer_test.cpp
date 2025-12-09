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
#include "socket/AtomsInUseListProducer.h"

#include <StatsdLoggingControl.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <gtest/gtest.h>

#include "socket_test_helpers.h"
#include "storage/StorageManager.h"

#ifdef __ANDROID__

namespace android {
namespace os {
namespace statsd {

using base::ParseInt;

class AtomsInUseListProducerTest : public ::testing::Test {
protected:
    void SetUp() override {
        cleanupSocketTestFiles(kTestFileName, kTestVersionProperty);
    }

    void TearDown() override {
        cleanupSocketTestFiles(kTestFileName, kTestVersionProperty);
    }
};

TEST_F(AtomsInUseListProducerTest, TestSetAtomsList) {
    AtomsInUseListProducer producer(kTestFileName, kTestVersionProperty);

    std::vector<int32_t> atoms = {2, 3, 4, 100001, 200001, 300001};
    ASSERT_TRUE(producer.setAtomsIds(atoms));
    int64_t listVersion = std::numeric_limits<int64_t>::max();
    ASSERT_TRUE(ParseInt(android::base::GetProperty(kTestVersionProperty, ""), &listVersion));
    verifyAtomsInUseFileContent(kTestFileName, atoms);

    // Test update with a new list
    atoms = {4, 5, 6};
    ASSERT_TRUE(producer.setAtomsIds(atoms));
    int64_t newVersion = std::numeric_limits<int64_t>::max();
    ASSERT_TRUE(ParseInt(android::base::GetProperty(kTestVersionProperty, ""), &newVersion));
    EXPECT_GT(newVersion, listVersion);
    verifyAtomsInUseFileContent(kTestFileName, atoms);
    listVersion = newVersion;

    atoms = {7, 8, 9};
    ASSERT_TRUE(producer.setAtomsIds(atoms));
    ASSERT_TRUE(ParseInt(android::base::GetProperty(kTestVersionProperty, ""), &newVersion));
    EXPECT_GT(newVersion, listVersion);
    verifyAtomsInUseFileContent(kTestFileName, atoms);
}

TEST_F(AtomsInUseListProducerTest, TestResetWithEmptyList) {
    AtomsInUseListProducer producer(kTestFileName, kTestVersionProperty);

    std::vector<int32_t> atoms = {2, 3, 4, 100001, 200001, 300001};
    ASSERT_TRUE(producer.setAtomsIds(atoms));
    int64_t listVersion = std::numeric_limits<int64_t>::max();
    ASSERT_TRUE(ParseInt(android::base::GetProperty(kTestVersionProperty, ""), &listVersion));
    verifyAtomsInUseFileContent(kTestFileName, atoms);

    // Test update with a new list
    atoms = {4, 5, 6};
    ASSERT_TRUE(producer.setAtomsIds(atoms));
    int64_t newVersion = std::numeric_limits<int64_t>::max();
    ASSERT_TRUE(ParseInt(android::base::GetProperty(kTestVersionProperty, ""), &newVersion));
    EXPECT_GT(newVersion, listVersion);
    verifyAtomsInUseFileContent(kTestFileName, atoms);
    listVersion = newVersion;

    // Test reset with an empty list
    std::vector<int32_t> emptyAtoms;
    ASSERT_TRUE(producer.setAtomsIds(emptyAtoms));

    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

TEST_F(AtomsInUseListProducerTest, TestReset) {
    AtomsInUseListProducer producer(kTestFileName, kTestVersionProperty);
    std::vector<int32_t> atoms = {1, 2, 3};

    // Setup
    ASSERT_TRUE(producer.setAtomsIds(atoms));
    int64_t newVersion = std::numeric_limits<int64_t>::max();
    ASSERT_TRUE(ParseInt(android::base::GetProperty(kTestVersionProperty, ""), &newVersion));

    // Reset
    producer.reset();

    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);

    // Reset again should be safe
    producer.reset();
    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

TEST_F(AtomsInUseListProducerTest, TestAtomIdsLimitExceeded) {
    AtomsInUseListProducer producer(kTestFileName, kTestVersionProperty);
    std::vector<int32_t> atoms(AtomsInUseListProducer::kMaxAtomIdsInList + 1, 1);

    ASSERT_FALSE(producer.setAtomsIds(atoms));

    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
