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

#pragma once

#include <stdint.h>

#include <thread>
#include <unordered_map>

#include "utils.h"

class RealTimeClock {
public:
    static int64_t getTimeNs() {
        return get_elapsed_realtime_ns();
    }
};

template <typename Clock>
class LoggingRateLimiter {
public:
    LoggingRateLimiter(int32_t logFrequencyThreshold, int32_t logFrequencyWindowMs)
        : mLogFrequencyThreshold(logFrequencyThreshold),
          mLogFrequencyWindowNs(logFrequencyWindowMs * 1000000) {
    }

    bool canLogAtom(uint32_t atomId) {
        const int64_t nowNs = Clock::getTimeNs();

        std::unique_lock<std::mutex> lock(mMutex);

        // update current logging frequency
        auto atomFrequencyIt = mLogFrequencies.find(atomId);

        if (atomFrequencyIt != mLogFrequencies.end()) {
            Frequency& frequency = atomFrequencyIt->second;
            if (nowNs - frequency.intervalStartNs >= mLogFrequencyWindowNs) {
                frequency.intervalStartNs = nowNs;
                frequency.logsCount = 1;
                return true;
            } else {
                // update frequency
                frequency.logsCount++;
            }
            return isLoggingUnderThreshold(frequency.logsCount);
        }
        // atomId not found, add it to the map with initial frequency
        mLogFrequencies[atomId] = {nowNs, 1};
        return true;
    }

private:
    bool isLoggingUnderThreshold(int32_t logsCount) const {
        return logsCount <= mLogFrequencyThreshold;
    }

    std::mutex mMutex;

    const int32_t mLogFrequencyThreshold;
    const int64_t mLogFrequencyWindowNs;

    struct Frequency {
        int64_t intervalStartNs;
        int32_t logsCount;
    };

    // Key is atom id.
    std::unordered_map<uint32_t, Frequency> mLogFrequencies;
};
