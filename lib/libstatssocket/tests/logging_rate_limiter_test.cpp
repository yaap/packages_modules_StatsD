/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "logging_rate_limiter.h"

#include <gtest/gtest.h>

using namespace ::testing;

class ManualClock {
    static int64_t mTime;

public:
    static int64_t getTimeNs() {
        return mTime;
    }

    static void setTimeNs(int64_t newTime) {
        mTime = newTime;
    }

    static void advanceTimeNs(int64_t advanceTimeValue) {
        mTime += advanceTimeValue;
    }
};

int64_t ManualClock::mTime = 0;

TEST(RateLimiter, TestRateLimitActivatedSimple) {
    // write events in a tight loop
    // LoggingRateLimiter should be activated after first event

    constexpr int32_t kLogFrequencyThreshold = 1;
    constexpr int32_t kLoggingFrequencyWindowMs = 1;

    LoggingRateLimiter<ManualClock> rateLimiter(kLogFrequencyThreshold, kLoggingFrequencyWindowMs);
    ManualClock::setTimeNs(0);

    EXPECT_TRUE(rateLimiter.canLogAtom(/*atomId*/ 1));
    EXPECT_FALSE(rateLimiter.canLogAtom(/*atomId*/ 1));

    // advance clock to next time window
    ManualClock::advanceTimeNs(kLoggingFrequencyWindowMs * 1'000'000);

    EXPECT_TRUE(rateLimiter.canLogAtom(/*atomId*/ 1));
    EXPECT_FALSE(rateLimiter.canLogAtom(/*atomId*/ 1));
}

TEST(RateLimiter, TestRateLimitActivated) {
    // write events in a tight loop
    // LoggingRateLimiter should be activated 100 events

    constexpr int32_t kLogFrequencyThreshold = 100;
    constexpr int32_t kLoggingFrequencyWindowMs = 100;
    constexpr int32_t kMaxTestEvents = 2400;

    LoggingRateLimiter<ManualClock> rateLimiter(kLogFrequencyThreshold, kLoggingFrequencyWindowMs);
    ManualClock::setTimeNs(0);

    int32_t eventsCount = 0;
    for (int event = 0; event < kMaxTestEvents; event++) {
        if (rateLimiter.canLogAtom(/*atomId*/ 1)) {
            eventsCount++;
        }
    }
    EXPECT_EQ(eventsCount, kLogFrequencyThreshold);
}

TEST(RateLimiter, TestRateLimitNotActivated) {
    // write events in a tight loop
    // LoggingRateLimiter should not be activated

    constexpr int32_t kLogFrequencyThreshold = 100;
    constexpr int32_t kLoggingFrequencyWindowMs = 100;
    constexpr int32_t kMaxTestEvents = 2400;

    LoggingRateLimiter<ManualClock> rateLimiter(kLogFrequencyThreshold, kLoggingFrequencyWindowMs);
    ManualClock::setTimeNs(0);

    int32_t eventsCount = 0;
    for (int event = 0; event < kMaxTestEvents; event++) {
        if (rateLimiter.canLogAtom(/*atomId*/ 1)) {
            eventsCount++;
        }
        // Simulate logging at single event per 10ms pace to satisfy rate limit
        // restrictions
        ManualClock::advanceTimeNs(10 * 1'000'000);
    }

    // check the values are in the range allowing 10% deviations
    EXPECT_EQ(eventsCount, kMaxTestEvents);
}

TEST(RateLimiter, TestRateLimitAcrossTimeWindow) {
    // write events in a tight loop
    // LoggingRateLimiter should be activated after 100 events within 100ms
    // time window, but once next window starts - it will allow logging again

    constexpr int32_t kLogFrequencyThreshold = 100;
    constexpr int32_t kLoggingFrequencyWindowMs = 100;
    constexpr int32_t kMaxTestEvents = 2400;

    LoggingRateLimiter<ManualClock> rateLimiter(kLogFrequencyThreshold, kLoggingFrequencyWindowMs);
    ManualClock::setTimeNs(0);

    int32_t eventsCount = 0;
    for (int event = 0; event < kMaxTestEvents; event++) {
        if (rateLimiter.canLogAtom(/*atomId*/ 1)) {
            eventsCount++;
        }
    }
    EXPECT_EQ(eventsCount, kLogFrequencyThreshold);

    // advance clock to next time window
    ManualClock::advanceTimeNs(kLoggingFrequencyWindowMs * 1'000'000);

    eventsCount = 0;
    for (int event = 0; event < kMaxTestEvents; event++) {
        if (rateLimiter.canLogAtom(/*atomId*/ 1)) {
            eventsCount++;
        }
    }
    EXPECT_EQ(eventsCount, kLogFrequencyThreshold);
}
