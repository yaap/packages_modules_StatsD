/*
 * Copyright (C) 2018 The Android Open Source Project
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
#include "EventMatcherWizard.h"

namespace android {
namespace os {
namespace statsd {

MatchLogEventResult EventMatcherWizard::matchLogEvent(const LogEvent& event, int matcherIndex) {
    if (matcherIndex < 0 || matcherIndex >= (int)mAllEventMatchers.size()) {
        return {MatchingState::kNotComputed, nullptr};
    }
    std::fill(mMatcherCache.begin(), mMatcherCache.end(), MatchingState::kNotComputed);
    // There is only one input of LogEvent - there is only one transformation instance
    // will be produced at a time. Also there is no full support for CombinationAtomMatchingTracker
    // transformations - see INVALID_CONFIG_REASON_MATCHER_COMBINATION_WITH_STRING_REPLACE
    mMatcherTransformations[matcherIndex].reset();
    mAllEventMatchers[matcherIndex]->onLogEvent(event, matcherIndex, mAllEventMatchers,
                                                mMatcherCache, mMatcherTransformations);

    MatchLogEventResult result = {mMatcherCache[matcherIndex],
                                  std::move(mMatcherTransformations[matcherIndex])};
    return result;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
