/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "Log.h"

#include "CombinationAtomMatchingTracker.h"

#include "matchers/matcher_util.h"

namespace android {
namespace os {
namespace statsd {

using std::nullopt;
using std::optional;
using std::set;
using std::shared_ptr;
using std::unordered_map;
using std::vector;

CombinationAtomMatchingTracker::CombinationAtomMatchingTracker(const int64_t id,
                                                               const uint64_t protoHash)
    : AtomMatchingTracker(id, protoHash) {
}

CombinationAtomMatchingTracker::~CombinationAtomMatchingTracker() {
}

void CombinationAtomMatchingTracker::init(
        const unordered_map<int64_t, AtomMatcherValue>& allAtomMatcherMap,
        const unordered_map<int64_t, int>& matcherMap) {
    AtomMatcher_Combination matcher =
            (allAtomMatcherMap.find(mId))->second.atomMatcher.combination();
    mLogicalOperation = matcher.operation();
    for (const auto& child : matcher.matcher()) {
        auto pair = matcherMap.find(child);
        int childIndex = pair->second;

        mChildren.push_back(childIndex);

        const set<int>& childTagIds =
                (allAtomMatcherMap.find(child))->second.atomMatchingTracker->getAtomIds();
        mAtomIds.insert(childTagIds.begin(), childTagIds.end());
    }
    return;
}

MatcherValidResult CombinationAtomMatchingTracker::isTrackerValid(
        const std::unordered_map<int64_t, AtomMatcherValue>& allAtomMatcherMap,
        std::unordered_set<int64_t>& stack) const {
    MatcherValidResult matcherValidResult = {nullopt /* invalidConfigReason */,
                                             false /* hasStringTransformation */};
    // mark this node as visited in the recursion stack.
    stack.insert(mId);

    AtomMatcher_Combination matcher = allAtomMatcherMap.find(mId)->second.atomMatcher.combination();

    // LogicalOperation is missing in the config
    if (!matcher.has_operation()) {
        matcherValidResult.invalidConfigReason = createInvalidConfigReasonWithMatcher(
                INVALID_CONFIG_REASON_MATCHER_NO_OPERATION, mId);
        return matcherValidResult;
    }

    if (matcher.operation() == LogicalOperation::NOT && matcher.matcher_size() != 1) {
        matcherValidResult.invalidConfigReason = createInvalidConfigReasonWithMatcher(
                INVALID_CONFIG_REASON_MATCHER_NOT_OPERATION_IS_NOT_UNARY, mId);
        return matcherValidResult;
    }

    for (const auto& child : matcher.matcher()) {
        auto pair = allAtomMatcherMap.find(child);
        if (pair == allAtomMatcherMap.end() || pair->second.atomMatchingTracker == nullptr) {
            ALOGW("Matcher %lld not found in the config", (long long)child);
            matcherValidResult.invalidConfigReason = createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_MATCHER_CHILD_NOT_FOUND, mId);
            matcherValidResult.invalidConfigReason->matcherIds.push_back(child);
            return matcherValidResult;
        }

        int64_t childMatcherId = pair->first;

        // if the child is a visited node in the recursion -> circle detected.
        if (stack.find(childMatcherId) != stack.end()) {
            ALOGE("Circle detected in matcher config");
            matcherValidResult.invalidConfigReason =
                    createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_CYCLE, mId);
            return matcherValidResult;
        }

        auto [invalidConfigReason, hasStringTransformation] =
                allAtomMatcherMap.find(childMatcherId)
                        ->second.atomMatchingTracker->isTrackerValid(allAtomMatcherMap, stack);
        if (hasStringTransformation) {
            ALOGE("String transformation detected in CombinationMatcher");
            matcherValidResult.invalidConfigReason = createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_MATCHER_COMBINATION_WITH_STRING_REPLACE, mId);
            matcherValidResult.hasStringTransformation = true;
            return matcherValidResult;
        }

        if (invalidConfigReason.has_value()) {
            ALOGW("child matcher init failed %lld", (long long)child);
            invalidConfigReason->matcherIds.push_back(mId);
            matcherValidResult.invalidConfigReason = invalidConfigReason;
            return matcherValidResult;
        }
    }
    stack.erase(mId);
    return matcherValidResult;
}

void CombinationAtomMatchingTracker::onConfigUpdated(
        const AtomMatcher& matcher, const unordered_map<int64_t, int>& atomMatchingTrackerMap) {
    mChildren.clear();
    const AtomMatcher_Combination& combinationMatcher = matcher.combination();
    for (const int64_t child : combinationMatcher.matcher()) {
        const auto& pair = atomMatchingTrackerMap.find(child);
        mChildren.push_back(pair->second);
    }
}

void CombinationAtomMatchingTracker::onLogEvent(
        const LogEvent& event, int matcherIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        vector<MatchingState>& matcherResults,
        vector<shared_ptr<LogEvent>>& matcherTransformations) {
    // this event has been processed.
    if (matcherResults[matcherIndex] != MatchingState::kNotComputed) {
        return;
    }

    if (mAtomIds.find(event.GetTagId()) == mAtomIds.end()) {
        matcherResults[matcherIndex] = MatchingState::kNotMatched;
        return;
    }

    // evaluate children matchers if they haven't been evaluated.
    for (const int childIndex : mChildren) {
        if (matcherResults[childIndex] == MatchingState::kNotComputed) {
            const sp<AtomMatchingTracker>& child = allAtomMatchingTrackers[childIndex];
            child->onLogEvent(event, childIndex, allAtomMatchingTrackers, matcherResults,
                              matcherTransformations);
        }
    }

    bool matched = combinationMatch(mChildren, mLogicalOperation, matcherResults);
    matcherResults[matcherIndex] = matched ? MatchingState::kMatched : MatchingState::kNotMatched;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
