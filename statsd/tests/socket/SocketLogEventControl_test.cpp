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
#include "socket/SocketLogEventControl.h"

#include <StatsdLoggingControl.h>
#include <android-base/properties.h>
#include <gtest/gtest.h>

#include <vector>

#include "socket_test_helpers.h"
#include "storage/StorageManager.h"
#include "tests/statsd_test_util.h"

#ifdef __ANDROID__

namespace android {
namespace os {
namespace statsd {

class SocketLogEventControlTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!SocketLogEventControl::isSupported()) {
            GTEST_SKIP() << "Skipping all tests for this fixture";
        }
        cleanupSocketTestFiles(kTestFileName, kTestVersionProperty);
    }

    void TearDown() override {
        cleanupSocketTestFiles(kTestFileName, kTestVersionProperty);
    }
};

TEST_F(SocketLogEventControlTest, TestSetAtomIdsWhenDisabled) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(false);

    std::vector<int32_t> atoms = {1, 2, 3};
    control.setAtomIds({atoms.begin(), atoms.end()}, /*consumer=*/(void*)1);

    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

TEST_F(SocketLogEventControlTest, TestSetAtomIdsWhenEnabled) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(true);

    std::vector<int32_t> atoms = {1, 2, 3};
    control.setAtomIds({atoms.begin(), atoms.end()}, /*consumer=*/(void*)1);

    verifyAtomsInUseFileContent(kTestFileName, atoms);
}

TEST_F(SocketLogEventControlTest, TestEnableControlWithAtoms) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    std::vector<int32_t> atoms = {1, 2, 3};
    control.setAtomIds({atoms.begin(), atoms.end()}, /*consumer=*/(void*)1);

    // No file should be created yet.
    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);

    control.setControlEnabled(true);

    verifyAtomsInUseFileContent(kTestFileName, atoms);
}

TEST_F(SocketLogEventControlTest, TestEnableControlWithoutAtoms) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(true);

    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

TEST_F(SocketLogEventControlTest, TestDisableControl) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(true);
    std::vector<int32_t> atoms = {1, 2, 3};
    control.setAtomIds({atoms.begin(), atoms.end()}, /*consumer=*/(void*)1);

    verifyAtomsInUseFileContent(kTestFileName, atoms);

    control.setControlEnabled(false);
    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

TEST_F(SocketLogEventControlTest, TestMultipleConsumers) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(true);

    std::vector<int32_t> atoms1 = {1, 2, 3};
    control.setAtomIds({atoms1.begin(), atoms1.end()}, /*consumer=*/(void*)1);
    verifyAtomsInUseFileContent(kTestFileName, atoms1);
    std::string version1 = android::base::GetProperty(kTestVersionProperty, "");
    EXPECT_NE(version1, "");

    std::vector<int32_t> atoms2 = {3, 4, 5};
    control.setAtomIds({atoms2.begin(), atoms2.end()}, /*consumer=*/(void*)2);
    std::vector<int32_t> expectedAtoms = {1, 2, 3, 4, 5};
    verifyAtomsInUseFileContent(kTestFileName, expectedAtoms);
    std::string version2 = android::base::GetProperty(kTestVersionProperty, "");
    EXPECT_NE(version2, "");
    EXPECT_NE(version1, version2);
}

TEST_F(SocketLogEventControlTest, TestRemoveConsumer) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(true);

    std::vector<int32_t> atoms1 = {1, 2, 3};
    control.setAtomIds({atoms1.begin(), atoms1.end()}, /*consumer=*/(void*)1);
    std::vector<int32_t> atoms2 = {3, 4, 5};
    control.setAtomIds({atoms2.begin(), atoms2.end()}, /*consumer=*/(void*)2);
    std::vector<int32_t> expectedAtoms = {1, 2, 3, 4, 5};
    verifyAtomsInUseFileContent(kTestFileName, expectedAtoms);

    // Remove consumer 2
    control.setAtomIds({}, /*consumer=*/(void*)2);
    verifyAtomsInUseFileContent(kTestFileName, atoms1);
}

TEST_F(SocketLogEventControlTest, TestResetToEmpty) {
    SocketLogEventControl control(kTestFileName, kTestVersionProperty);
    control.setControlEnabled(true);

    std::vector<int32_t> atoms1 = {1, 2, 3};
    control.setAtomIds({atoms1.begin(), atoms1.end()}, /*consumer=*/(void*)1);
    verifyAtomsInUseFileContent(kTestFileName, atoms1);

    // Remove consumer 1, now it's empty
    control.setAtomIds({}, /*consumer=*/(void*)1);
    verifyAtomsInUseListAbsence(kTestFileName, kTestVersionProperty);
}

}  // namespace statsd
}  // namespace os
}  // namespace android

#endif
