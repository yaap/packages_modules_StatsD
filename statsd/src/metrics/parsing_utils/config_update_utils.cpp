/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "config_update_utils.h"

#include "external/StatsPullerManager.h"
#include "hash.h"
#include "matchers/EventMatcherWizard.h"

namespace android {
namespace os {
namespace statsd {

using google::protobuf::MessageLite;
using std::map;
using std::nullopt;
using std::optional;
using std::set;
using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

// Recursive function to determine if a matcher needs to be updated. Populates matcherUpdateStatus.
// Returns nullopt if successful and InvalidConfigReason if not.
optional<InvalidConfigReason> determineMatcherUpdateStatus(
        const StatsdConfig& config, const AtomMatcher& matcher,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const vector<sp<AtomMatchingTracker>>& oldAtomMatchingTrackers,
        const unordered_map<int64_t, AtomMatcherValue>& allAtomMatcherMap,
        unordered_map<int64_t, UpdateStatus>& matcherUpdateStatus,
        unordered_set<int64_t>& cycleTracker) {
    int64_t id = matcher.id();
    // Have already examined this matcher.
    if (matcherUpdateStatus.contains(id)) {
        return nullopt;
    }

    // Check if new matcher.
    const auto& oldAtomMatchingTrackerIt = oldAtomMatchingTrackerMap.find(id);
    if (oldAtomMatchingTrackerIt == oldAtomMatchingTrackerMap.end()) {
        matcherUpdateStatus[id] = UPDATE_NEW;
        return nullopt;
    }

    // This is an existing matcher. Check if it has changed.
    string serializedMatcher;
    if (!matcher.SerializeToString(&serializedMatcher)) {
        ALOGE("Unable to serialize matcher %lld", (long long)id);
        return createInvalidConfigReasonWithMatcher(
                INVALID_CONFIG_REASON_MATCHER_SERIALIZATION_FAILED, id);
    }
    uint64_t newProtoHash = Hash64(serializedMatcher);
    if (newProtoHash != oldAtomMatchingTrackers[oldAtomMatchingTrackerIt->second]->getProtoHash()) {
        matcherUpdateStatus[id] = UPDATE_REPLACE;
        return nullopt;
    }

    optional<InvalidConfigReason> invalidConfigReason;
    switch (matcher.contents_case()) {
        case AtomMatcher::ContentsCase::kSimpleAtomMatcher: {
            matcherUpdateStatus[id] = UPDATE_PRESERVE;
            return nullopt;
        }
        case AtomMatcher::ContentsCase::kCombination: {
            // Recurse to check if children have changed.
            cycleTracker.insert(id);
            UpdateStatus status = UPDATE_PRESERVE;
            for (const int64_t childMatcherId : matcher.combination().matcher()) {
                const auto& childIt = allAtomMatcherMap.find(childMatcherId);
                if (childIt == allAtomMatcherMap.end()) {
                    ALOGW("Matcher %lld not found in the config", (long long)childMatcherId);
                    invalidConfigReason = createInvalidConfigReasonWithMatcher(
                            INVALID_CONFIG_REASON_MATCHER_CHILD_NOT_FOUND, id);
                    invalidConfigReason->matcherIds.push_back(childMatcherId);
                    return invalidConfigReason;
                }
                const int64_t childId = childIt->first;
                if (cycleTracker.contains(childId)) {
                    ALOGE("Cycle detected in matcher config");
                    invalidConfigReason = createInvalidConfigReasonWithMatcher(
                            INVALID_CONFIG_REASON_MATCHER_CYCLE, id);
                    invalidConfigReason->matcherIds.push_back(childId);
                    return invalidConfigReason;
                }
                invalidConfigReason = determineMatcherUpdateStatus(
                        config, childIt->second.atomMatcher, oldAtomMatchingTrackerMap,
                        oldAtomMatchingTrackers, allAtomMatcherMap, matcherUpdateStatus,
                        cycleTracker);
                if (invalidConfigReason.has_value()) {
                    invalidConfigReason->matcherIds.push_back(id);
                    return invalidConfigReason;
                }

                if (matcherUpdateStatus[childId] == UPDATE_REPLACE) {
                    status = UPDATE_REPLACE;
                    break;
                }
            }
            matcherUpdateStatus[id] = status;
            cycleTracker.erase(id);
            return nullopt;
        }
        default: {
            ALOGE("Matcher \"%lld\" malformed", (long long)id);
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE, id);
        }
    }
    return nullopt;
}

bool updateAtomMatchingTrackers(
        const StatsdConfig& config, const sp<UidMap>& uidMap,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const vector<sp<AtomMatchingTracker>>& oldAtomMatchingTrackers,
        std::unordered_map<int, std::vector<int>>& allTagIdsToMatchersMap,
        unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        vector<sp<AtomMatchingTracker>>& newAtomMatchingTrackers, set<int64_t>& replacedMatchers,
        unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    bool allTrackersValid = true;
    unordered_map<int64_t, AtomMatcherValue> allAtomMatcherMap;
    optional<InvalidConfigReason> invalidConfigReason;
    newAtomMatchingTrackers.reserve(
            config.atom_matcher_size());  // Always reserve the max since errors are rare.

    // Maps matcher id to their position in the config. For fast lookup of dependencies.
    for (int i = 0; i < config.atom_matcher_size(); i++) {
        const AtomMatcher& matcher = config.atom_matcher(i);
        if (allAtomMatcherMap.find(matcher.id()) != allAtomMatcherMap.end()) {
            allTrackersValid = false;
            ALOGE("Duplicate atom matcher found for id %lld", (long long)matcher.id());
            invalidEntities[{matcher.id(), INVALID_ENTITY_TYPE_MATCHER}] =
                    createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_DUPLICATE,
                                                         matcher.id());
        }
        allAtomMatcherMap[matcher.id()] = {matcher, nullptr};
    }

    // For combination matchers, we need to determine if any children need to be updated.
    unordered_map<int64_t, UpdateStatus> matcherUpdateStatus;
    unordered_set<int64_t> cycleTracker;
    for (const auto& [id, atomMatcherValue] : allAtomMatcherMap) {
        cycleTracker.clear();
        invalidConfigReason = determineMatcherUpdateStatus(
                config, atomMatcherValue.atomMatcher, oldAtomMatchingTrackerMap,
                oldAtomMatchingTrackers, allAtomMatcherMap, matcherUpdateStatus, cycleTracker);
        if (invalidConfigReason.has_value()) {
            invalidEntities[{id, INVALID_ENTITY_TYPE_MATCHER}] = invalidConfigReason.value();
        }
    }

