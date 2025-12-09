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

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"
#include "CombinationConditionTracker.h"

namespace android {
namespace os {
namespace statsd {

using std::nullopt;
using std::optional;
using std::unordered_map;
using std::unordered_set;
using std::vector;

CombinationConditionTracker::CombinationConditionTracker(const int64_t id, const uint64_t protoHash)
    : ConditionTracker(id, protoHash) {
    VLOG("creating CombinationConditionTracker %lld", (long long)mConditionId);
}

CombinationConditionTracker::~CombinationConditionTracker() {
    VLOG("~CombinationConditionTracker() %lld", (long long)mConditionId);
}

void CombinationConditionTracker::init(
        const int index, const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& conditionIdIndexMap,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        unordered_set<int64_t>& initializedTrackers, vector<ConditionState>& conditionCache) {
    mIndex = index;
    VLOG("Combination predicate init() %lld", (long long)mConditionId);
    if (initializedTrackers.contains(mConditionId)) {
        // All the children are guaranteed to be initialized, but the recursion is needed to
        // fill the conditionCache properly, since another combination condition or metric
        // might rely on this. The recursion is needed to compute the current condition.

        // Init is called instead of isConditionMet so that the ConditionKey can be filled with the
        // default key for sliced conditions, since we do not know all indirect descendants here.
        for (const int childIndex : mChildren) {
            if (conditionCache[childIndex] == ConditionState::kNotEvaluated) {
                allConditionTrackers[childIndex]->init(
                        childIndex, allConditionsMap, allConditionTrackers, conditionIdIndexMap,
                        atomMatchingTrackerMap, initializedTrackers, conditionCache);
            }
        }
        conditionCache[mIndex] =
                evaluateCombinationCondition(mChildren, mLogicalOperation, conditionCache);
        return;
    }
    Predicate_Combination combinationCondition =
            allConditionsMap.find(mConditionId)->second.predicate.combination();

    mLogicalOperation = combinationCondition.operation();

    for (auto child : combinationCondition.predicate()) {
        auto it = conditionIdIndexMap.find(child);
        int childIndex = it->second;
        const auto& childTracker = allConditionTrackers[childIndex];

        // Recursively init the child trackers first
        childTracker->init(childIndex, allConditionsMap, allConditionTrackers, conditionIdIndexMap,
                           atomMatchingTrackerMap, initializedTrackers, conditionCache);
        if (allConditionTrackers[childIndex]->isSliced()) {
            setSliced(true);
            mSlicedChildren.push_back(childIndex);
        } else {
            mUnSlicedChildren.push_back(childIndex);
        }
        mChildren.push_back(childIndex);
        mTrackerIndex.insert(childTracker->getAtomMatchingTrackerIndex().begin(),
                             childTracker->getAtomMatchingTrackerIndex().end());
    }
    mUnSlicedPartCondition =
            evaluateCombinationCondition(mUnSlicedChildren, mLogicalOperation, conditionCache);
    conditionCache[mIndex] =
            evaluateCombinationCondition(mChildren, mLogicalOperation, conditionCache);

    initializedTrackers.insert(mConditionId);

    return;
}

const optional<InvalidConfigReason> CombinationConditionTracker::isTrackerValid(
        const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        unordered_set<int64_t>& stack) const {
    stack.insert(mConditionId);

    Predicate_Combination combinationCondition =
            allConditionsMap.find(mConditionId)->second.predicate.combination();
    optional<InvalidConfigReason> invalidConfigReason;

    for (auto child : combinationCondition.predicate()) {
        auto it = allConditionsMap.find(child);

        if (it == allConditionsMap.end() || it->second.conditionTracker == nullptr) {
            ALOGW("Predicate %lld not found in the config", (long long)child);
            invalidConfigReason = createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_CONDITION_CHILD_NOT_FOUND, mConditionId);
            invalidConfigReason->conditionIds.push_back(child);
            return invalidConfigReason;
        }

        // if the child is a visited node in the recursion -> circle detected.
        if (stack.contains(child)) {
            ALOGW("Circle detected!!!");
            invalidConfigReason = createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_CONDITION_CYCLE, mConditionId);
            return invalidConfigReason;
        }

        sp<ConditionTracker> childTracker = it->second.conditionTracker;
        invalidConfigReason = childTracker->isTrackerValid(allConditionsMap, stack);

