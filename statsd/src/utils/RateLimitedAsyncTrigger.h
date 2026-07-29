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

#ifndef RATE_LIMITED_ASYNC_TRIGGER_H
#define RATE_LIMITED_ASYNC_TRIGGER_H

#include <atomic>
#include <functional>
#include <memory>

namespace android {
namespace os {
namespace statsd {

using TimeGetterNs = std::function<int64_t()>;

using TriggerExecutor = std::function<void()>;

class RateLimitedAsyncTrigger {
public:
    RateLimitedAsyncTrigger(int64_t cooldownNs, TimeGetterNs getTimeNs,
                            TriggerExecutor executeTrigger);

    // Checks if the trigger should fire based on the rate limit for this instance
    // and executes the configured trigger function if it should.
    // This function is thread-safe.
    void trigger();

    // Resets the last trigger time for this instance, for testing purposes only.
    void resetForTest();

private:
    const int64_t mCooldownNs;
    const TimeGetterNs mGetTimeNs;
    const TriggerExecutor mExecuteTrigger;
    std::atomic<int64_t> mLastTriggerTimeNs;
};

}  // namespace statsd
}  // namespace os
}  // namespace android

#endif  // RATE_LIMITED_ASYNC_TRIGGER_H