    for (auto& [id, atomMatcherValue] : allAtomMatcherMap) {
        switch (matcherUpdateStatus[id]) {
            case UPDATE_PRESERVE: {
                const auto& oldAtomMatchingTrackerIt = oldAtomMatchingTrackerMap.find(id);
                if (oldAtomMatchingTrackerIt == oldAtomMatchingTrackerMap.end()) {
                    ALOGE("Could not find AtomMatcher %lld in the previous config, but expected it "
                          "to be there",
                          (long long)id);
                    invalidEntities[{id, INVALID_ENTITY_TYPE_MATCHER}] =
                            createInvalidConfigReasonWithMatcher(
                                    INVALID_CONFIG_REASON_MATCHER_NOT_IN_PREV_CONFIG, id);
                    break;
                }
                const sp<AtomMatchingTracker>& tracker =
                        oldAtomMatchingTrackers[oldAtomMatchingTrackerIt->second];
                atomMatcherValue.atomMatchingTracker = tracker;
                break;
            }
            case UPDATE_REPLACE:
                replacedMatchers.insert(id);
                [[fallthrough]];  // Intentionally fallthrough to create the new matcher.
            case UPDATE_NEW: {
                sp<AtomMatchingTracker> tracker = createAtomMatchingTracker(
                        atomMatcherValue.atomMatcher, uidMap, invalidConfigReason);
                if (tracker == nullptr) {
                    allTrackersValid = false;
                    if (invalidConfigReason.has_value()) {
                        invalidEntities[{id, INVALID_ENTITY_TYPE_MATCHER}] =
                                invalidConfigReason.value();
                    } else {
                        // Should never happen
                        invalidEntities[{id, INVALID_ENTITY_TYPE_MATCHER}] =
                                createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_UNKNOWN,
                                                                     id);
                    }
                }
                atomMatcherValue.atomMatchingTracker = tracker;
                break;
            }
            default: {
                if (invalidEntities.contains({id, INVALID_ENTITY_TYPE_MATCHER})) {
                    // Invalid matchers may not have an update state set.
                    break;
                }
                allTrackersValid = false;
                ALOGE("Matcher \"%lld\" update state is unknown. This should never happen",
                      (long long)id);
                invalidEntities[{id, INVALID_ENTITY_TYPE_MATCHER}] =
                        createInvalidConfigReasonWithMatcher(
                                INVALID_CONFIG_REASON_MATCHER_UPDATE_STATUS_UNKNOWN, id);
            }
        }
    }

    for (const auto& [id, atomMatcherValue] : allAtomMatcherMap) {
        if (atomMatcherValue.atomMatchingTracker == nullptr) {
            continue;
        }
        cycleTracker.clear();
        const auto [invalidReason, _] = atomMatcherValue.atomMatchingTracker->isTrackerValid(
                allAtomMatcherMap, cycleTracker);
        if (invalidReason.has_value()) {
            allTrackersValid = false;
            invalidEntities[{atomMatcherValue.atomMatchingTracker->getId(),
                             INVALID_ENTITY_TYPE_MATCHER}] = invalidReason.value();
        }
    }

    // Iterate through the matchers from the original config to guarantee consistent order.
    int outIndex = 0;
    for (const auto& matcher : config.atom_matcher()) {
        if (invalidEntities.find({matcher.id(), INVALID_ENTITY_TYPE_MATCHER}) ==
            invalidEntities.end()) {
            newAtomMatchingTrackerMap[matcher.id()] = outIndex;
            newAtomMatchingTrackers.push_back(allAtomMatcherMap[matcher.id()].atomMatchingTracker);
            ++outIndex;
        }
    }

    for (size_t matcherIndex = 0; matcherIndex < newAtomMatchingTrackers.size(); matcherIndex++) {
        auto& matcher = newAtomMatchingTrackers[matcherIndex];
        if (matcherUpdateStatus[matcher->getId()] == UPDATE_PRESERVE) {
            matcher->onConfigUpdated(allAtomMatcherMap[matcher->getId()].atomMatcher,
                                     newAtomMatchingTrackerMap);
        } else {
            matcher->init(allAtomMatcherMap, newAtomMatchingTrackerMap);
        }

        // Collect all the tag ids that are interesting. TagIds exist in leaf nodes only.
        const set<int>& tagIds = matcher->getAtomIds();
        for (int atomId : tagIds) {
            auto& matchers = allTagIdsToMatchersMap[atomId];
            // Performance note:
            // For small amount of elements linear search in vector will be
            // faster then look up in a set:
            // - we do not expect matchers vector per atom id will have significant size (< 10)
            // - iteration via vector is the fastest way compared to other containers (set, etc.)
            //   in the hot path MetricsManager::onLogEvent()
            // - vector<T> will have the smallest memory footprint compared to any other
            //   std containers implementation
            if (find(matchers.begin(), matchers.end(), matcherIndex) == matchers.end()) {
                matchers.push_back(matcherIndex);
            }
        }
    }

    return allTrackersValid;
}

// Recursive function to determine if a condition needs to be updated. Populates
// conditionUpdateStatus. Returns nullopt if successful and InvalidConfigReason if not.
optional<InvalidConfigReason> determineConditionUpdateStatus(
        const StatsdConfig& config, const Predicate& predicate,
        const unordered_map<int64_t, int>& oldConditionTrackerMap,
        const vector<sp<ConditionTracker>>& oldConditionTrackers,
        const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        const set<int64_t>& replacedMatchers,
        unordered_map<int64_t, UpdateStatus>& conditionUpdateStatus,
        unordered_set<int64_t>& cycleTracker) {
    int64_t id = predicate.id();
    // Have already examined this condition.
    if (conditionUpdateStatus.contains(id)) {
        return nullopt;
    }

    // Check if new condition.
    const auto& oldConditionTrackerIt = oldConditionTrackerMap.find(id);
    if (oldConditionTrackerIt == oldConditionTrackerMap.end()) {
        conditionUpdateStatus[id] = UPDATE_NEW;
        return nullopt;
    }

    // This is an existing condition. Check if it has changed.
    string serializedCondition;
    if (!predicate.SerializeToString(&serializedCondition)) {
        ALOGE("Unable to serialize predicate %lld", (long long)id);
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_CONDITION_SERIALIZATION_FAILED, id);
    }
    uint64_t newProtoHash = Hash64(serializedCondition);
    if (newProtoHash != oldConditionTrackers[oldConditionTrackerIt->second]->getProtoHash()) {
        conditionUpdateStatus[id] = UPDATE_REPLACE;
        return nullopt;
    }

    optional<InvalidConfigReason> invalidConfigReason;
    switch (predicate.contents_case()) {
        case Predicate::ContentsCase::kSimplePredicate: {
            // Need to check if any of the underlying matchers changed.
            const SimplePredicate& simplePredicate = predicate.simple_predicate();
            if (simplePredicate.has_start()) {
                if (replacedMatchers.find(simplePredicate.start()) != replacedMatchers.end()) {
                    conditionUpdateStatus[id] = UPDATE_REPLACE;
                    return nullopt;
                }
            }
            if (simplePredicate.has_stop()) {
                if (replacedMatchers.find(simplePredicate.stop()) != replacedMatchers.end()) {
                    conditionUpdateStatus[id] = UPDATE_REPLACE;
                    return nullopt;
                }
            }
            if (simplePredicate.has_stop_all()) {
                if (replacedMatchers.find(simplePredicate.stop_all()) != replacedMatchers.end()) {
                    conditionUpdateStatus[id] = UPDATE_REPLACE;
                    return nullopt;
                }
            }
            conditionUpdateStatus[id] = UPDATE_PRESERVE;
            return nullopt;
        }
        case Predicate::ContentsCase::kCombination: {
            // Need to recurse on the children to see if any of the child predicates changed.
            cycleTracker.insert(id);
            UpdateStatus status = UPDATE_PRESERVE;
            for (const int64_t childPredicateId : predicate.combination().predicate()) {
                const auto& childIt = allConditionsMap.find(childPredicateId);
                if (childIt == allConditionsMap.end()) {
                    ALOGW("Predicate %lld not found in the config", (long long)childPredicateId);
                    invalidConfigReason = createInvalidConfigReasonWithPredicate(
                            INVALID_CONFIG_REASON_CONDITION_CHILD_NOT_FOUND, id);
                    invalidConfigReason->conditionIds.push_back(childPredicateId);
                    return invalidConfigReason;
                }
                const int64_t childId = childIt->first;
                if (cycleTracker.contains(childId)) {
                    ALOGE("Cycle detected in predicate config");
                    invalidConfigReason = createInvalidConfigReasonWithPredicate(
                            INVALID_CONFIG_REASON_CONDITION_CYCLE, id);
                    invalidConfigReason->conditionIds.push_back(childPredicateId);
                    return invalidConfigReason;
                }
                invalidConfigReason = determineConditionUpdateStatus(
                        config, childIt->second.predicate, oldConditionTrackerMap,
                        oldConditionTrackers, allConditionsMap, replacedMatchers,
                        conditionUpdateStatus, cycleTracker);
                if (invalidConfigReason.has_value()) {
                    invalidConfigReason->conditionIds.push_back(id);
                    return invalidConfigReason;
                }

                if (conditionUpdateStatus[childId] == UPDATE_REPLACE) {
                    status = UPDATE_REPLACE;
                    break;
                }
            }
            conditionUpdateStatus[id] = status;
            cycleTracker.erase(id);
            return nullopt;
        }
        default: {
            ALOGE("Predicate \"%lld\" malformed", (long long)id);
            return createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_CONDITION_MALFORMED_CONTENTS_CASE, id);
        }
    }

    return nullopt;
}

