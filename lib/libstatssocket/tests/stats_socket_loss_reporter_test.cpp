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

#include "stats_socket_loss_reporter.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "stats_annotations.h"
#include "stats_event.h"

using namespace ::testing;

static int writeEvent(int i) {
    AStatsEvent* event = AStatsEvent_obtain();
    AStatsEvent_setAtomId(event, i);
    AStatsEvent_writeInt32(event, 5);
    int successResult = AStatsEvent_write(event);
    AStatsEvent_release(event);
    return successResult;
}

constexpr int kTestAtomIdStart = 400'000;

TEST(SocketLossReporterTest, TestMultithreadedWritesDoNotDeadlock) {
    const int atomsToWriteBackToBack = 500'000;
    const int numThreads = 4;

    std::vector<std::thread> threads;
    std::vector<std::atomic<bool>> finishedFlags(numThreads);

    auto worker = [&](int threadIndex) {
        for (int i = 0; i < atomsToWriteBackToBack; ++i) {
            writeEvent(kTestAtomIdStart);
        }
        finishedFlags[threadIndex] = true;
    };

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker, i);
    }

    // single atom write takes <10 microseconds
    // if thread functions are not finished within 500'000 * 10us ~ 5 second
    // test is failed

    sleep(5);

    for (int i = 0; i < numThreads; ++i) {
        EXPECT_TRUE(finishedFlags[i]) << "Thread " << i << " did not finish.";
    }

    for (auto& t : threads) {
        t.join();
    }
}

void dumpAtomsLossStats(int iterations, bool doForce) {
    for (int i = 0; i < iterations; i++) {
        StatsSocketLossReporter::getInstance().dumpAtomsLossStats(doForce);
    }
}

class SocketLossReporterDeadlockTest : public ::testing::TestWithParam<bool> {};

TEST_P(SocketLossReporterDeadlockTest, TestDumpAtomsLossStatsDoesNotDeadlock) {
    const bool forceDump = GetParam();
    const int atomsToWriteBackToBack = 500'000;

    std::atomic_bool workerThreadFinished = false;
    std::atomic_bool terminateWorkThreads = false;

    std::thread workerThread([&]() {
        dumpAtomsLossStats(atomsToWriteBackToBack, forceDump);
        workerThreadFinished = true;
    });

    std::atomic_bool terminateSpamThreads = false;
    std::thread spamThread1([&]() {
        while (!terminateSpamThreads) {
            for (int i = 0; i < 100; i++) {
                writeEvent(kTestAtomIdStart + i);
            }
        }
    });
    std::thread spamThread2([&]() {
        while (!terminateSpamThreads) {
            for (int e = 1; e < 5; e++) {
                for (int i = 0; i < 100; i++) {
                    StatsSocketLossReporter::getInstance().noteDrop(e, kTestAtomIdStart + i);
                }
            }
        }
    });

    // single atom write takes <10 microseconds
    // if thread functions are not finished within 500'000 * 10us ~ 5 second
    // test is failed

    sleep(5);

    terminateWorkThreads = true;
    terminateSpamThreads = true;

    EXPECT_TRUE(workerThreadFinished) << "Worker thread blocked for a longer than expected";

    workerThread.join();
    spamThread1.join();
    spamThread2.join();
}

INSTANTIATE_TEST_SUITE_P(SocketLossReporterTest, SocketLossReporterDeadlockTest,
                         Values(false, true), [](const TestParamInfo<bool>& info) {
                             return info.param ? "ForcedDump" : "CooledDownDump";
                         });
