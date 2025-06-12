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

#define STATSD_DEBUG false  // STOPSHIP if true

#include "Log.h"

#include "LoggingRate.h"

#include <algorithm>

using namespace std;

namespace android {
namespace os {
namespace statsd {

LoggingRate::LoggingRate(int maxStatsNum, int64_t logFrequencyWindowNs)
    : mMaxRateInfoSize(maxStatsNum), mLogFrequencyWindowNs(logFrequencyWindowNs) {
}

void LoggingRate::noteLogEvent(uint32_t atomId, int64_t eventTimestampNs) {
    auto rateInfoIt = mRateInfo.find(atomId);

    if (rateInfoIt != mRateInfo.end()) {
        RateInfo& rateInfo = rateInfoIt->second;
        if (eventTimestampNs - rateInfo.intervalStartNs >= mLogFrequencyWindowNs) {
            rateInfo.intervalStartNs = eventTimestampNs;
            rateInfo.rate = 1;
        } else {
            // update rateInfo
            rateInfo.rate++;
#ifdef STATSD_DEBUG
            if (rateInfo.maxRate < rateInfo.rate) {
                VLOG("For Atom %d new maxRate is %d", atomId, rateInfo.rate);
            }
#endif
            rateInfo.maxRate = max(rateInfo.maxRate, rateInfo.rate);
        }
    } else if (mRateInfo.size() < mMaxRateInfoSize) {
        // atomId not found, add it to the map with initial frequency
        mRateInfo[atomId] = {eventTimestampNs, 1, 1};
    }
}

int32_t LoggingRate::getMaxRate(uint32_t atomId) const {
    const auto rateInfoIt = mRateInfo.find(atomId);
    if (rateInfoIt != mRateInfo.end()) {
        return rateInfoIt->second.maxRate;
    }
    return 0;
}

std::vector<LoggingRate::PeakRatePerAtomId> LoggingRate::getMaxRates(size_t topN) const {
    std::vector<PeakRatePerAtomId> result;
    result.reserve(mRateInfo.size());

    for (auto& [atomId, rateInfo] : mRateInfo) {
        result.emplace_back(atomId, rateInfo.maxRate);
    }

    std::sort(result.begin(), result.end(),
              [](const PeakRatePerAtomId& a, const PeakRatePerAtomId& b) {
                  return a.second > b.second;
              });

    if (topN < result.size()) {
        result.erase(result.begin() + topN, result.end());
    }

    return result;
}

void LoggingRate::reset() {
    mRateInfo.clear();
}

}  // namespace statsd
}  // namespace os
}  // namespace android