bool updateConditions(const ConfigKey& key, const StatsdConfig& config,
                      const unordered_map<int64_t, int>& atomMatchingTrackerMap,
                      const set<int64_t>& replacedMatchers,
                      const unordered_map<int64_t, int>& oldConditionTrackerMap,
                      const vector<sp<ConditionTracker>>& oldConditionTrackers,
                      unordered_map<int64_t, int>& newConditionTrackerMap,
                      vector<sp<ConditionTracker>>& newConditionTrackers,
                      unordered_map<int, vector<int>>& trackerToConditionMap,
                      vector<ConditionState>& conditionCache, set<int64_t>& replacedConditions,
                      unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    bool allConditionsValid = true;
    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    optional<InvalidConfigReason> invalidConfigReason;

    for (int i = 0; i < config.predicate_size(); i++) {
        const Predicate& condition = config.predicate(i);
        if (allConditionsMap.find(condition.id()) != allConditionsMap.end()) {
            ALOGE("Duplicate Predicate found!");
            allConditionsValid = false;
            invalidEntities[{condition.id(), INVALID_ENTITY_TYPE_PREDICATE}] =
                    createInvalidConfigReasonWithPredicate(
                            INVALID_CONFIG_REASON_CONDITION_DUPLICATE, condition.id());
        }
        allConditionsMap[condition.id()] = {condition, nullptr};
    }

    unordered_map<int64_t, UpdateStatus> conditionUpdateStatus;
    unordered_set<int64_t> cycleTracker;
    for (const auto& [id, conditionValue] : allConditionsMap) {
        cycleTracker.clear();
        invalidConfigReason = determineConditionUpdateStatus(
                config, conditionValue.predicate, oldConditionTrackerMap, oldConditionTrackers,
                allConditionsMap, replacedMatchers, conditionUpdateStatus, cycleTracker);
        if (invalidConfigReason.has_value()) {
            allConditionsValid = false;
            invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] = invalidConfigReason.value();
        }
    }

    // Update status has been determined for all conditions. Now perform the update.
    unordered_set<int64_t> preservedConditions;
    for (auto& [id, conditionValue] : allConditionsMap) {
        const Predicate& predicate = conditionValue.predicate;
        switch (conditionUpdateStatus[id]) {
            case UPDATE_PRESERVE: {
                preservedConditions.insert(id);
                const auto& oldConditionTrackerIt = oldConditionTrackerMap.find(id);
                if (oldConditionTrackerIt == oldConditionTrackerMap.end()) {
                    ALOGE("Could not find Predicate %lld in the previous config, but expected it "
                          "to be there",
                          (long long)id);
                    allConditionsValid = false;
                    invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] =
                            createInvalidConfigReasonWithPredicate(
                                    INVALID_CONFIG_REASON_CONDITION_NOT_IN_PREV_CONFIG, id);
                }
                const sp<ConditionTracker>& tracker =
                        oldConditionTrackers[oldConditionTrackerIt->second];
                conditionValue.conditionTracker = tracker;
                break;
            }
            case UPDATE_REPLACE:
                replacedConditions.insert(id);
                [[fallthrough]];  // Intentionally fallthrough to create the new condition tracker.
            case UPDATE_NEW: {
                sp<ConditionTracker> tracker = createConditionTracker(
                        key, predicate, invalidEntities, invalidConfigReason);
                if (tracker == nullptr) {
                    allConditionsValid = false;
                    if (invalidConfigReason.has_value()) {
                        invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] =
                                invalidConfigReason.value();
                    } else {
                        // Should never happen
                        invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] =
                                createInvalidConfigReasonWithPredicate(
                                        INVALID_CONFIG_REASON_UNKNOWN, id);
                    }
                }
                conditionValue.conditionTracker = tracker;
                break;
            }
            default: {
                if (invalidEntities.contains({id, INVALID_ENTITY_TYPE_PREDICATE})) {
                    // Invalid conditions may not have an update state set.
                    break;
                }
                ALOGE("Condition \"%lld\" update state is unknown. This should never happen",
                      (long long)id);
                allConditionsValid = false;
                invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] =
                        createInvalidConfigReasonWithPredicate(
                                INVALID_CONFIG_REASON_CONDITION_UPDATE_STATUS_UNKNOWN, id);
            }
        }
    }

    for (const auto& [id, conditionValue] : allConditionsMap) {
        if (conditionValue.conditionTracker == nullptr) {
            continue;
        }
        cycleTracker.clear();
        const auto& invalidReason =
                conditionValue.conditionTracker->isTrackerValid(allConditionsMap, cycleTracker);
        if (invalidReason.has_value()) {
            allConditionsValid = false;
            invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] = invalidReason.value();
        }
    }

    // Reserve the maximum amount since invalid entities are rare
    newConditionTrackers.reserve(config.predicate_size());
    conditionCache.reserve(config.predicate_size());
    int outIndex = 0;
    for (const auto& predicate : config.predicate()) {
        if (!invalidEntities.contains({predicate.id(), INVALID_ENTITY_TYPE_PREDICATE})) {
            newConditionTrackerMap[predicate.id()] = outIndex;
            newConditionTrackers.push_back(allConditionsMap.at(predicate.id()).conditionTracker);
            conditionCache.push_back(ConditionState::kNotEvaluated);
            ++outIndex;
        }
    }

    unordered_set<int64_t> initializedConditions;
    for (const int64_t conditionId : preservedConditions) {
        if (!invalidEntities.contains({conditionId, INVALID_ENTITY_TYPE_PREDICATE})) {
            const auto& conditionValue = allConditionsMap.find(conditionId)->second;
            const int conditionIndex = newConditionTrackerMap[conditionId];
            conditionValue.conditionTracker->onConfigUpdated(
                    allConditionsMap, conditionIndex, newConditionTrackers, atomMatchingTrackerMap,
                    newConditionTrackerMap);
            // Preserved conditions are added to this set to avoid reinitializing them in the
            // subsequent loop.
            initializedConditions.insert(conditionId);
        }
    }

    for (size_t conditionIndex = 0; conditionIndex < newConditionTrackers.size();
         ++conditionIndex) {
        // Calling init on preserved conditions is OK. It is needed to fill the condition cache.
        auto& conditionTracker = newConditionTrackers[conditionIndex];
        conditionTracker->init(conditionIndex, allConditionsMap, newConditionTrackers,
                               newConditionTrackerMap, atomMatchingTrackerMap,
                               initializedConditions, conditionCache);
        for (const int trackerIndex : conditionTracker->getAtomMatchingTrackerIndex()) {
            vector<int>& conditionList = trackerToConditionMap[trackerIndex];
            conditionList.push_back(conditionIndex);
        }
    }

    return allConditionsValid;
}

