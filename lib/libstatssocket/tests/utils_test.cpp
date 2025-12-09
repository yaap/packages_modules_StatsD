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

#include "utils.h"

#include <gtest/gtest.h>

using namespace ::testing;

TEST(CooldownTimerTest, TestInitialState) {
    CooldownTimer timer(1000);
    // Timer should be expired initially.
    EXPECT_TRUE(timer.isExpired(0));
    EXPECT_TRUE(timer.isExpired(1000));
}

TEST(CooldownTimerTest, TestStartAndExpiry) {
    const int64_t timeoutNanos = 100 * 1000 * 1000;  // 100ms
    CooldownTimer timer(timeoutNanos);

    const int64_t startTimeNanos = 5000;
    timer.start(startTimeNanos);

    // Check before expiry
    EXPECT_FALSE(timer.isExpired(startTimeNanos));
    EXPECT_FALSE(timer.isExpired(startTimeNanos + timeoutNanos - 1));

    // Check at expiry
    EXPECT_TRUE(timer.isExpired(startTimeNanos + timeoutNanos));

    // Check after expiry
    EXPECT_TRUE(timer.isExpired(startTimeNanos + timeoutNanos + 1));
}

TEST(CooldownTimerTest, TestRestart) {
    const int64_t timeoutNanos = 100 * 1000 * 1000;  // 100ms
    CooldownTimer timer(timeoutNanos);

    const int64_t firstStartTimeNanos = 5000;
    timer.start(firstStartTimeNanos);

    const int64_t secondStartTimeNanos = firstStartTimeNanos + 50 * 1000 * 1000;  // 50ms later
    timer.start(secondStartTimeNanos);

    // Expiry should be based on the second start time now.
    EXPECT_FALSE(timer.isExpired(firstStartTimeNanos + timeoutNanos - 1));
    EXPECT_FALSE(timer.isExpired(secondStartTimeNanos + timeoutNanos - 1));
    EXPECT_TRUE(timer.isExpired(secondStartTimeNanos + timeoutNanos));
}

TEST(CooldownTimerTest, TestZeroTimeout) {
    const int64_t timeoutNanos = 0;
    CooldownTimer timer(timeoutNanos);

    const int64_t startTimeNanos = 5000;
    timer.start(startTimeNanos);

    // With zero timeout, it should be expired immediately.
    EXPECT_TRUE(timer.isExpired(startTimeNanos));
    EXPECT_TRUE(timer.isExpired(startTimeNanos + 1));
}