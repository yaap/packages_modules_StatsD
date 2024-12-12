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

#include "src/metrics/HistogramValue.h"

#include <gtest/gtest.h>

#ifdef __ANDROID__

using namespace testing;

namespace android {
namespace os {
namespace statsd {
namespace {

TEST(HistogramValueTest, TestGetCompactedBinCountsSize) {
    EXPECT_EQ(getCompactedBinCountsSize({}), 0);
    EXPECT_EQ(getCompactedBinCountsSize({0}), 1);
    EXPECT_EQ(getCompactedBinCountsSize({1}), 1);
    EXPECT_EQ(getCompactedBinCountsSize({0, 0}), 1);
    EXPECT_EQ(getCompactedBinCountsSize({0, 1}), 2);
    EXPECT_EQ(getCompactedBinCountsSize({1, 0}), 2);
    EXPECT_EQ(getCompactedBinCountsSize({1, 1}), 2);
    EXPECT_EQ(getCompactedBinCountsSize({1, 0, 0}), 2);
    EXPECT_EQ(getCompactedBinCountsSize({1, 0, 1}), 3);
    EXPECT_EQ(getCompactedBinCountsSize({1, 0, 0, 1}), 3);
    EXPECT_EQ(getCompactedBinCountsSize({1, 0, 1, 0}), 4);
    EXPECT_EQ(getCompactedBinCountsSize({0, 0, 1, 0, 0}), 3);
}

TEST(HistogramValueTest, TestErrorBinsMismatch) {
    EXPECT_EQ(HistogramValue({1, 1, 1}) + HistogramValue({2, 2, 3, 4}),
              HistogramValue::ERROR_BINS_MISMATCH);
    EXPECT_EQ(HistogramValue({3, 4, 5}) - HistogramValue({1, 2, 3, 4}),
              HistogramValue::ERROR_BINS_MISMATCH);
}

TEST(HistogramValueTest, TestErrorBinCountTooHigh) {
    EXPECT_EQ(HistogramValue({3, 4, 5}) - HistogramValue({4, 2, 3}),
              HistogramValue::ERROR_BIN_COUNT_TOO_HIGH);
}

}  // anonymous namespace

}  // namespace statsd
}  // namespace os
}  // namespace android
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