bool updateStates(const StatsdConfig& config, const map<int64_t, uint64_t>& oldStateProtoHashes,
                  unordered_map<int64_t, int>& stateAtomIdMap,
                  unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
                  map<int64_t, uint64_t>& newStateProtoHashes, set<int64_t>& replacedStates,
                  unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    // Share with metrics_manager_util.
    bool allStatesValid = initStates(config, stateAtomIdMap, allStateGroupMaps, newStateProtoHashes,
                                     invalidEntities);

    for (const auto& [stateId, stateHash] : oldStateProtoHashes) {
        const auto& it = newStateProtoHashes.find(stateId);
        if (it != newStateProtoHashes.end() && it->second != stateHash) {
            replacedStates.insert(stateId);
        }
    }
    return allStatesValid;
}
// Returns true if any matchers in the metric activation were replaced.
bool metricActivationDepsChange(const StatsdConfig& config,
                                const unordered_map<int64_t, int>& metricToActivationMap,
                                const int64_t metricId, const set<int64_t>& replacedMatchers) {
    const auto& metricActivationIt = metricToActivationMap.find(metricId);
    if (metricActivationIt == metricToActivationMap.end()) {
        return false;
    }
    const MetricActivation& metricActivation = config.metric_activation(metricActivationIt->second);
    for (int i = 0; i < metricActivation.event_activation_size(); i++) {
        const EventActivation& activation = metricActivation.event_activation(i);
        if (replacedMatchers.find(activation.atom_matcher_id()) != replacedMatchers.end()) {
            return true;
        }
        if (activation.has_deactivation_atom_matcher_id()) {
            if (replacedMatchers.find(activation.deactivation_atom_matcher_id()) !=
                replacedMatchers.end()) {
                return true;
            }
        }
    }
    return false;
}

optional<InvalidConfigReason> determineMetricUpdateStatus(
        const StatsdConfig& config, const MessageLite& metric, const int64_t metricId,
        const MetricType metricType, const set<int64_t>& matcherDependencies,
        const set<int64_t>& conditionDependencies,
        const ::google::protobuf::RepeatedField<int64_t>& stateDependencies,
        const ::google::protobuf::RepeatedPtrField<MetricConditionLink>& conditionLinks,
        const unordered_map<int64_t, int>& oldMetricProducerMap,
        const vector<sp<MetricProducer>>& oldMetricProducers,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int64_t>& replacedMatchers, const set<int64_t>& replacedConditions,
        const set<int64_t>& replacedStates, UpdateStatus& updateStatus) {
    // Check if new metric
    const auto& oldMetricProducerIt = oldMetricProducerMap.find(metricId);
    if (oldMetricProducerIt == oldMetricProducerMap.end()) {
        updateStatus = UPDATE_NEW;
        return nullopt;
    }

    // This is an existing metric, check if it has changed.
    uint64_t metricHash;
    optional<InvalidConfigReason> invalidConfigReason =
            getMetricProtoHash(config, metric, metricId, metricToActivationMap, metricHash);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }
    const sp<MetricProducer>& oldMetricProducer = oldMetricProducers[oldMetricProducerIt->second];
    if (oldMetricProducer->getMetricType() != metricType ||
        oldMetricProducer->getProtoHash() != metricHash) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }

    // Take intersections of the matchers/predicates/states that the metric
    // depends on with those that have been replaced. If a metric depends on any
    // replaced component, it too must be replaced.
    set<int64_t> intersection;
    set_intersection(matcherDependencies.begin(), matcherDependencies.end(),
                     replacedMatchers.begin(), replacedMatchers.end(),
                     inserter(intersection, intersection.begin()));
    if (intersection.size() > 0) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }
    set_intersection(conditionDependencies.begin(), conditionDependencies.end(),
                     replacedConditions.begin(), replacedConditions.end(),
                     inserter(intersection, intersection.begin()));
    if (intersection.size() > 0) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }
    set_intersection(stateDependencies.begin(), stateDependencies.end(), replacedStates.begin(),
                     replacedStates.end(), inserter(intersection, intersection.begin()));
    if (intersection.size() > 0) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }

    for (const auto& metricConditionLink : conditionLinks) {
        if (replacedConditions.find(metricConditionLink.condition()) != replacedConditions.end()) {
            updateStatus = UPDATE_REPLACE;
            return nullopt;
        }
    }

    if (metricActivationDepsChange(config, metricToActivationMap, metricId, replacedMatchers)) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }

    updateStatus = UPDATE_PRESERVE;
    return nullopt;
}

