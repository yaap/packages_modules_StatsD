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

#include "utils/RateLimitedAsyncTrigger.h"

#include <gtest/gtest.h>

#ifdef __ANDROID__

using namespace std;

namespace android {
namespace os {
namespace statsd {

namespace {
int64_t g_now_elapsed_ns = 0;
int g_trigger_call_count = 0;

int64_t fakeGetElapsedRealtimeNs() {
    return g_now_elapsed_ns;
}
void fakeTriggerExecutor() {
    g_trigger_call_count++;
}

constexpr int64_t TEST_COOLDOWN_NS = 1000;

class RateLimitedTriggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_now_elapsed_ns = 0;
        g_trigger_call_count = 0;
    }

    // Construct the instance under test here
    RateLimitedAsyncTrigger triggerInstance{TEST_COOLDOWN_NS, fakeGetElapsedRealtimeNs,
                                            fakeTriggerExecutor};
};
}  // namespace

TEST_F(RateLimitedTriggerTest, TriggerFiresFirstTime) {
    g_now_elapsed_ns = 100;
    triggerInstance.trigger();
    EXPECT_EQ(g_trigger_call_count, 1);
}

TEST_F(RateLimitedTriggerTest, TriggerRateLimited) {
    g_now_elapsed_ns = 100;
    triggerInstance.trigger();
    g_now_elapsed_ns += TEST_COOLDOWN_NS - 1;
    triggerInstance.trigger();
    EXPECT_EQ(g_trigger_call_count, 1);
}

TEST_F(RateLimitedTriggerTest, TriggerFiresAfterCooldown) {
    g_now_elapsed_ns = 100;
    triggerInstance.trigger();
    g_now_elapsed_ns += TEST_COOLDOWN_NS;
    triggerInstance.trigger();
    EXPECT_EQ(g_trigger_call_count, 2);

    g_now_elapsed_ns += 1;
    triggerInstance.trigger();
    EXPECT_EQ(g_trigger_call_count, 2);
}

TEST_F(RateLimitedTriggerTest, ResetForTest) {
    g_now_elapsed_ns = 100;
    triggerInstance.trigger();
    EXPECT_EQ(g_trigger_call_count, 1);

    g_now_elapsed_ns += 1;
    triggerInstance.resetForTest();
    triggerInstance.trigger();
    EXPECT_EQ(g_trigger_call_count, 2);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
