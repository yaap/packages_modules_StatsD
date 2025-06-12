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

#pragma once

#include <gtest/gtest_prod.h>
#include <stdint.h>

#include <unordered_map>
#include <vector>

namespace android {
namespace os {
namespace statsd {

/**
 * @brief This class tracks the logging rate for each atom id.
 *        The rate is calculated as a fixed time window counter
 */
class LoggingRate {
public:
    LoggingRate(int maxStatsNum, int64_t logFrequencyWindowNs);

    void noteLogEvent(uint32_t atomId, int64_t eventTimestampNs);

    // returns max logging rate recorded for atomId if available, 0 otherwise
    int32_t getMaxRate(uint32_t atomId) const;

    using PeakRatePerAtomId = std::pair<int32_t, int32_t>;

    std::vector<PeakRatePerAtomId> getMaxRates(size_t topN) const;

    void reset();

private:
    struct RateInfo {
        int64_t intervalStartNs;
        int32_t rate;
        int32_t maxRate;
    };

    const size_t mMaxRateInfoSize;
    const int64_t mLogFrequencyWindowNs;

    std::unordered_map<uint32_t, RateInfo> mRateInfo;

    FRIEND_TEST(StatsdStatsTest, TestLoggingRateReportReset);
};

}  // namespace statsd
}  // namespace os
}  // namespace android
