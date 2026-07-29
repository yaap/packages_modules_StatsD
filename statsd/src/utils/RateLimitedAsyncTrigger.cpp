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

#include "utils/RateLimitedAsyncTrigger.h"

namespace android {
namespace os {
namespace statsd {

RateLimitedAsyncTrigger::RateLimitedAsyncTrigger(int64_t cooldownNs, TimeGetterNs getTimeNs,
                                                 TriggerExecutor executeTrigger)
    : mCooldownNs(cooldownNs),
      mGetTimeNs(std::move(getTimeNs)),
      mExecuteTrigger(std::move(executeTrigger)),
      mLastTriggerTimeNs(0) {
    if (!mGetTimeNs || !mExecuteTrigger) {
        ALOGE("TimeGetterNs and TriggerExecutor cannot be null!");
    }
}

void RateLimitedAsyncTrigger::trigger() {
    int64_t nowNs = mGetTimeNs();
    int64_t lastNs = mLastTriggerTimeNs.load(std::memory_order_relaxed);

    const int64_t diff = nowNs - lastNs;

    if (lastNs != 0 && diff >= 0 && diff < mCooldownNs) {
        VLOG("Trigger rate-limited");
        return;
    }

    if (mLastTriggerTimeNs.compare_exchange_strong(lastNs, nowNs)) {
        mExecuteTrigger();
    }
}

void RateLimitedAsyncTrigger::resetForTest() {
    mLastTriggerTimeNs.store(0, std::memory_order_relaxed);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