optional<InvalidConfigReason> determineAllMetricUpdateStatuses(
        const StatsdConfig& config, const unordered_map<int64_t, int>& oldMetricProducerMap,
        const vector<sp<MetricProducer>>& oldMetricProducers,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int64_t>& replacedMatchers, const set<int64_t>& replacedConditions,
        const set<int64_t>& replacedStates, vector<UpdateStatus>& metricUpdateStatus) {
    int metricIndex = 0;
    optional<InvalidConfigReason> invalidConfigReason;
    for (int i = 0; i < config.count_metric_size(); i++, metricIndex++) {
        const CountMetric& metric = config.count_metric(i);
        set<int64_t> conditionDependencies;
        if (metric.has_condition()) {
            conditionDependencies.insert(metric.condition());
        }
        invalidConfigReason = determineMetricUpdateStatus(
                config, metric, metric.id(), METRIC_TYPE_COUNT, {metric.what()},
                conditionDependencies, metric.slice_by_state(), metric.links(),
                oldMetricProducerMap, oldMetricProducers, metricToActivationMap, replacedMatchers,
                replacedConditions, replacedStates, metricUpdateStatus[metricIndex]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }
    for (int i = 0; i < config.duration_metric_size(); i++, metricIndex++) {
        const DurationMetric& metric = config.duration_metric(i);
        set<int64_t> conditionDependencies({metric.what()});
        if (metric.has_condition()) {
            conditionDependencies.insert(metric.condition());
        }
        invalidConfigReason = determineMetricUpdateStatus(
                config, metric, metric.id(), METRIC_TYPE_DURATION, /*matcherDependencies=*/{},
                conditionDependencies, metric.slice_by_state(), metric.links(),
                oldMetricProducerMap, oldMetricProducers, metricToActivationMap, replacedMatchers,
                replacedConditions, replacedStates, metricUpdateStatus[metricIndex]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }
    for (int i = 0; i < config.event_metric_size(); i++, metricIndex++) {
        const EventMetric& metric = config.event_metric(i);
        set<int64_t> conditionDependencies;
        if (metric.has_condition()) {
            conditionDependencies.insert(metric.condition());
        }
        invalidConfigReason = determineMetricUpdateStatus(
                config, metric, metric.id(), METRIC_TYPE_EVENT, {metric.what()},
                conditionDependencies, ::google::protobuf::RepeatedField<int64_t>(), metric.links(),
                oldMetricProducerMap, oldMetricProducers, metricToActivationMap, replacedMatchers,
                replacedConditions, replacedStates, metricUpdateStatus[metricIndex]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }
    for (int i = 0; i < config.value_metric_size(); i++, metricIndex++) {
        const ValueMetric& metric = config.value_metric(i);
        set<int64_t> conditionDependencies;
        if (metric.has_condition()) {
            conditionDependencies.insert(metric.condition());
        }
        invalidConfigReason = determineMetricUpdateStatus(
                config, metric, metric.id(), METRIC_TYPE_VALUE, {metric.what()},
                conditionDependencies, metric.slice_by_state(), metric.links(),
                oldMetricProducerMap, oldMetricProducers, metricToActivationMap, replacedMatchers,
                replacedConditions, replacedStates, metricUpdateStatus[metricIndex]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }
    for (int i = 0; i < config.gauge_metric_size(); i++, metricIndex++) {
        const GaugeMetric& metric = config.gauge_metric(i);
        set<int64_t> conditionDependencies;
        if (metric.has_condition()) {
            conditionDependencies.insert(metric.condition());
        }
        set<int64_t> matcherDependencies({metric.what()});
        if (metric.has_trigger_event()) {
            matcherDependencies.insert(metric.trigger_event());
        }
        invalidConfigReason = determineMetricUpdateStatus(
                config, metric, metric.id(), METRIC_TYPE_GAUGE, matcherDependencies,
                conditionDependencies, ::google::protobuf::RepeatedField<int64_t>(), metric.links(),
                oldMetricProducerMap, oldMetricProducers, metricToActivationMap, replacedMatchers,
                replacedConditions, replacedStates, metricUpdateStatus[metricIndex]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    for (int i = 0; i < config.kll_metric_size(); i++, metricIndex++) {
        const KllMetric& metric = config.kll_metric(i);
        set<int64_t> conditionDependencies;
        if (metric.has_condition()) {
            conditionDependencies.insert(metric.condition());
        }
        invalidConfigReason = determineMetricUpdateStatus(
                config, metric, metric.id(), METRIC_TYPE_KLL, {metric.what()},
                conditionDependencies, metric.slice_by_state(), metric.links(),
                oldMetricProducerMap, oldMetricProducers, metricToActivationMap, replacedMatchers,
                replacedConditions, replacedStates, metricUpdateStatus[metricIndex]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return nullopt;
}

// Called when a metric is preserved during a config update. Finds the metric in oldMetricProducers
// and calls onConfigUpdated to update all indices.
optional<sp<MetricProducer>> updateMetric(
        const StatsdConfig& config, const int configIndex, const int metricIndex,
        const int64_t metricId, const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const sp<EventMatcherWizard>& matcherWizard,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& conditionTrackerMap, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& oldMetricProducerMap,
        const vector<sp<MetricProducer>>& oldMetricProducers,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation, optional<InvalidConfigReason>& invalidConfigReason) {
    const auto& oldMetricProducerIt = oldMetricProducerMap.find(metricId);
    if (oldMetricProducerIt == oldMetricProducerMap.end()) {
        ALOGE("Could not find Metric %lld in the previous config, but expected it "
              "to be there",
              (long long)metricId);
        invalidConfigReason =
                InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_NOT_IN_PREV_CONFIG, metricId);
        return nullopt;
    }
    const int oldIndex = oldMetricProducerIt->second;
    sp<MetricProducer> producer = oldMetricProducers[oldIndex];
    invalidConfigReason = producer->onConfigUpdated(
            config, configIndex, metricIndex, allAtomMatchingTrackers, oldAtomMatchingTrackerMap,
            newAtomMatchingTrackerMap, matcherWizard, allConditionTrackers, conditionTrackerMap,
            wizard, metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
            activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
            metricsWithActivation);
    if (invalidConfigReason.has_value()) {
        return nullopt;
    }
    return {producer};
}

optional<InvalidConfigReason> updateMetrics(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const set<int64_t>& replacedMatchers,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const set<int64_t>& replacedConditions, vector<sp<ConditionTracker>>& allConditionTrackers,
        const vector<ConditionState>& initialConditionCache,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
        const set<int64_t>& replacedStates, const unordered_map<int64_t, int>& oldMetricProducerMap,
        const vector<sp<MetricProducer>>& oldMetricProducers,
        const wp<ConfigMetadataProvider> configMetadataProvider,
        unordered_map<int64_t, int>& newMetricProducerMap,
        vector<sp<MetricProducer>>& newMetricProducers,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& trackerToMetricMap, set<int64_t>& noReportMetricIds,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation, set<int64_t>& replacedMetrics) {
    sp<ConditionWizard> wizard = new ConditionWizard(allConditionTrackers);
    sp<EventMatcherWizard> matcherWizard = new EventMatcherWizard(allAtomMatchingTrackers);
    const int allMetricsCount = config.count_metric_size() + config.duration_metric_size() +
                                config.event_metric_size() + config.gauge_metric_size() +
                                config.value_metric_size() + config.kll_metric_size();
    newMetricProducers.reserve(allMetricsCount);
    optional<InvalidConfigReason> invalidConfigReason;

    if (config.has_restricted_metrics_delegate_package_name() &&
        allMetricsCount != config.event_metric_size()) {
        ALOGE("Restricted metrics only support event metric");
        return InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED);
    }

    // Construct map from metric id to metric activation index. The map will be used to determine
    // the metric activation corresponding to a metric.
    unordered_map<int64_t, int> metricToActivationMap;
    for (int i = 0; i < config.metric_activation_size(); i++) {
        const MetricActivation& metricActivation = config.metric_activation(i);
        int64_t metricId = metricActivation.metric_id();
        if (metricToActivationMap.find(metricId) != metricToActivationMap.end()) {
            ALOGE("Metric %lld has multiple MetricActivations", (long long)metricId);
            return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_HAS_MULTIPLE_ACTIVATIONS,
                                       metricId);
        }
        metricToActivationMap.insert({metricId, i});
    }

    vector<UpdateStatus> metricUpdateStatus(allMetricsCount, UPDATE_UNKNOWN);
    invalidConfigReason = determineAllMetricUpdateStatuses(
            config, oldMetricProducerMap, oldMetricProducers, metricToActivationMap,
            replacedMatchers, replacedConditions, replacedStates, metricUpdateStatus);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    // Now, perform the update. Must iterate the metric types in the same order
    int metricIndex = 0;
    for (int i = 0; i < config.count_metric_size(); i++, metricIndex++) {
        const CountMetric& metric = config.count_metric(i);
        newMetricProducerMap[metric.id()] = metricIndex;
        optional<sp<MetricProducer>> producer;
        switch (metricUpdateStatus[metricIndex]) {
            case UPDATE_PRESERVE: {
                producer = updateMetric(
                        config, i, metricIndex, metric.id(), allAtomMatchingTrackers,
                        oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                        allConditionTrackers, conditionTrackerMap, wizard, oldMetricProducerMap,
                        oldMetricProducers, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason);
                break;
            }
            case UPDATE_REPLACE:
                replacedMetrics.insert(metric.id());
                [[fallthrough]];  // Intentionally fallthrough to create the new metric producer.
            case UPDATE_NEW: {
                producer = createCountMetricProducerAndUpdateMetadata(
                        key, config, timeBaseNs, currentTimeNs, metric, metricIndex,
                        allAtomMatchingTrackers, newAtomMatchingTrackerMap, allConditionTrackers,
                        conditionTrackerMap, initialConditionCache, wizard, stateAtomIdMap,
                        allStateGroupMaps, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason, configMetadataProvider);
                break;
            }
            default: {
                ALOGE("Metric \"%lld\" update state is unknown. This should never happen",
                      (long long)metric.id());
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_UPDATE_STATUS_UNKNOWN,
                                           metric.id());
            }
        }
        if (!producer) {
            return invalidConfigReason;
        }
        newMetricProducers.push_back(producer.value());
    }
    for (int i = 0; i < config.duration_metric_size(); i++, metricIndex++) {
        const DurationMetric& metric = config.duration_metric(i);
        newMetricProducerMap[metric.id()] = metricIndex;
        optional<sp<MetricProducer>> producer;
        switch (metricUpdateStatus[metricIndex]) {
            case UPDATE_PRESERVE: {
                producer = updateMetric(
                        config, i, metricIndex, metric.id(), allAtomMatchingTrackers,
                        oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                        allConditionTrackers, conditionTrackerMap, wizard, oldMetricProducerMap,
                        oldMetricProducers, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason);
                break;
            }
            case UPDATE_REPLACE:
                replacedMetrics.insert(metric.id());
                [[fallthrough]];  // Intentionally fallthrough to create the new metric producer.
            case UPDATE_NEW: {
                producer = createDurationMetricProducerAndUpdateMetadata(
                        key, config, timeBaseNs, currentTimeNs, metric, metricIndex,
                        allAtomMatchingTrackers, newAtomMatchingTrackerMap, allConditionTrackers,
                        conditionTrackerMap, initialConditionCache, wizard, stateAtomIdMap,
                        allStateGroupMaps, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason, configMetadataProvider);
                break;
            }
            default: {
                ALOGE("Metric \"%lld\" update state is unknown. This should never happen",
                      (long long)metric.id());
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_UPDATE_STATUS_UNKNOWN,
                                           metric.id());
            }
        }
        if (!producer) {
            return invalidConfigReason;
        }
        newMetricProducers.push_back(producer.value());
    }
    for (int i = 0; i < config.event_metric_size(); i++, metricIndex++) {
        const EventMetric& metric = config.event_metric(i);
        newMetricProducerMap[metric.id()] = metricIndex;
        optional<sp<MetricProducer>> producer;
        switch (metricUpdateStatus[metricIndex]) {
            case UPDATE_PRESERVE: {
                producer = updateMetric(
                        config, i, metricIndex, metric.id(), allAtomMatchingTrackers,
                        oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                        allConditionTrackers, conditionTrackerMap, wizard, oldMetricProducerMap,
                        oldMetricProducers, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason);
                break;
            }
            case UPDATE_REPLACE:
                replacedMetrics.insert(metric.id());
                [[fallthrough]];  // Intentionally fallthrough to create the new metric producer.
            case UPDATE_NEW: {
                producer = createEventMetricProducerAndUpdateMetadata(
                        key, config, timeBaseNs, metric, metricIndex, allAtomMatchingTrackers,
                        newAtomMatchingTrackerMap, allConditionTrackers, conditionTrackerMap,
                        initialConditionCache, wizard, stateAtomIdMap, allStateGroupMaps,
                        metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
                        activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                        metricsWithActivation, invalidConfigReason, configMetadataProvider);
                break;
            }
            default: {
                ALOGE("Metric \"%lld\" update state is unknown. This should never happen",
                      (long long)metric.id());
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_UPDATE_STATUS_UNKNOWN,
                                           metric.id());
            }
        }
        if (!producer) {
            return invalidConfigReason;
        }
        newMetricProducers.push_back(producer.value());
    }

    for (int i = 0; i < config.value_metric_size(); i++, metricIndex++) {
        const ValueMetric& metric = config.value_metric(i);
        newMetricProducerMap[metric.id()] = metricIndex;
        optional<sp<MetricProducer>> producer;
        switch (metricUpdateStatus[metricIndex]) {
            case UPDATE_PRESERVE: {
                producer = updateMetric(
                        config, i, metricIndex, metric.id(), allAtomMatchingTrackers,
                        oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                        allConditionTrackers, conditionTrackerMap, wizard, oldMetricProducerMap,
                        oldMetricProducers, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason);
                break;
            }
            case UPDATE_REPLACE:
                replacedMetrics.insert(metric.id());
                [[fallthrough]];  // Intentionally fallthrough to create the new metric producer.
            case UPDATE_NEW: {
                producer = createNumericValueMetricProducerAndUpdateMetadata(
                        key, config, timeBaseNs, currentTimeNs, pullerManager, metric, metricIndex,
                        allAtomMatchingTrackers, newAtomMatchingTrackerMap, allConditionTrackers,
                        conditionTrackerMap, initialConditionCache, wizard, matcherWizard,
                        stateAtomIdMap, allStateGroupMaps, metricToActivationMap,
                        trackerToMetricMap, conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason, configMetadataProvider);
                break;
            }
            default: {
                ALOGE("Metric \"%lld\" update state is unknown. This should never happen",
                      (long long)metric.id());
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_UPDATE_STATUS_UNKNOWN,
                                           metric.id());
            }
        }
        if (!producer) {
            return invalidConfigReason;
        }
        newMetricProducers.push_back(producer.value());
    }

    for (int i = 0; i < config.gauge_metric_size(); i++, metricIndex++) {
        const GaugeMetric& metric = config.gauge_metric(i);
        newMetricProducerMap[metric.id()] = metricIndex;
        optional<sp<MetricProducer>> producer;
        switch (metricUpdateStatus[metricIndex]) {
            case UPDATE_PRESERVE: {
                producer = updateMetric(
                        config, i, metricIndex, metric.id(), allAtomMatchingTrackers,
                        oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                        allConditionTrackers, conditionTrackerMap, wizard, oldMetricProducerMap,
                        oldMetricProducers, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason);
                break;
            }
            case UPDATE_REPLACE:
                replacedMetrics.insert(metric.id());
                [[fallthrough]];  // Intentionally fallthrough to create the new metric producer.
            case UPDATE_NEW: {
                producer = createGaugeMetricProducerAndUpdateMetadata(
                        key, config, timeBaseNs, currentTimeNs, pullerManager, metric, metricIndex,
                        allAtomMatchingTrackers, newAtomMatchingTrackerMap, allConditionTrackers,
                        conditionTrackerMap, initialConditionCache, wizard, matcherWizard,
                        stateAtomIdMap, allStateGroupMaps, metricToActivationMap,
                        trackerToMetricMap, conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason, configMetadataProvider);
                break;
            }
            default: {
                ALOGE("Metric \"%lld\" update state is unknown. This should never happen",
                      (long long)metric.id());
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_UPDATE_STATUS_UNKNOWN,
                                           metric.id());
            }
        }
        if (!producer) {
            return invalidConfigReason;
        }
        newMetricProducers.push_back(producer.value());
    }

    for (int i = 0; i < config.kll_metric_size(); i++, metricIndex++) {
        const KllMetric& metric = config.kll_metric(i);
        newMetricProducerMap[metric.id()] = metricIndex;
        optional<sp<MetricProducer>> producer;
        switch (metricUpdateStatus[metricIndex]) {
            case UPDATE_PRESERVE: {
                producer = updateMetric(
                        config, i, metricIndex, metric.id(), allAtomMatchingTrackers,
                        oldAtomMatchingTrackerMap, newAtomMatchingTrackerMap, matcherWizard,
                        allConditionTrackers, conditionTrackerMap, wizard, oldMetricProducerMap,
                        oldMetricProducers, metricToActivationMap, trackerToMetricMap,
                        conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason);
                break;
            }
            case UPDATE_REPLACE:
                replacedMetrics.insert(metric.id());
                [[fallthrough]];  // Intentionally fallthrough to create the new metric
                                  // producer.
            case UPDATE_NEW: {
                producer = createKllMetricProducerAndUpdateMetadata(
                        key, config, timeBaseNs, currentTimeNs, pullerManager, metric, metricIndex,
                        allAtomMatchingTrackers, newAtomMatchingTrackerMap, allConditionTrackers,
                        conditionTrackerMap, initialConditionCache, wizard, matcherWizard,
                        stateAtomIdMap, allStateGroupMaps, metricToActivationMap,
                        trackerToMetricMap, conditionToMetricMap, activationAtomTrackerToMetricMap,
                        deactivationAtomTrackerToMetricMap, metricsWithActivation,
                        invalidConfigReason, configMetadataProvider);
                break;
            }
            default: {
                ALOGE("Metric \"%lld\" update state is unknown. This should never happen",
                      (long long)metric.id());
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_UPDATE_STATUS_UNKNOWN,
                                           metric.id());
            }
        }
        if (!producer) {
            return invalidConfigReason;
        }
        newMetricProducers.push_back(producer.value());
    }

    for (int i = 0; i < config.no_report_metric_size(); ++i) {
        const int64_t noReportMetric = config.no_report_metric(i);
        if (newMetricProducerMap.find(noReportMetric) == newMetricProducerMap.end()) {
            ALOGW("no_report_metric %" PRId64 " not exist", noReportMetric);
            return InvalidConfigReason(INVALID_CONFIG_REASON_NO_REPORT_METRIC_NOT_FOUND,
                                       noReportMetric);
        }
        noReportMetricIds.insert(noReportMetric);
    }
    const set<int> atomsAllowedFromAnyUid(config.whitelisted_atom_ids().begin(),
                                          config.whitelisted_atom_ids().end());
    for (int i = 0; i < allMetricsCount; i++) {
        sp<MetricProducer> producer = newMetricProducers[i];
        // Register metrics to StateTrackers
        for (int atomId : producer->getSlicedStateAtoms()) {
            // Register listener for atoms that use allowed_log_sources.
            // Using atoms allowed from any uid as a sliced state atom is not allowed.
            // Redo this check for all metrics in case the atoms allowed from any uid changed.
            if (atomsAllowedFromAnyUid.find(atomId) != atomsAllowedFromAnyUid.end()) {
                return InvalidConfigReason(
                        INVALID_CONFIG_REASON_METRIC_SLICED_STATE_ATOM_ALLOWED_FROM_ANY_UID,
                        producer->getMetricId());
                // Preserved metrics should've already registered.`
            } else if (metricUpdateStatus[i] != UPDATE_PRESERVE) {
                StateManager::getInstance().registerListener(atomId, producer);
            }
        }
    }

    // Init new/replaced metrics.
    for (size_t i = 0; i < newMetricProducers.size(); i++) {
        if (metricUpdateStatus[i] == UPDATE_REPLACE || metricUpdateStatus[i] == UPDATE_NEW) {
            newMetricProducers[i]->prepareFirstBucket();
        }
    }

    for (const sp<MetricProducer>& oldMetricProducer : oldMetricProducers) {
        const auto& it = newMetricProducerMap.find(oldMetricProducer->getMetricId());
        // Consider metric removed if it's not present in newMetricProducerMap or it's replaced.
        if (it == newMetricProducerMap.end() ||
            replacedMetrics.find(oldMetricProducer->getMetricId()) != replacedMetrics.end()) {
            oldMetricProducer->onMetricRemove();
        }
    }
    return nullopt;
}

