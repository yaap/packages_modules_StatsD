/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <com_android_os_statsd_flags.h>
#include <flag_macros.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "stats_buffer_writer.h"
#include "stats_event.h"
#include "stats_socket.h"
#include "utils.h"

using namespace ::testing;

#define TEST_NS com::android::os::statsd::flags

TEST(StatsWriterTest, TestSocketClose) {
    AStatsEvent* event = AStatsEvent_obtain();
    AStatsEvent_setAtomId(event, 100);
    AStatsEvent_writeInt32(event, 5);
    int successResult = AStatsEvent_write(event);
    AStatsEvent_release(event);

    // In the case of a successful write, we return the number of bytes written.
    EXPECT_GT(successResult, 0);
    EXPECT_FALSE(stats_log_is_closed());

    AStatsSocket_close();

    EXPECT_TRUE(stats_log_is_closed());
}

TEST_WITH_FLAGS(StatsWriterTest, TestRateLimit,
                REQUIRES_FLAGS_ENABLED(ACONFIG_FLAG(TEST_NS, logging_rate_limit_enabled))) {
    // write events in a tight loop
    // libstatssocket should start rate limit after 240 events

    const int32_t maxTestEvents = 2400;
    const int64_t startNs = get_elapsed_realtime_ns();
    int32_t eventsCount = 0;
    for (int i = 0; i < maxTestEvents; i++) {
        AStatsEvent* event = AStatsEvent_obtain();
        AStatsEvent_setAtomId(event, 100);
        AStatsEvent_writeInt32(event, 5);
        int bytesWritten = AStatsEvent_write(event);
        AStatsEvent_release(event);
        if (bytesWritten > 0) {
            eventsCount++;
        }
    }

    const int64_t timeToRateLimitMs = (get_elapsed_realtime_ns() - startNs) / 1'000'000;

    // threshold values are aligned with stats_buffer_writer.cpp
    constexpr int32_t kLogFrequencyThreshold = 240;
    constexpr int32_t kLoggingFrequencyWindowMs = 100;

    EXPECT_LE(eventsCount, kLogFrequencyThreshold);
    EXPECT_THAT(timeToRateLimitMs, Le(kLoggingFrequencyWindowMs));
}