        if (invalidConfigReason.has_value()) {
            ALOGW("Child initialization failed %lld ", (long long)child);
            invalidConfigReason->conditionIds.push_back(mConditionId);
            return invalidConfigReason;
        } else {
            VLOG("Child initialization success %lld ", (long long)child);
        }
    }
    stack.erase(mConditionId);
    return nullopt;
}

void CombinationConditionTracker::onConfigUpdated(
        const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap, const int index,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap) {
    ConditionTracker::onConfigUpdated(allConditionsMap, index, allConditionTrackers,
                                      atomMatchingTrackerMap, conditionTrackerMap);
    mTrackerIndex.clear();
    mChildren.clear();
    mUnSlicedChildren.clear();
    mSlicedChildren.clear();
    Predicate_Combination combinationCondition =
            allConditionsMap.find(mConditionId)->second.predicate.combination();

    for (const int64_t child : combinationCondition.predicate()) {
        const auto& it = conditionTrackerMap.find(child);

        int childIndex = it->second;
        const sp<ConditionTracker>& childTracker = allConditionTrackers[childIndex];

        // Ensures that the child's tracker indices are updated.
        childTracker->onConfigUpdated(allConditionsMap, childIndex, allConditionTrackers,
                                      atomMatchingTrackerMap, conditionTrackerMap);
        if (allConditionTrackers[childIndex]->isSliced()) {
            mSlicedChildren.push_back(childIndex);
        } else {
            mUnSlicedChildren.push_back(childIndex);
        }
        mChildren.push_back(childIndex);
        mTrackerIndex.insert(childTracker->getAtomMatchingTrackerIndex().begin(),
                             childTracker->getAtomMatchingTrackerIndex().end());
    }
    return;
}

void CombinationConditionTracker::isConditionMet(
        const ConditionKey& conditionParameters, const vector<sp<ConditionTracker>>& allConditions,
        const bool isPartialLink,
        vector<ConditionState>& conditionCache) const {
    // So far, this is fine as there is at most one child having sliced output.
    for (const int childIndex : mChildren) {
        if (conditionCache[childIndex] == ConditionState::kNotEvaluated) {
            allConditions[childIndex]->isConditionMet(conditionParameters, allConditions,
                                                      isPartialLink,
                                                      conditionCache);
        }
    }
    conditionCache[mIndex] =
            evaluateCombinationCondition(mChildren, mLogicalOperation, conditionCache);
}

void CombinationConditionTracker::evaluateCondition(
        const LogEvent& event, const std::vector<MatchingState>& eventMatcherValues,
        const std::vector<sp<ConditionTracker>>& mAllConditions,
        std::vector<ConditionState>& nonSlicedConditionCache,
        std::vector<uint8_t>& conditionChangedCache) {
    // value is up to date.
    if (nonSlicedConditionCache[mIndex] != ConditionState::kNotEvaluated) {
        return;
    }

    for (const int childIndex : mChildren) {
        // So far, this is fine as there is at most one child having sliced output.
        if (nonSlicedConditionCache[childIndex] == ConditionState::kNotEvaluated) {
            const sp<ConditionTracker>& child = mAllConditions[childIndex];
            child->evaluateCondition(event, eventMatcherValues, mAllConditions,
                                     nonSlicedConditionCache, conditionChangedCache);
        }
    }

    ConditionState newCondition =
            evaluateCombinationCondition(mChildren, mLogicalOperation, nonSlicedConditionCache);
    if (!mSliced) {
        bool nonSlicedChanged = (mUnSlicedPartCondition != newCondition);
        mUnSlicedPartCondition = newCondition;

        nonSlicedConditionCache[mIndex] = mUnSlicedPartCondition;
        conditionChangedCache[mIndex] = nonSlicedChanged;
    } else {
        mUnSlicedPartCondition = evaluateCombinationCondition(mUnSlicedChildren, mLogicalOperation,
                                                              nonSlicedConditionCache);

        for (const int childIndex : mChildren) {
            // If any of the sliced condition in children condition changes, the combination
            // condition may be changed too.
            if (conditionChangedCache[childIndex]) {
                conditionChangedCache[mIndex] = true;
                break;
            }
        }
        nonSlicedConditionCache[mIndex] = newCondition;
        VLOG("CombinationPredicate %lld sliced may changed? %d", (long long)mConditionId,
            conditionChangedCache[mIndex] == true);
    }
}

bool CombinationConditionTracker::equalOutputDimensions(
        const std::vector<sp<ConditionTracker>>& allConditions,
        const vector<Matcher>& dimensions) const {
    if (mSlicedChildren.size() != 1 ||
        mSlicedChildren.front() >= (int)allConditions.size() ||
        mLogicalOperation != LogicalOperation::AND) {
        return false;
    }
    const sp<ConditionTracker>& slicedChild = allConditions.at(mSlicedChildren.front());
    return slicedChild->equalOutputDimensions(allConditions, dimensions);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