optional<InvalidConfigReason> determineAlertUpdateStatus(
        const Alert& alert, const unordered_map<int64_t, int>& oldAlertTrackerMap,
        const vector<sp<AnomalyTracker>>& oldAnomalyTrackers, const set<int64_t>& replacedMetrics,
        UpdateStatus& updateStatus) {
    // Check if new alert.
    const auto& oldAnomalyTrackerIt = oldAlertTrackerMap.find(alert.id());
    if (oldAnomalyTrackerIt == oldAlertTrackerMap.end()) {
        updateStatus = UPDATE_NEW;
        return nullopt;
    }

    // This is an existing alert, check if it has changed.
    string serializedAlert;
    if (!alert.SerializeToString(&serializedAlert)) {
        ALOGW("Unable to serialize alert %lld", (long long)alert.id());
        return createInvalidConfigReasonWithAlert(INVALID_CONFIG_REASON_ALERT_SERIALIZATION_FAILED,
                                                  alert.id());
    }
    uint64_t newProtoHash = Hash64(serializedAlert);
    const auto [invalidConfigReason, oldProtoHash] =
            oldAnomalyTrackers[oldAnomalyTrackerIt->second]->getProtoHash();
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }
    if (newProtoHash != oldProtoHash) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }

    // Check if the metric this alert relies on has changed.
    if (replacedMetrics.find(alert.metric_id()) != replacedMetrics.end()) {
        updateStatus = UPDATE_REPLACE;
        return nullopt;
    }

    updateStatus = UPDATE_PRESERVE;
    return nullopt;
}

