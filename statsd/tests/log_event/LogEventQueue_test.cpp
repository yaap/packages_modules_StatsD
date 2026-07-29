// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "logd/LogEventQueue.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdio.h>

#include <thread>

#include "socket/StatsSocketListener.h"
#include "stats_event.h"
#include "tests/statsd_test_util.h"

namespace android {
namespace os {
namespace statsd {

using namespace android;
using namespace testing;

using std::unique_ptr;

namespace {

AStatsEvent* makeStatsEvent(uint64_t timestampNs) {
    AStatsEvent* statsEvent = AStatsEvent_obtain();
    AStatsEvent_setAtomId(statsEvent, 10);
    AStatsEvent_overwriteTimestamp(statsEvent, timestampNs);
    AStatsEvent_build(statsEvent);
    return statsEvent;
}

std::unique_ptr<LogEvent> makeLogEvent(uint64_t timestampNs) {
    AStatsEvent* statsEvent = makeStatsEvent(timestampNs);
    std::unique_ptr<LogEvent> logEvent = std::make_unique<LogEvent>(/*uid=*/0, /*pid=*/0);
    parseStatsEventToLogEvent(statsEvent, logEvent.get());
    EXPECT_EQ(logEvent->GetElapsedTimestampNs(), timestampNs);
    return logEvent;
}

}  // anonymous namespace

#ifdef __ANDROID__
TEST(LogEventQueue_test, TestGoodConsumer) {
    LogEventQueue queue(50);
    int64_t eventTimeNs = 100;
    std::thread writer([&queue, eventTimeNs] {
        LogEventQueue::Result result;
        for (int i = 0; i < 100; i++) {
            result = queue.push(makeLogEvent(eventTimeNs + i * 1000));
            EXPECT_TRUE(result.success);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread reader([&queue, eventTimeNs] {
        for (int i = 0; i < 100; i++) {
            auto event = queue.waitPop();
            EXPECT_TRUE(event != nullptr);
            // All events are in right order.
            EXPECT_EQ(eventTimeNs + i * 1000, event->GetElapsedTimestampNs());
        }
    });

    reader.join();
    writer.join();
}

TEST(LogEventQueue_test, TestSlowConsumer) {
    LogEventQueue queue(50);
    int64_t eventTimeNs = 100;
    std::thread writer([&queue, eventTimeNs] {
        int failure_count = 0;
        LogEventQueue::Result result;
        for (int i = 0; i < 100; i++) {
            result = queue.push(makeLogEvent(eventTimeNs + i * 1000));
            if (!result.success) {
                failure_count++;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // There is some remote chance that reader thread not get chance to run before writer thread
        // ends. That's why the following comparison is not "==".
        // There will be at least 45 events lost due to overflow.
        EXPECT_TRUE(failure_count >= 45);
        // The oldest event must be at least the 6th event.
        EXPECT_TRUE(result.oldestTimestampNs <= (100 + 5 * 1000));
    });

    std::thread reader([&queue, eventTimeNs] {
        // The consumer quickly processed 5 events, then it got stuck (not reading anymore).
        for (int i = 0; i < 5; i++) {
            auto event = queue.waitPop();
            EXPECT_TRUE(event != nullptr);
            // All events are in right order.
            EXPECT_EQ(eventTimeNs + i * 1000, event->GetElapsedTimestampNs());
        }
    });

    reader.join();
    writer.join();
}

TEST(LogEventQueue_test, TestOverflowAndRecovery) {
    // This test verifies the queue's behavior during and after an overflow event.
    // It ensures that the queue correctly identifies the start of an overflow,
    // handles subsequent overflowed events, and then correctly recovers
    // once space becomes available. This exercises the code paths that trigger
    // atrace events for overflow start and end.
    LogEventQueue queue(10);  // Use a small queue for easier testing.
    int64_t eventTimeNs = 100;

    // 1. Fill the queue to its limit.
    for (int i = 0; i < 10; i++) {
        auto result = queue.push(makeLogEvent(eventTimeNs + i * 1000));
        EXPECT_TRUE(result.success);
        EXPECT_EQ(i + 1, result.size);
    }

    // 2. Push one more item to trigger the start of an overflow.
    // This is where ATRACE_BEGIN("Statsd::QueueOverflow") should be called.
    auto result = queue.push(makeLogEvent(eventTimeNs + 10 * 1000));
    EXPECT_FALSE(result.success);
    EXPECT_EQ(10, result.size);
    EXPECT_EQ(eventTimeNs, result.oldestTimestampNs);  // oldest is the first event

    // 3. Push a few more items to simulate a continued overflow.
    // The overflow lost count should be incrementing here.
    for (int i = 11; i < 15; i++) {
        result = queue.push(makeLogEvent(eventTimeNs + i * 1000));
        EXPECT_FALSE(result.success);
        EXPECT_EQ(10, result.size);
    }

    // 4. Pop an item from the queue to make space.
    auto event = queue.waitPop();
    EXPECT_TRUE(event != nullptr);
    EXPECT_EQ(eventTimeNs, event->GetElapsedTimestampNs());

    // 5. Push another item. This should now succeed and end the overflow.
    // This is where ATRACE_END() and ATRACE_INT(...) should be called.
    result = queue.push(makeLogEvent(eventTimeNs + 15 * 1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(10, result.size);

    // 6. Push one more item. This should also succeed and should not trigger
    // any new overflow-related trace events.
    event = queue.waitPop();  // Make space first.
    EXPECT_TRUE(event != nullptr);
    result = queue.push(makeLogEvent(eventTimeNs + 16 * 1000));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(10, result.size);
}

TEST(LogEventQueue_test, TestQueueMaxSize) {
    StatsdStats::getInstance().reset();

    LogEventQueue queue(50);
    LogEventFilter filter;
    filter.setFilteringEnabled(false);

    int64_t eventTimeNs = 100;
    int64_t oldestEventNs = 0;
    int32_t newSize = 0;
    for (int i = 0; i < 30; i++, eventTimeNs++) {
        auto statsEvent = makeStatsEvent(eventTimeNs);
        size_t bufferSize;
        const uint8_t* buffer = AStatsEvent_getBuffer(statsEvent, &bufferSize);
        StatsSocketListener::processStatsEventBuffer(buffer, bufferSize, 0, 0, queue, filter);
        AStatsEvent_release(statsEvent);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObserved, i + 1);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObservedElapsedNanos, eventTimeNs);
    }

    const int32_t lastMaxSizeObserved = StatsdStats::getInstance().mEventQueueMaxSizeObserved;
    const int64_t lastMaxSizeElapsedNanos =
            StatsdStats::getInstance().mEventQueueMaxSizeObservedElapsedNanos;

    // consumer reads the entire queue
    int64_t nextEventTs = 100;
    for (int i = 0; i < 30; i++, nextEventTs++) {
        auto event = queue.waitPop();
        EXPECT_TRUE(event != nullptr);
        // All events are in right order.
        EXPECT_EQ(nextEventTs, event->GetElapsedTimestampNs());
    }

    // the expectation after queue drained entirely the max count & ts do not update for
    // smaller values
    {
        auto statsEvent = makeStatsEvent(eventTimeNs);
        size_t bufferSize;
        const uint8_t* buffer = AStatsEvent_getBuffer(statsEvent, &bufferSize);
        StatsSocketListener::processStatsEventBuffer(buffer, bufferSize, 0, 0, queue, filter);
        AStatsEvent_release(statsEvent);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObserved, lastMaxSizeObserved);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObservedElapsedNanos,
                  lastMaxSizeElapsedNanos);
        eventTimeNs++;
    }

    for (int i = 0; i < 1; i++, nextEventTs++) {
        auto event = queue.waitPop();
        EXPECT_TRUE(event != nullptr);
        // All events are in right order.
        EXPECT_EQ(nextEventTs, event->GetElapsedTimestampNs());
    }

    // the expectation after queue drained entirely the max count & ts do update for
    // bigger values
    // fill up to the the previous max values observed - stats are not changed
    for (int i = 0; i < lastMaxSizeObserved; i++, eventTimeNs++) {
        auto statsEvent = makeStatsEvent(eventTimeNs);
        size_t bufferSize;
        const uint8_t* buffer = AStatsEvent_getBuffer(statsEvent, &bufferSize);
        StatsSocketListener::processStatsEventBuffer(buffer, bufferSize, 0, 0, queue, filter);
        AStatsEvent_release(statsEvent);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObserved, lastMaxSizeObserved);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObservedElapsedNanos,
                  lastMaxSizeElapsedNanos);
    }

    // add extra elements to update the stats
    for (int i = 0; i < 10; i++, eventTimeNs++) {
        auto statsEvent = makeStatsEvent(eventTimeNs);
        size_t bufferSize;
        const uint8_t* buffer = AStatsEvent_getBuffer(statsEvent, &bufferSize);
        StatsSocketListener::processStatsEventBuffer(buffer, bufferSize, 0, 0, queue, filter);
        AStatsEvent_release(statsEvent);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObserved,
                  lastMaxSizeObserved + i + 1);
        EXPECT_EQ(StatsdStats::getInstance().mEventQueueMaxSizeObservedElapsedNanos, eventTimeNs);
    }
}

#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif

}  // namespace statsd
}  // namespace os
}  // namespace android