optional<InvalidConfigReason> updateAlerts(const StatsdConfig& config, const int64_t currentTimeNs,
                                           const unordered_map<int64_t, int>& metricProducerMap,
                                           const set<int64_t>& replacedMetrics,
                                           const unordered_map<int64_t, int>& oldAlertTrackerMap,
                                           const vector<sp<AnomalyTracker>>& oldAnomalyTrackers,
                                           const sp<AlarmMonitor>& anomalyAlarmMonitor,
                                           vector<sp<MetricProducer>>& allMetricProducers,
                                           unordered_map<int64_t, int>& newAlertTrackerMap,
                                           vector<sp<AnomalyTracker>>& newAnomalyTrackers) {
    int alertCount = config.alert_size();
    vector<UpdateStatus> alertUpdateStatuses(alertCount);
    optional<InvalidConfigReason> invalidConfigReason;
    for (int i = 0; i < alertCount; i++) {
        invalidConfigReason =
                determineAlertUpdateStatus(config.alert(i), oldAlertTrackerMap, oldAnomalyTrackers,
                                           replacedMetrics, alertUpdateStatuses[i]);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    for (int i = 0; i < alertCount; i++) {
        const Alert& alert = config.alert(i);
        newAlertTrackerMap[alert.id()] = newAnomalyTrackers.size();
        switch (alertUpdateStatuses[i]) {
            case UPDATE_PRESERVE: {
                // Find the alert and update it.
                const auto& oldAnomalyTrackerIt = oldAlertTrackerMap.find(alert.id());
                if (oldAnomalyTrackerIt == oldAlertTrackerMap.end()) {
                    ALOGW("Could not find AnomalyTracker %lld in the previous config, but "
                          "expected it to be there",
                          (long long)alert.id());
                    return createInvalidConfigReasonWithAlert(
                            INVALID_CONFIG_REASON_ALERT_NOT_IN_PREV_CONFIG, alert.id());
                }
                sp<AnomalyTracker> anomalyTracker = oldAnomalyTrackers[oldAnomalyTrackerIt->second];
                anomalyTracker->onConfigUpdated();
                // Add the alert to the relevant metric.
                const auto& metricProducerIt = metricProducerMap.find(alert.metric_id());
                if (metricProducerIt == metricProducerMap.end()) {
                    ALOGW("alert \"%lld\" has unknown metric id: \"%lld\"", (long long)alert.id(),
                          (long long)alert.metric_id());
                    return createInvalidConfigReasonWithAlert(
                            INVALID_CONFIG_REASON_ALERT_METRIC_NOT_FOUND, alert.metric_id(),
                            alert.id());
                }
                allMetricProducers[metricProducerIt->second]->addAnomalyTracker(anomalyTracker,
                                                                                currentTimeNs);
                newAnomalyTrackers.push_back(anomalyTracker);
                break;
            }
            case UPDATE_REPLACE:
            case UPDATE_NEW: {
                optional<sp<AnomalyTracker>> anomalyTracker = createAnomalyTracker(
                        alert, anomalyAlarmMonitor, alertUpdateStatuses[i], currentTimeNs,
                        metricProducerMap, allMetricProducers, invalidConfigReason);
                if (!anomalyTracker) {
                    return invalidConfigReason;
                }
                newAnomalyTrackers.push_back(anomalyTracker.value());
                break;
            }
            default: {
                ALOGE("Alert \"%lld\" update state is unknown. This should never happen",
                      (long long)alert.id());
                return createInvalidConfigReasonWithAlert(
                        INVALID_CONFIG_REASON_ALERT_UPDATE_STATUS_UNKNOWN, alert.id());
            }
        }
    }
    invalidConfigReason = initSubscribersForSubscriptionType(
            config, Subscription::ALERT, newAlertTrackerMap, newAnomalyTrackers);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }
    return nullopt;
}

optional<InvalidConfigReason> updateStatsdConfig(
        const ConfigKey& key, const StatsdConfig& config, const sp<UidMap>& uidMap,
        const sp<StatsPullerManager>& pullerManager, const sp<AlarmMonitor>& anomalyAlarmMonitor,
        const sp<AlarmMonitor>& periodicAlarmMonitor, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const vector<sp<AtomMatchingTracker>>& oldAtomMatchingTrackers,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const vector<sp<ConditionTracker>>& oldConditionTrackers,
        const unordered_map<int64_t, int>& oldConditionTrackerMap,
        const vector<sp<MetricProducer>>& oldMetricProducers,
        const unordered_map<int64_t, int>& oldMetricProducerMap,
        const vector<sp<AnomalyTracker>>& oldAnomalyTrackers,
        const unordered_map<int64_t, int>& oldAlertTrackerMap,
        const map<int64_t, uint64_t>& oldStateProtoHashes,
        const wp<ConfigMetadataProvider> configMetadataProvider,
        std::unordered_map<int, std::vector<int>>& allTagIdsToMatchersMap,
        vector<sp<AtomMatchingTracker>>& newAtomMatchingTrackers,
        unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        vector<sp<ConditionTracker>>& newConditionTrackers,
        unordered_map<int64_t, int>& newConditionTrackerMap,
        vector<sp<MetricProducer>>& newMetricProducers,
        unordered_map<int64_t, int>& newMetricProducerMap,
        vector<sp<AnomalyTracker>>& newAnomalyTrackers,
        unordered_map<int64_t, int>& newAlertTrackerMap,
        vector<sp<AlarmTracker>>& newPeriodicAlarmTrackers,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& trackerToConditionMap,
        unordered_map<int, vector<int>>& activationTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationTrackerToMetricMap,
        vector<int>& metricsWithActivation, map<int64_t, uint64_t>& newStateProtoHashes,
        set<int64_t>& noReportMetricIds) {
    set<int64_t> replacedMatchers;
    set<int64_t> replacedConditions;
    set<int64_t> replacedStates;
    set<int64_t> replacedMetrics;
    vector<ConditionState> conditionCache;
    unordered_map<int64_t, int> stateAtomIdMap;
    unordered_map<int64_t, unordered_map<int, int64_t>> allStateGroupMaps;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    optional<InvalidConfigReason> invalidConfigReason;

    if (config.package_certificate_hash_size_bytes() > UINT8_MAX) {
        ALOGE("Invalid value for package_certificate_hash_size_bytes: %d",
              config.package_certificate_hash_size_bytes());
        return InvalidConfigReason(INVALID_CONFIG_REASON_PACKAGE_CERT_HASH_SIZE_TOO_LARGE);
    }

    bool allTrackersValid = updateAtomMatchingTrackers(
            config, uidMap, oldAtomMatchingTrackerMap, oldAtomMatchingTrackers,
            allTagIdsToMatchersMap, newAtomMatchingTrackerMap, newAtomMatchingTrackers,
            replacedMatchers, invalidEntities);
    if (!allTrackersValid) {
        ALOGE("updateAtomMatchingTrackers failed");
        return invalidEntities.begin()->second;
    }

    bool allConditionsValid = updateConditions(
            key, config, newAtomMatchingTrackerMap, replacedMatchers, oldConditionTrackerMap,
            oldConditionTrackers, newConditionTrackerMap, newConditionTrackers,
            trackerToConditionMap, conditionCache, replacedConditions, invalidEntities);
    if (!allConditionsValid) {
        ALOGE("updateConditions failed");
        return invalidEntities.begin()->second;
    }

    bool allStatesValid =
            updateStates(config, oldStateProtoHashes, stateAtomIdMap, allStateGroupMaps,
                         newStateProtoHashes, replacedStates, invalidEntities);
    if (!allStatesValid) {
        ALOGE("updateStates failed");
        return invalidEntities.begin()->second;
    }

    invalidConfigReason = updateMetrics(
            key, config, timeBaseNs, currentTimeNs, pullerManager, oldAtomMatchingTrackerMap,
            newAtomMatchingTrackerMap, replacedMatchers, newAtomMatchingTrackers,
            newConditionTrackerMap, replacedConditions, newConditionTrackers, conditionCache,
            stateAtomIdMap, allStateGroupMaps, replacedStates, oldMetricProducerMap,
            oldMetricProducers, configMetadataProvider, newMetricProducerMap, newMetricProducers,
            conditionToMetricMap, trackerToMetricMap, noReportMetricIds,
            activationTrackerToMetricMap, deactivationTrackerToMetricMap, metricsWithActivation,
            replacedMetrics);
    if (invalidConfigReason.has_value()) {
        ALOGE("updateMetrics failed");
        return invalidConfigReason;
    }

    invalidConfigReason = updateAlerts(config, currentTimeNs, newMetricProducerMap, replacedMetrics,
                                       oldAlertTrackerMap, oldAnomalyTrackers, anomalyAlarmMonitor,
                                       newMetricProducers, newAlertTrackerMap, newAnomalyTrackers);
    if (invalidConfigReason.has_value()) {
        ALOGE("updateAlerts failed");
        return invalidConfigReason;
    }

    invalidConfigReason = initAlarms(config, key, periodicAlarmMonitor, timeBaseNs, currentTimeNs,
                                     newPeriodicAlarmTrackers);
    // Alarms do not have any state, so we can reuse the initialization logic.
    if (invalidConfigReason.has_value()) {
        ALOGE("initAlarms failed");
        return invalidConfigReason;
    }
    return nullopt;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
