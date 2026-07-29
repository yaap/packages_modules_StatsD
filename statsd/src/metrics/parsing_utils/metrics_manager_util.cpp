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

#include "metrics_manager_util.h"

#include <inttypes.h>

#include <variant>

#include "FieldValue.h"
#include "condition/CombinationConditionTracker.h"
#include "condition/SimpleConditionTracker.h"
#include "external/StatsPullerManager.h"
#include "guardrail/StatsdStats.h"
#include "hash.h"
#include "matchers/CombinationAtomMatchingTracker.h"
#include "matchers/EventMatcherWizard.h"
#include "matchers/SimpleAtomMatchingTracker.h"
#include "metrics/CountMetricProducer.h"
#include "metrics/DurationMetricProducer.h"
#include "metrics/EventMetricProducer.h"
#include "metrics/GaugeMetricProducer.h"
#include "metrics/KllMetricProducer.h"
#include "metrics/MetricProducer.h"
#include "metrics/NumericValueMetricProducer.h"
#include "metrics/RestrictedEventMetricProducer.h"
#include "metrics/parsing_utils/histogram_parsing_utils.h"
#include "state/StateManager.h"
#include "stats_util.h"

namespace android {
namespace os {
namespace statsd {

using google::protobuf::MessageLite;
using std::map;
using std::nullopt;
using std::optional;
using std::set;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::unordered_map;
using std::unordered_set;
using std::vector;

namespace {

bool hasLeafNode(const FieldMatcher& matcher) {
    if (!matcher.has_field()) {
        return false;
    }
    for (int i = 0; i < matcher.child_size(); ++i) {
        if (hasLeafNode(matcher.child(i))) {
            return true;
        }
    }
    return true;
}

// DFS for ensuring there is no
// 1. value matching in the FieldValueMatcher tree with Position::ALL.
// 2. string replacement in the FieldValueMatcher tree without a value matcher with Position::ANY.
// Using vector to keep track of visited FieldValueMatchers since we expect number of
// FieldValueMatchers to be low.
optional<InvalidConfigReasonEnum> validateFvmPositionAllAndAny(
        const FieldValueMatcher& fvm, bool inPositionAll, bool inPositionAny,
        vector<FieldValueMatcher const*>& visited) {
    visited.push_back(&fvm);
    inPositionAll = inPositionAll || fvm.position() == Position::ALL;
    inPositionAny = inPositionAny || fvm.position() == Position::ANY;
    if (fvm.value_matcher_case() == FieldValueMatcher::kMatchesTuple) {
        for (const FieldValueMatcher& childFvm : fvm.matches_tuple().field_value_matcher()) {
            if (std::find(visited.cbegin(), visited.cend(), &childFvm) != visited.cend()) {
                continue;
            }
            const optional<InvalidConfigReasonEnum> reasonEnum =
                    validateFvmPositionAllAndAny(childFvm, inPositionAll, inPositionAny, visited);
            if (reasonEnum != nullopt) {
                return reasonEnum;
            }
        }
        return nullopt;
    }
    if (inPositionAll && fvm.value_matcher_case() != FieldValueMatcher::VALUE_MATCHER_NOT_SET) {
        // value_matcher is set to something other than matches_tuple with Position::ALL
        return INVALID_CONFIG_REASON_MATCHER_VALUE_MATCHER_WITH_POSITION_ALL;
    }
    if (inPositionAny && fvm.value_matcher_case() == FieldValueMatcher::VALUE_MATCHER_NOT_SET &&
        fvm.has_replace_string()) {
        // value_matcher is not set and there is a string replacement with Position::ANY
        return INVALID_CONFIG_REASON_MATCHER_STRING_REPLACE_WITH_NO_VALUE_MATCHER_WITH_POSITION_ANY;
    }
    return nullopt;
}

optional<InvalidConfigReason> validateSimpleAtomMatcher(int64_t matcherId,
                                                        const SimpleAtomMatcher& simpleMatcher) {
    for (const FieldValueMatcher& fvm : simpleMatcher.field_value_matcher()) {
        if (fvm.value_matcher_case() == FieldValueMatcher::VALUE_MATCHER_NOT_SET &&
            !fvm.has_replace_string()) {
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_MATCHER_NO_VALUE_MATCHER_NOR_STRING_REPLACER, matcherId);
        } else if (fvm.has_replace_string() &&
                   !(fvm.value_matcher_case() == FieldValueMatcher::VALUE_MATCHER_NOT_SET ||
                     fvm.value_matcher_case() == FieldValueMatcher::kEqString ||
                     fvm.value_matcher_case() == FieldValueMatcher::kEqAnyString ||
                     fvm.value_matcher_case() == FieldValueMatcher::kNeqAnyString ||
                     fvm.value_matcher_case() == FieldValueMatcher::kEqWildcardString ||
                     fvm.value_matcher_case() == FieldValueMatcher::kEqAnyWildcardString ||
                     fvm.value_matcher_case() == FieldValueMatcher::kNeqAnyWildcardString)) {
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_MATCHER_INVALID_VALUE_MATCHER_WITH_STRING_REPLACE,
                    matcherId);
        }
        vector<FieldValueMatcher const*> visited;
        const optional<InvalidConfigReasonEnum> reasonEnum = validateFvmPositionAllAndAny(
                fvm, false /* inPositionAll */, false /* inPositionAny */, visited);
        if (reasonEnum != nullopt) {
            return createInvalidConfigReasonWithMatcher(*reasonEnum, matcherId);
        }
    }
    return nullopt;
}

optional<InvalidConfigReason> checkMetricAtomMatchingTrackers(
        const int64_t matcherId, const int64_t metricId, const bool enforceOneAtom,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    if (invalidEntities.contains({matcherId, INVALID_ENTITY_TYPE_MATCHER})) {
        return createInvalidConfigReasonWithMatcher(
                INVALID_CONFIG_REASON_METRIC_INVALID_MATCHER_DEPENDENCY, metricId, matcherId);
    }
    auto logTrackerIt = atomMatchingTrackerMap.find(matcherId);
    if (logTrackerIt == atomMatchingTrackerMap.end()) {
        ALOGW("cannot find the AtomMatcher \"%lld\" in config", (long long)matcherId);
        return createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_METRIC_MATCHER_NOT_FOUND,
                                                    metricId, matcherId);
    }
    if (enforceOneAtom && allAtomMatchingTrackers[logTrackerIt->second]->getAtomIds().size() > 1) {
        ALOGE("AtomMatcher \"%lld\" has more than one tag ids. When a metric has dimension, "
              "the \"what\" can only be about one atom type. trigger_event matchers can also only "
              "be about one atom type.",
              (long long)matcherId);
        return createInvalidConfigReasonWithMatcher(
                INVALID_CONFIG_REASON_METRIC_MATCHER_MORE_THAN_ONE_ATOM, metricId, matcherId);
    }
    return nullopt;
}

optional<InvalidConfigReason> checkMetricWithConditions(
        const int64_t condition, const int64_t metricId,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const ::google::protobuf::RepeatedPtrField<MetricConditionLink>& links,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    if (invalidEntities.contains({condition, INVALID_ENTITY_TYPE_PREDICATE})) {
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_METRIC_INVALID_PREDICATE_DEPENDENCY, metricId, condition);
    }
    auto condition_it = conditionTrackerMap.find(condition);
    if (condition_it == conditionTrackerMap.end()) {
        ALOGW("cannot find Predicate \"%lld\" in the config", (long long)condition);
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_METRIC_CONDITION_NOT_FOUND, metricId, condition);
    }
    for (const auto& link : links) {
        auto it = conditionTrackerMap.find(link.condition());
        if (it == conditionTrackerMap.end()) {
            ALOGW("cannot find Predicate \"%lld\" in the config", (long long)link.condition());
            return createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_METRIC_CONDITION_LINK_NOT_FOUND, metricId,
                    link.condition());
        }
    }
    return nullopt;
}

optional<InvalidConfigReason> checkMetricWithStates(
        const StatsdConfig& config, const int64_t metricId,
        const ::google::protobuf::RepeatedField<int64_t>& stateIds,
        const unordered_map<int64_t, int>& stateAtomIdMap, const set<int> atomsAllowedFromAnyUid) {
    for (const auto& stateId : stateIds) {
        auto it = stateAtomIdMap.find(stateId);
        if (it == stateAtomIdMap.end()) {
            ALOGW("cannot find State %" PRId64 " in the config", stateId);
            return createInvalidConfigReasonWithState(INVALID_CONFIG_REASON_METRIC_STATE_NOT_FOUND,
                                                      metricId, stateId);
        }
        int atomId = it->second;
        if (atomsAllowedFromAnyUid.find(atomId) != atomsAllowedFromAnyUid.end()) {
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_METRIC_SLICED_STATE_ATOM_ALLOWED_FROM_ANY_UID, metricId);
        }
    }
    return nullopt;
}

// Validates a metricActivation.
optional<InvalidConfigReason> checkMetricActivation(
        const StatsdConfig& config, const int64_t metricId,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    // Check if metric has an associated activation
    auto itr = metricToActivationMap.find(metricId);
    if (itr == metricToActivationMap.end()) {
        return nullopt;
    }

    int activationIndex = itr->second;
    const MetricActivation& metricActivation = config.metric_activation(activationIndex);

    for (int i = 0; i < metricActivation.event_activation_size(); i++) {
        const EventActivation& activation = metricActivation.event_activation(i);
        if (invalidEntities.contains(
                    {activation.atom_matcher_id(), INVALID_ENTITY_TYPE_PREDICATE})) {
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_METRIC_ACTIVATION_INVALID_MATCHER_DEPENDENCY, metricId,
                    activation.atom_matcher_id());
        }
        auto itr = atomMatchingTrackerMap.find(activation.atom_matcher_id());
        if (itr == atomMatchingTrackerMap.end()) {
            ALOGE("Atom matcher not found for event activation.");
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_METRIC_ACTIVATION_MATCHER_NOT_FOUND, metricId,
                    activation.atom_matcher_id());
        }

        if (activation.has_deactivation_atom_matcher_id()) {
            if (invalidEntities.contains({activation.deactivation_atom_matcher_id(),
                                          INVALID_ENTITY_TYPE_PREDICATE})) {
                return createInvalidConfigReasonWithMatcher(
                        INVALID_CONFIG_REASON_METRIC_ACTIVATION_INVALID_MATCHER_DEPENDENCY,
                        metricId, activation.deactivation_atom_matcher_id());
            }
            itr = atomMatchingTrackerMap.find(activation.deactivation_atom_matcher_id());
            if (itr == atomMatchingTrackerMap.end()) {
                ALOGE("Atom matcher not found for event deactivation.");
                return createInvalidConfigReasonWithMatcher(
                        INVALID_CONFIG_REASON_METRIC_DEACTIVATION_MATCHER_NOT_FOUND, metricId,
                        activation.deactivation_atom_matcher_id());
            }
        }
    }
    return nullopt;
}

optional<InvalidConfigReason> checkMetricWithDimensionalSampling(
        const int64_t metricId, const DimensionalSamplingInfo& dimSamplingInfo,
        const vector<Matcher>& dimensionsInWhat) {
    if (!dimSamplingInfo.has_sampled_what_field()) {
        ALOGE("metric DimensionalSamplingInfo missing sampledWhatField");
        return InvalidConfigReason(
                INVALID_CONFIG_REASON_METRIC_DIMENSIONAL_SAMPLING_INFO_MISSING_SAMPLED_FIELD,
                metricId);
    }

    if (dimSamplingInfo.shard_count() <= 1) {
        ALOGE("metric shardCount must be > 1");
        return InvalidConfigReason(
                INVALID_CONFIG_REASON_METRIC_DIMENSIONAL_SAMPLING_INFO_INCORRECT_SHARD_COUNT,
                metricId);
    }

    if (HasPositionALL(dimSamplingInfo.sampled_what_field()) ||
        HasPositionANY(dimSamplingInfo.sampled_what_field())) {
        ALOGE("metric has repeated field with position ALL or ANY as the sampled dimension");
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELD_INCORRECT_SIZE,
                                   metricId);
    }
    SamplingInfo samplingInfo;
    translateFieldMatcher(dimSamplingInfo.sampled_what_field(), &samplingInfo.sampledWhatFields);
    if (samplingInfo.sampledWhatFields.size() != 1) {
        ALOGE("metric has incorrect number of sampled dimension fields");
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELD_INCORRECT_SIZE,
                                   metricId);
    }
    if (!subsetDimensions(samplingInfo.sampledWhatFields, dimensionsInWhat)) {
        return InvalidConfigReason(
                INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELDS_NOT_SUBSET_DIM_IN_WHAT, metricId);
    }
    return nullopt;
}

template <typename T>
optional<InvalidConfigReason> checkUidFields(const T& metric) {
    if (metric.has_uid_fields()) {
        if (HasPositionANY(metric.uid_fields())) {
            ALOGE("Metric %lld has position ANY in uid fields", (long long)metric.id());
            return InvalidConfigReason(INVALID_CONFIG_REASON_UID_FIELDS_WITH_POSITION_ANY,
                                       metric.id());
        }
    }
    return nullopt;
}

template <typename T>
optional<InvalidConfigReason> checkCommonMetricFields(
        const StatsdConfig& config, const T& metric,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    const auto& it = invalidEntities.find({metric.id(), INVALID_ENTITY_TYPE_METRIC});
    if (it != invalidEntities.end()) {
        return it->second;
    }
    if (!metric.has_id() || !metric.has_what()) {
        ALOGE("cannot find metric id or \"what\" in metric \"%lld\"", (long long)metric.id());
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metric.id());
    }

    optional<InvalidConfigReason> invalidConfigReason;
    if (metric.has_condition()) {
        invalidConfigReason =
                checkMetricWithConditions(metric.condition(), metric.id(), conditionTrackerMap,
                                          metric.links(), invalidEntities);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    } else if (metric.links_size() > 0) {
        ALOGW("metrics has a MetricConditionLink but doesn't have a condition");
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                   metric.id());
    }

    if (metric.slice_by_state_size() > 0) {
        invalidConfigReason = checkMetricWithStates(config, metric.id(), metric.slice_by_state(),
                                                    stateAtomIdMap, atomsAllowedFromAnyUid);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    } else if (metric.state_link_size() > 0) {
        ALOGW("Metric has a MetricStateLink but doesn't have a sliced state");
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_STATELINK_NO_STATE, metric.id());
    }

    invalidConfigReason = checkMetricActivation(config, metric.id(), metricToActivationMap,
                                                atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    uint64_t metricHash;
    invalidConfigReason =
            getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    invalidConfigReason = checkUidFields(metric);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }
    return nullopt;
}

}  // namespace

sp<AtomMatchingTracker> createAtomMatchingTracker(
        const AtomMatcher& logMatcher, const sp<UidMap>& uidMap,
        optional<InvalidConfigReason>& invalidConfigReason) {
    string serializedMatcher;
    if (!logMatcher.SerializeToString(&serializedMatcher)) {
        ALOGE("Unable to serialize matcher %lld", (long long)logMatcher.id());
        invalidConfigReason = createInvalidConfigReasonWithMatcher(
                INVALID_CONFIG_REASON_MATCHER_SERIALIZATION_FAILED, logMatcher.id());
        return nullptr;
    }
    uint64_t protoHash = Hash64(serializedMatcher);
    switch (logMatcher.contents_case()) {
        case AtomMatcher::ContentsCase::kSimpleAtomMatcher: {
            invalidConfigReason =
                    validateSimpleAtomMatcher(logMatcher.id(), logMatcher.simple_atom_matcher());
            if (invalidConfigReason != nullopt) {
                ALOGE("Matcher \"%lld\" malformed", (long long)logMatcher.id());
                return nullptr;
            }
            sp<AtomMatchingTracker> simpleAtomMatcher = new SimpleAtomMatchingTracker(
                    logMatcher.id(), protoHash, logMatcher.simple_atom_matcher(), uidMap);
            return simpleAtomMatcher;
        }
        case AtomMatcher::ContentsCase::kCombination:
            return new CombinationAtomMatchingTracker(logMatcher.id(), protoHash);
        default:
            ALOGE("Matcher \"%lld\" malformed", (long long)logMatcher.id());
            invalidConfigReason = createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE, logMatcher.id());
            return nullptr;
    }
}

sp<ConditionTracker> createConditionTracker(
        const ConfigKey& key, const Predicate& predicate,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities,
        optional<InvalidConfigReason>& invalidConfigReason) {
    string serializedPredicate;
    if (!predicate.SerializeToString(&serializedPredicate)) {
        ALOGE("Unable to serialize predicate %lld", (long long)predicate.id());
        invalidConfigReason = createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_CONDITION_SERIALIZATION_FAILED, predicate.id());
        return nullptr;
    }
    uint64_t protoHash = Hash64(serializedPredicate);
    switch (predicate.contents_case()) {
        case Predicate::ContentsCase::kSimplePredicate: {
            // Check if dependencies are all valid.
            const auto& simplePredicate = predicate.simple_predicate();
            if (simplePredicate.has_start() &&
                invalidEntities.contains({simplePredicate.start(), INVALID_ENTITY_TYPE_MATCHER})) {
                invalidConfigReason = createInvalidConfigReasonWithPredicate(
                        INVALID_CONFIG_REASON_CONDITION_INVALID_MATCHER_DEPENDENCY, predicate.id());
                invalidConfigReason->matcherIds.push_back(simplePredicate.start());
                return nullptr;
            }
            if (simplePredicate.has_stop() &&
                invalidEntities.contains({simplePredicate.stop(), INVALID_ENTITY_TYPE_MATCHER})) {
                invalidConfigReason = createInvalidConfigReasonWithPredicate(
                        INVALID_CONFIG_REASON_CONDITION_INVALID_MATCHER_DEPENDENCY, predicate.id());
                invalidConfigReason->matcherIds.push_back(simplePredicate.stop());
                return nullptr;
            }
            if (simplePredicate.has_stop_all() &&
                invalidEntities.contains(
                        {simplePredicate.stop_all(), INVALID_ENTITY_TYPE_MATCHER})) {
                invalidConfigReason = createInvalidConfigReasonWithPredicate(
                        INVALID_CONFIG_REASON_CONDITION_INVALID_MATCHER_DEPENDENCY, predicate.id());
                invalidConfigReason->matcherIds.push_back(simplePredicate.stop_all());
                return nullptr;
            }
            return new SimpleConditionTracker(key, predicate.id(), protoHash);
        }
        case Predicate::ContentsCase::kCombination: {
            // Check if dependencies are all valid.
            const auto& combinationPredicate = predicate.combination();
            if (!combinationPredicate.has_operation()) {
                invalidConfigReason = createInvalidConfigReasonWithPredicate(
                        INVALID_CONFIG_REASON_CONDITION_NO_OPERATION, predicate.id());
                return nullptr;
            }
            if (combinationPredicate.operation() == LogicalOperation::NOT &&
                combinationPredicate.predicate_size() != 1) {
                invalidConfigReason = createInvalidConfigReasonWithPredicate(
                        INVALID_CONFIG_REASON_CONDITION_NOT_OPERATION_IS_NOT_UNARY, predicate.id());
                return nullptr;
            }
            return new CombinationConditionTracker(predicate.id(), protoHash);
        }
        default:
            ALOGE("Predicate \"%lld\" malformed", (long long)predicate.id());
            invalidConfigReason = createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_CONDITION_MALFORMED_CONTENTS_CASE, predicate.id());
            return nullptr;
    }
}

optional<InvalidConfigReason> getMetricProtoHash(
        const StatsdConfig& config, const MessageLite& metric, const int64_t id,
        const unordered_map<int64_t, int>& metricToActivationMap, uint64_t& metricHash) {
    string serializedMetric;
    if (!metric.SerializeToString(&serializedMetric)) {
        ALOGE("Unable to serialize metric %lld", (long long)id);
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SERIALIZATION_FAILED, id);
    }
    metricHash = Hash64(serializedMetric);

    // Combine with activation hash, if applicable
    const auto& metricActivationIt = metricToActivationMap.find(id);
    if (metricActivationIt != metricToActivationMap.end()) {
        string serializedActivation;
        const MetricActivation& activation = config.metric_activation(metricActivationIt->second);
        if (!activation.SerializeToString(&serializedActivation)) {
            ALOGE("Unable to serialize metric activation for metric %lld", (long long)id);
            return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_ACTIVATION_SERIALIZATION_FAILED,
                                       id);
        }
        metricHash = Hash64(to_string(metricHash).append(to_string(Hash64(serializedActivation))));
    }
    return nullopt;
}

void handleMetricWithAtomMatchingTrackers(const int64_t matcherId, const int metricIndex,
                                          const unordered_map<int64_t, int>& atomMatchingTrackerMap,
                                          unordered_map<int, vector<int>>& trackerToMetricMap,
                                          int& logTrackerIndex) {
    logTrackerIndex = atomMatchingTrackerMap.at(matcherId);
    trackerToMetricMap[logTrackerIndex].push_back(metricIndex);
}

void handleMetricWithConditions(const int64_t condition, const int metricIndex,
                                const unordered_map<int64_t, int>& conditionTrackerMap,
                                int& conditionIndex,
                                unordered_map<int, vector<int>>& conditionToMetricMap) {
    conditionIndex = conditionTrackerMap.at(condition);
    // will create new vector if not exist before.
    conditionToMetricMap[conditionIndex].push_back(metricIndex);
}

// Initializes state data structures for a metric.
// input:
// [stateIds]: the slice_by_state ids for this metric
// [stateAtomIdMap]: this map contains the mapping from all state ids to atom ids
// [allStateGroupMaps]: this map contains the mapping from state ids and state
//                      values to state group ids for all states
// output:
// [slicedStateAtoms]: a vector of atom ids of all the slice_by_states
// [stateGroupMap]: this map should contain the mapping from states ids and state
//                      values to state group ids for all states that this metric
//                      is interested in
optional<InvalidConfigReason> handleMetricWithStates(
        const ::google::protobuf::RepeatedField<int64_t>& stateIds,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
        vector<int>& slicedStateAtoms,
        unordered_map<int, unordered_map<int, int64_t>>& stateGroupMap) {
    for (const auto& stateId : stateIds) {
        int atomId = stateAtomIdMap.at(stateId);
        slicedStateAtoms.push_back(atomId);

        auto stateIt = allStateGroupMaps.find(stateId);
        if (stateIt != allStateGroupMaps.end()) {
            stateGroupMap[atomId] = stateIt->second;
        }
    }
    return nullopt;
}

optional<InvalidConfigReason> checkMetricWithStateLink(const int64_t metricId,
                                                       const FieldMatcher& stateMatcher,
                                                       const vector<Matcher>& dimensionsInWhat) {
    vector<Matcher> stateMatchers;
    translateFieldMatcher(stateMatcher, &stateMatchers);
    if (!subsetDimensions(stateMatchers, dimensionsInWhat)) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_STATELINKS_NOT_SUBSET_DIM_IN_WHAT,
                                   metricId);
    }
    return nullopt;
}

void handleMetricWithDimensionalSampling(const DimensionalSamplingInfo& dimSamplingInfo,
                                         SamplingInfo& samplingInfo) {
    samplingInfo.shardCount = dimSamplingInfo.shard_count();
    translateFieldMatcher(dimSamplingInfo.sampled_what_field(), &samplingInfo.sampledWhatFields);
}

template <typename T>
void setUidFieldsIfNecessary(const T& metric, sp<MetricProducer> metricProducer) {
    if (metric.has_uid_fields()) {
        std::vector<Matcher> uidFields;
        translateFieldMatcher(metric.uid_fields(), &uidFields);
        metricProducer->setUidFields(uidFields);
    }
}

// Validates a metricActivation and populates state.
// EventActivationMap and EventDeactivationMap are supplied to a MetricProducer
// to provide the producer with state about its activators and deactivators.
void handleMetricActivation(
        const StatsdConfig& config, const int64_t metricId, const int metricIndex,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        unordered_map<int, shared_ptr<Activation>>& eventActivationMap,
        unordered_map<int, vector<shared_ptr<Activation>>>& eventDeactivationMap) {
    // Check if metric has an associated activation
    auto itr = metricToActivationMap.find(metricId);
    if (itr == metricToActivationMap.end()) {
        return;
    }

    int activationIndex = itr->second;
    const MetricActivation& metricActivation = config.metric_activation(activationIndex);

    for (int i = 0; i < metricActivation.event_activation_size(); i++) {
        const EventActivation& activation = metricActivation.event_activation(i);

        ActivationType activationType = (activation.has_activation_type())
                                                ? activation.activation_type()
                                                : metricActivation.activation_type();
        std::shared_ptr<Activation> activationWrapper =
                std::make_shared<Activation>(activationType, activation.ttl_seconds() * NS_PER_SEC);

        int atomMatcherIndex = atomMatchingTrackerMap.at(activation.atom_matcher_id());
        activationAtomTrackerToMetricMap[atomMatcherIndex].push_back(metricIndex);
        eventActivationMap.emplace(atomMatcherIndex, activationWrapper);

        if (activation.has_deactivation_atom_matcher_id()) {
            int deactivationAtomMatcherIndex =
                    atomMatchingTrackerMap.at(activation.deactivation_atom_matcher_id());
            deactivationAtomTrackerToMetricMap[deactivationAtomMatcherIndex].push_back(metricIndex);
            eventDeactivationMap[deactivationAtomMatcherIndex].push_back(activationWrapper);
        }
    }
    metricsWithActivation.push_back(metricIndex);
}

optional<InvalidConfigReason> checkMetricActivationOnConfigUpdate(
        const StatsdConfig& config, const int64_t metricId,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int, shared_ptr<Activation>>& oldEventActivationMap) {
    // Check if metric has an associated activation.
    const auto& itr = metricToActivationMap.find(metricId);
    if (itr == metricToActivationMap.end()) {
        return nullopt;
    }

    int activationIndex = itr->second;
    const MetricActivation& metricActivation = config.metric_activation(activationIndex);

    for (int i = 0; i < metricActivation.event_activation_size(); i++) {
        const int64_t activationMatcherId = metricActivation.event_activation(i).atom_matcher_id();

        // Find the old activation struct and copy it over.
        const auto& oldActivationIt = oldAtomMatchingTrackerMap.find(activationMatcherId);
        if (oldActivationIt == oldAtomMatchingTrackerMap.end()) {
            ALOGE("Atom matcher not found in existing config for event activation.");
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_METRIC_ACTIVATION_MATCHER_NOT_FOUND_EXISTING, metricId,
                    activationMatcherId);
        }
        int oldActivationMatcherIndex = oldActivationIt->second;
        const auto& oldEventActivationIt = oldEventActivationMap.find(oldActivationMatcherIndex);
        if (oldEventActivationIt == oldEventActivationMap.end()) {
            ALOGE("Could not find existing event activation to update");
            return createInvalidConfigReasonWithMatcher(
                    INVALID_CONFIG_REASON_METRIC_ACTIVATION_NOT_FOUND_EXISTING, metricId,
                    activationMatcherId);
        }
    }
    return nullopt;
}

// Validates a metricActivation and populates state.
// Fills the new event activation/deactivation maps, preserving the existing activations
// Returns false if there are errors.
void handleMetricActivationOnConfigUpdate(
        const StatsdConfig& config, const int64_t metricId, const int metricIndex,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const unordered_map<int, shared_ptr<Activation>>& oldEventActivationMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        unordered_map<int, shared_ptr<Activation>>& newEventActivationMap,
        unordered_map<int, vector<shared_ptr<Activation>>>& newEventDeactivationMap) {
    // Check if metric has an associated activation.
    const auto& itr = metricToActivationMap.find(metricId);
    if (itr == metricToActivationMap.end()) {
        return;
    }

    int activationIndex = itr->second;
    const MetricActivation& metricActivation = config.metric_activation(activationIndex);

    for (int i = 0; i < metricActivation.event_activation_size(); i++) {
        const int64_t activationMatcherId = metricActivation.event_activation(i).atom_matcher_id();

        int newActivationMatcherIndex = newAtomMatchingTrackerMap.at(activationMatcherId);

        // Find the old activation struct and copy it over.
        int oldActivationMatcherIndex = oldAtomMatchingTrackerMap.at(activationMatcherId);
        auto& oldEventActivation = oldEventActivationMap.at(oldActivationMatcherIndex);
        newEventActivationMap.emplace(newActivationMatcherIndex, oldEventActivation);
        activationAtomTrackerToMetricMap[newActivationMatcherIndex].push_back(metricIndex);

        if (metricActivation.event_activation(i).has_deactivation_atom_matcher_id()) {
            const int64_t deactivationMatcherId =
                    metricActivation.event_activation(i).deactivation_atom_matcher_id();
            int newDeactivationMatcherIndex = newAtomMatchingTrackerMap.at(deactivationMatcherId);
            newEventDeactivationMap[newDeactivationMatcherIndex].push_back(oldEventActivation);
            deactivationAtomTrackerToMetricMap[newDeactivationMatcherIndex].push_back(metricIndex);
        }
    }
    metricsWithActivation.push_back(metricIndex);
}

optional<InvalidConfigReason> isNewCountMetricValid(
        const StatsdConfig& config, const CountMetric& metric,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason = checkCommonMetricFields(
            config, metric, atomMatchingTrackerMap, conditionTrackerMap, stateAtomIdMap,
            metricToActivationMap, atomsAllowedFromAnyUid, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    invalidConfigReason = checkMetricAtomMatchingTrackers(
            metric.what(), metric.id(), metric.has_dimensions_in_what(), allAtomMatchingTrackers,
            atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    // Check that all metric state links are a subset of dimensions_in_what fields.
    std::vector<Matcher> dimensionsInWhat;
    translateFieldMatcher(metric.dimensions_in_what(), &dimensionsInWhat);
    for (const auto& stateLink : metric.state_link()) {
        invalidConfigReason =
                checkMetricWithStateLink(metric.id(), stateLink.fields_in_what(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            ALOGW("CountMetric's MetricStateLinks must be a subset of dimensions in what");
            return invalidConfigReason;
        }
    }

    if (metric.has_threshold() &&
        (metric.threshold().value_comparison_case() == UploadThreshold::kLtFloat ||
         metric.threshold().value_comparison_case() == UploadThreshold::kGtFloat)) {
        ALOGW("Count metric incorrect upload threshold type or no type used");
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_BAD_THRESHOLD, metric.id());
    }

    if (metric.has_dimensional_sampling_info()) {
        invalidConfigReason = checkMetricWithDimensionalSampling(
                metric.id(), metric.dimensional_sampling_info(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return nullopt;
}

sp<MetricProducer> createCountMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const CountMetric& metric, const int metricIndex,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider) {
    int trackerIndex;
    handleMetricWithAtomMatchingTrackers(metric.what(), metricIndex, atomMatchingTrackerMap,
                                         trackerToMetricMap, trackerIndex);

    int conditionIndex = -1;
    if (metric.has_condition()) {
        handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                   conditionIndex, conditionToMetricMap);
    }

    std::vector<int> slicedStateAtoms;
    unordered_map<int, unordered_map<int, int64_t>> stateGroupMap;
    if (metric.slice_by_state_size() > 0) {
        handleMetricWithStates(metric.slice_by_state(), stateAtomIdMap, allStateGroupMaps,
                               slicedStateAtoms, stateGroupMap);
    }

    unordered_map<int, shared_ptr<Activation>> eventActivationMap;
    unordered_map<int, vector<shared_ptr<Activation>>> eventDeactivationMap;
    handleMetricActivation(config, metric.id(), metricIndex, metricToActivationMap,
                           atomMatchingTrackerMap, activationAtomTrackerToMetricMap,
                           deactivationAtomTrackerToMetricMap, metricsWithActivation,
                           eventActivationMap, eventDeactivationMap);

    uint64_t metricHash;
    getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);

    sp<MetricProducer> metricProducer = new CountMetricProducer(
            key, metric, conditionIndex, initialConditionCache, wizard, metricHash, timeBaseNs,
            currentTimeNs, configMetadataProvider, eventActivationMap, eventDeactivationMap,
            slicedStateAtoms, stateGroupMap);

    SamplingInfo samplingInfo;
    if (metric.has_dimensional_sampling_info()) {
        handleMetricWithDimensionalSampling(metric.dimensional_sampling_info(), samplingInfo);
        metricProducer->setSamplingInfo(samplingInfo);
    }

    setUidFieldsIfNecessary(metric, metricProducer);
    return metricProducer;
}

optional<InvalidConfigReason> isNewDurationMetricValid(
        const StatsdConfig& config, const DurationMetric& metric,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason = checkCommonMetricFields(
            config, metric, atomMatchingTrackerMap, conditionTrackerMap, stateAtomIdMap,
            metricToActivationMap, atomsAllowedFromAnyUid, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (invalidEntities.contains({metric.what(), INVALID_ENTITY_TYPE_PREDICATE})) {
        ALOGE("returning invalid predicate dependency");
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_METRIC_INVALID_PREDICATE_DEPENDENCY, metric.id(),
                metric.what());
    }
    const auto& what_it = allConditionsMap.find(metric.what());
    if (what_it == allConditionsMap.end()) {
        ALOGE("DurationMetric's \"what\" is not present in the condition trackers");
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_DURATION_METRIC_WHAT_NOT_FOUND, metric.id(), metric.what());
    }

    const Predicate& durationWhat = (what_it->second).predicate;
    if (durationWhat.contents_case() != Predicate::ContentsCase::kSimplePredicate) {
        ALOGE("DurationMetric's \"what\" must be a simple condition");
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_DURATION_METRIC_WHAT_NOT_SIMPLE, metric.id(), metric.what());
    }

    const SimplePredicate& simplePredicate = durationWhat.simple_predicate();

    if (!simplePredicate.has_start()) {
        ALOGE("Duration metrics must specify a valid start event matcher");
        return createInvalidConfigReasonWithPredicate(
                INVALID_CONFIG_REASON_DURATION_METRIC_MISSING_START, metric.id(), metric.what());
    }
    invalidConfigReason = checkMetricAtomMatchingTrackers(
            simplePredicate.start(), metric.id(), metric.has_dimensions_in_what(),
            allAtomMatchingTrackers, atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (simplePredicate.has_stop()) {
        invalidConfigReason = checkMetricAtomMatchingTrackers(
                simplePredicate.stop(), metric.id(), metric.has_dimensions_in_what(),
                allAtomMatchingTrackers, atomMatchingTrackerMap, invalidEntities);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    if (simplePredicate.has_stop_all()) {
        invalidConfigReason = checkMetricAtomMatchingTrackers(
                simplePredicate.stop_all(), metric.id(), metric.has_dimensions_in_what(),
                allAtomMatchingTrackers, atomMatchingTrackerMap, invalidEntities);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    if (metric.slice_by_state_size() > 0) {
        if (metric.aggregation_type() == DurationMetric::MAX_SPARSE) {
            ALOGE("DurationMetric with aggregation type MAX_SPARSE cannot be sliced by state");
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_DURATION_METRIC_MAX_SPARSE_HAS_SLICE_BY_STATE,
                    metric.id());
        }
    }

    // Check that all metric state links are a subset of dimensions_in_what fields.
    std::vector<Matcher> dimensionsInWhat;
    translateFieldMatcher(metric.dimensions_in_what(), &dimensionsInWhat);
    for (const auto& stateLink : metric.state_link()) {
        invalidConfigReason =
                checkMetricWithStateLink(metric.id(), stateLink.fields_in_what(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            ALOGW("DurationMetric's MetricStateLinks must be a subset of dimensions in what");
            return invalidConfigReason;
        }
    }

    if (metric.has_threshold()) {
        switch (metric.threshold().value_comparison_case()) {
            case UploadThreshold::kLtInt:
            case UploadThreshold::kGtInt:
            case UploadThreshold::kLteInt:
            case UploadThreshold::kGteInt:
                break;
            default:
                ALOGE("Duration metric incorrect upload threshold type or no type used");
                return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_BAD_THRESHOLD, metric.id());
        }
    }

    const FieldMatcher& internalDimensions = simplePredicate.dimensions();
    vector<Matcher> translatedInternalDimensions;

    if (internalDimensions.has_field()) {
        translateFieldMatcher(internalDimensions, &translatedInternalDimensions);
    }
    // Dimensions in what must be subset of internal dimensions
    if (!subsetDimensions(dimensionsInWhat, translatedInternalDimensions)) {
        ALOGE("Dimensions in what must be a subset of the internal dimensions");
        return InvalidConfigReason(
                INVALID_CONFIG_REASON_METRIC_DIMENSIONS_IN_WHAT_NOT_SUBSET_OF_INTERNAL_DIMENSIONS,
                metric.id());
    }

    for (const auto& link : metric.links()) {
        std::vector<Matcher> metricFields;
        translateFieldMatcher(link.fields_in_what(), &metricFields);
        if (!subsetDimensions(metricFields, translatedInternalDimensions)) {
            ALOGE(("Condition links must be a subset of the internal dimensions"));
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_METRIC_CONDITION_LINKS_NOT_SUBSET_OF_INTERNAL_DIMENSIONS,
                    metric.id());
        }
    }

    // Checking state links being a subet of internal dimensions is not needed because
    // 1. dimensions_in_what being a subset of internal dims is already checked
    // 2. state links are subset of dimensions_in_what is already checked
    // 3. state links are transitively a subset of internal dims.

    if (metric.has_dimensional_sampling_info()) {
        invalidConfigReason = checkMetricWithDimensionalSampling(
                metric.id(), metric.dimensional_sampling_info(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return nullopt;
}

sp<MetricProducer> createDurationMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const DurationMetric& metric, const int metricIndex,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider) {
    const Predicate& durationWhat = allConditionsMap.at(metric.what()).predicate;
    const SimplePredicate& simplePredicate = durationWhat.simple_predicate();
    const bool nesting = simplePredicate.count_nesting();
    const int whatIndex = conditionTrackerMap.at(metric.what());
    int startIndex = -1, stopIndex = -1, stopAllIndex = -1;
    handleMetricWithAtomMatchingTrackers(simplePredicate.start(), metricIndex,
                                         atomMatchingTrackerMap, trackerToMetricMap, startIndex);

    if (simplePredicate.has_stop()) {
        handleMetricWithAtomMatchingTrackers(simplePredicate.stop(), metricIndex,
                                             atomMatchingTrackerMap, trackerToMetricMap, stopIndex);
    }

    if (simplePredicate.has_stop_all()) {
        handleMetricWithAtomMatchingTrackers(simplePredicate.stop_all(), metricIndex,
                                             atomMatchingTrackerMap, trackerToMetricMap,
                                             stopAllIndex);
    }

    int conditionIndex = -1;
    if (metric.has_condition()) {
        handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                   conditionIndex, conditionToMetricMap);
    }

    std::vector<int> slicedStateAtoms;
    unordered_map<int, unordered_map<int, int64_t>> stateGroupMap;
    if (metric.slice_by_state_size() > 0) {
        handleMetricWithStates(metric.slice_by_state(), stateAtomIdMap, allStateGroupMaps,
                               slicedStateAtoms, stateGroupMap);
    }

    unordered_map<int, shared_ptr<Activation>> eventActivationMap;
    unordered_map<int, vector<shared_ptr<Activation>>> eventDeactivationMap;
    handleMetricActivation(config, metric.id(), metricIndex, metricToActivationMap,
                           atomMatchingTrackerMap, activationAtomTrackerToMetricMap,
                           deactivationAtomTrackerToMetricMap, metricsWithActivation,
                           eventActivationMap, eventDeactivationMap);

    uint64_t metricHash;
    getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);

    const FieldMatcher& internalDimensions = simplePredicate.dimensions();

    sp<MetricProducer> metricProducer = new DurationMetricProducer(
            key, metric, conditionIndex, initialConditionCache, whatIndex, startIndex, stopIndex,
            stopAllIndex, nesting, wizard, metricHash, internalDimensions, timeBaseNs,
            currentTimeNs, configMetadataProvider, eventActivationMap, eventDeactivationMap,
            slicedStateAtoms, stateGroupMap);

    SamplingInfo samplingInfo;
    if (metric.has_dimensional_sampling_info()) {
        handleMetricWithDimensionalSampling(metric.dimensional_sampling_info(), samplingInfo);
        metricProducer->setSamplingInfo(samplingInfo);
    }

    setUidFieldsIfNecessary(metric, metricProducer);
    return metricProducer;
}

optional<InvalidConfigReason> isNewEventMetricValid(
        const StatsdConfig& config, const EventMetric& metric,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason = checkCommonMetricFields(
            config, metric, atomMatchingTrackerMap, conditionTrackerMap, stateAtomIdMap,
            metricToActivationMap, atomsAllowedFromAnyUid, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (metric.has_fields_filter()) {
        const FieldFilter& filter = metric.fields_filter();
        if ((filter.has_fields() && !hasLeafNode(filter.fields())) ||
            (filter.has_omit_fields() && !hasLeafNode(filter.omit_fields()))) {
            ALOGW("Incorrect field filter setting in EventMetric %lld", (long long)metric.id());
            return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_FIELD_FILTER,
                                       metric.id());
        }
    }

    invalidConfigReason = checkMetricAtomMatchingTrackers(metric.what(), metric.id(), false,
                                                          allAtomMatchingTrackers,
                                                          atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (metric.sampling_percentage() < 1 || metric.sampling_percentage() > 100) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_SAMPLING_PERCENTAGE,
                                   metric.id());
    }

    return nullopt;
}

sp<MetricProducer> createEventMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const bool isRestrictedMetric, const EventMetric& metric, const int metricIndex,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider) {
    int trackerIndex;
    handleMetricWithAtomMatchingTrackers(metric.what(), metricIndex, atomMatchingTrackerMap,
                                         trackerToMetricMap, trackerIndex);

    int conditionIndex = -1;
    if (metric.has_condition()) {
        handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                   conditionIndex, conditionToMetricMap);
    }

    std::vector<int> slicedStateAtoms;
    unordered_map<int, unordered_map<int, int64_t>> stateGroupMap;
    if (metric.slice_by_state_size() > 0) {
        handleMetricWithStates(metric.slice_by_state(), stateAtomIdMap, allStateGroupMaps,
                               slicedStateAtoms, stateGroupMap);
    }

    unordered_map<int, shared_ptr<Activation>> eventActivationMap;
    unordered_map<int, vector<shared_ptr<Activation>>> eventDeactivationMap;
    handleMetricActivation(config, metric.id(), metricIndex, metricToActivationMap,
                           atomMatchingTrackerMap, activationAtomTrackerToMetricMap,
                           deactivationAtomTrackerToMetricMap, metricsWithActivation,
                           eventActivationMap, eventDeactivationMap);

    uint64_t metricHash;
    getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);

    sp<MetricProducer> metricProducer;
    if (isRestrictedMetric) {
        metricProducer = new RestrictedEventMetricProducer(
                key, metric, conditionIndex, initialConditionCache, wizard, metricHash, timeBaseNs,
                configMetadataProvider, eventActivationMap, eventDeactivationMap, slicedStateAtoms,
                stateGroupMap);
    } else {
        metricProducer = new EventMetricProducer(
                key, metric, conditionIndex, initialConditionCache, wizard, metricHash, timeBaseNs,
                configMetadataProvider, eventActivationMap, eventDeactivationMap, slicedStateAtoms,
                stateGroupMap);
    }

    setUidFieldsIfNecessary(metric, metricProducer);
    return metricProducer;
}

namespace {  // anonymous namespace
bool hasClientAggregatedBins(const ValueMetric& metric, int binConfigIndex) {
    return metric.histogram_bin_configs_size() > binConfigIndex &&
           metric.histogram_bin_configs(binConfigIndex).has_client_aggregated_bins();
}

optional<InvalidConfigReason> validatePositionAllInValueFields(
        const ValueMetric& metric, int binConfigIndex, ValueMetric::AggregationType aggType,
        vector<Matcher>::iterator matchersStartIt, const vector<Matcher>::iterator& matchersEndIt) {
    if (aggType == ValueMetric::HISTOGRAM && hasClientAggregatedBins(metric, binConfigIndex)) {
        while (matchersStartIt != matchersEndIt) {
            if (!matchersStartIt->hasAllPositionMatcher()) {
                ALOGE("value_field requires position ALL for client-aggregated histograms. "
                      "ValueMetric \"%lld\"",
                      (long long)metric.id());
                return InvalidConfigReason(
                        INVALID_CONFIG_REASON_VALUE_METRIC_HIST_CLIENT_AGGREGATED_NO_POSITION_ALL,
                        metric.id());
            }
            matchersStartIt++;
        }
        return nullopt;
    }
    while (matchersStartIt != matchersEndIt) {
        if (matchersStartIt->hasAllPositionMatcher()) {
            ALOGE("value_field with position ALL is only supported for client-aggregated "
                  "histograms. ValueMetric \"%lld\"",
                  (long long)metric.id());
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_VALUE_METRIC_VALUE_FIELD_HAS_POSITION_ALL, metric.id());
        }
        matchersStartIt++;
    }
    return nullopt;
}
}  // anonymous namespace

optional<InvalidConfigReason> isNewNumericValueMetricValid(
        const StatsdConfig& config, const ValueMetric& metric,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason = checkCommonMetricFields(
            config, metric, atomMatchingTrackerMap, conditionTrackerMap, stateAtomIdMap,
            metricToActivationMap, atomsAllowedFromAnyUid, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }
    if (!metric.has_value_field()) {
        ALOGE("cannot find \"value_field\" in ValueMetric \"%lld\"", (long long)metric.id());
        return InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_MISSING_VALUE_FIELD,
                                   metric.id());
    }
    std::vector<Matcher> fieldMatchers;
    translateFieldMatcher(metric.value_field(), &fieldMatchers);
    if (fieldMatchers.size() < 1) {
        ALOGE("incorrect \"value_field\" in ValueMetric \"%lld\"", (long long)metric.id());
        return InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HAS_INCORRECT_VALUE_FIELD,
                                   metric.id());
    }

    std::vector<ValueMetric::AggregationType> aggregationTypes;
    int histogramCount = 0;
    if (!metric.aggregation_types().empty()) {
        if (metric.has_aggregation_type()) {
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_VALUE_METRIC_DEFINES_SINGLE_AND_MULTIPLE_AGG_TYPES,
                    metric.id());
        }
        if (metric.aggregation_types_size() != (int)fieldMatchers.size()) {
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_VALUE_METRIC_AGG_TYPES_DNE_VALUE_FIELDS_SIZE,
                    metric.id());
        }
        for (int i = 0; i < metric.aggregation_types_size(); i++) {
            const ValueMetric::AggregationType aggType = metric.aggregation_types(i);
            aggregationTypes.push_back(aggType);
            if (aggType == ValueMetric::HISTOGRAM) {
                histogramCount++;
            }
            invalidConfigReason = validatePositionAllInValueFields(
                    metric, histogramCount - 1, aggType, fieldMatchers.begin() + i,
                    fieldMatchers.begin() + i + 1);
            if (invalidConfigReason != nullopt) {
                return invalidConfigReason;
            }
        }
    } else {  // aggregation_type() is set or default is used.
        const ValueMetric::AggregationType aggType = metric.aggregation_type();
        aggregationTypes.push_back(aggType);
        if (aggType == ValueMetric::HISTOGRAM) {
            histogramCount = 1;
        }
        invalidConfigReason = validatePositionAllInValueFields(
                metric, 0, aggType, fieldMatchers.begin(), fieldMatchers.end());
        if (invalidConfigReason != nullopt) {
            return invalidConfigReason;
        }
    }

    if (metric.histogram_bin_configs_size() != histogramCount) {
        ALOGE("%d histogram aggregations specified but there are %d histogram_bin_configs",
              histogramCount, metric.histogram_bin_configs_size());
        return InvalidConfigReason(
                INVALID_CONFIG_REASON_VALUE_METRIC_HIST_COUNT_DNE_HIST_BIN_CONFIGS_COUNT,
                metric.id());
    }

    if (aggregationTypes.front() == ValueMetric::HISTOGRAM && metric.has_threshold()) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_WITH_UPLOAD_THRESHOLD,
                                   metric.id());
    }

    if (histogramCount > 0 && metric.has_value_direction() &&
        metric.value_direction() != ValueMetric::INCREASING) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_INVALID_VALUE_DIRECTION,
                                   metric.id());
    }

    ParseHistogramBinConfigsResult parseBinConfigsResult =
            parseHistogramBinConfigs(metric, aggregationTypes);
    if (std::holds_alternative<InvalidConfigReason>(parseBinConfigsResult)) {
        return std::get<InvalidConfigReason>(parseBinConfigsResult);
    }

    invalidConfigReason = checkMetricAtomMatchingTrackers(metric.what(), metric.id(), true,
                                                          allAtomMatchingTrackers,
                                                          atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    // Check that all metric state links are a subset of dimensions_in_what fields.
    std::vector<Matcher> dimensionsInWhat;
    translateFieldMatcher(metric.dimensions_in_what(), &dimensionsInWhat);
    for (const auto& stateLink : metric.state_link()) {
        invalidConfigReason =
                checkMetricWithStateLink(metric.id(), stateLink.fields_in_what(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            ALOGW("ValueMetric's MetricStateLinks must be a subset of the dimensions in what");
            return invalidConfigReason;
        }
    }

    if (metric.has_dimensional_sampling_info()) {
        invalidConfigReason = checkMetricWithDimensionalSampling(
                metric.id(), metric.dimensional_sampling_info(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return nullopt;
}

sp<MetricProducer> createNumericValueMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const ValueMetric& metric, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const sp<EventMatcherWizard>& matcherWizard,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider) {
    std::vector<Matcher> fieldMatchers;
    translateFieldMatcher(metric.value_field(), &fieldMatchers);

    std::vector<ValueMetric::AggregationType> aggregationTypes;
    if (!metric.aggregation_types().empty()) {
        for (int i = 0; i < metric.aggregation_types_size(); i++) {
            aggregationTypes.push_back(metric.aggregation_types(i));
        }
    } else {  // aggregation_type() is set or default is used.
        aggregationTypes.push_back(metric.aggregation_type());
    }

    ParseHistogramBinConfigsResult parseBinConfigsResult =
            parseHistogramBinConfigs(metric, aggregationTypes);

    int trackerIndex;
    handleMetricWithAtomMatchingTrackers(metric.what(), metricIndex, atomMatchingTrackerMap,
                                         trackerToMetricMap, trackerIndex);

    const sp<AtomMatchingTracker>& atomMatcher = allAtomMatchingTrackers.at(trackerIndex);
    int atomTagId = *(atomMatcher->getAtomIds().begin());
    int pullTagId = pullerManager->PullerForMatcherExists(atomTagId) ? atomTagId : -1;

    int conditionIndex = -1;
    if (metric.has_condition()) {
        handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                   conditionIndex, conditionToMetricMap);
    }

    std::vector<int> slicedStateAtoms;
    unordered_map<int, unordered_map<int, int64_t>> stateGroupMap;
    if (metric.slice_by_state_size() > 0) {
        handleMetricWithStates(metric.slice_by_state(), stateAtomIdMap, allStateGroupMaps,
                               slicedStateAtoms, stateGroupMap);
    }

    unordered_map<int, shared_ptr<Activation>> eventActivationMap;
    unordered_map<int, vector<shared_ptr<Activation>>> eventDeactivationMap;
    handleMetricActivation(config, metric.id(), metricIndex, metricToActivationMap,
                           atomMatchingTrackerMap, activationAtomTrackerToMetricMap,
                           deactivationAtomTrackerToMetricMap, metricsWithActivation,
                           eventActivationMap, eventDeactivationMap);

    uint64_t metricHash;
    getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);

    const TimeUnit bucketSizeTimeUnit =
            metric.bucket() == TIME_UNIT_UNSPECIFIED ? ONE_HOUR : metric.bucket();
    const int64_t bucketSizeNs =
            MillisToNano(TimeUnitToBucketSizeInMillisGuardrailed(key.GetUid(), bucketSizeTimeUnit));

    const bool containsAnyPositionInDimensionsInWhat = HasPositionANY(metric.dimensions_in_what());
    const bool shouldUseNestedDimensions = ShouldUseNestedDimensions(metric.dimensions_in_what());

    const auto [dimensionSoftLimit, dimensionHardLimit] =
            StatsdStats::getAtomDimensionKeySizeLimits(
                    pullTagId,
                    StatsdStats::clampDimensionKeySizeLimit(metric.max_dimensions_per_bucket()));

    // get the condition_correction_threshold_nanos value
    const optional<int64_t> conditionCorrectionThresholdNs =
            metric.has_condition_correction_threshold_nanos()
                    ? optional<int64_t>(metric.condition_correction_threshold_nanos())
                    : nullopt;

    const vector<optional<const BinStarts>>& binStartsList =
            std::get<vector<optional<const BinStarts>>>(parseBinConfigsResult);
    sp<MetricProducer> metricProducer = new NumericValueMetricProducer(
            key, metric, metricHash, {pullTagId, pullerManager},
            {timeBaseNs, currentTimeNs, bucketSizeNs, metric.min_bucket_size_nanos(),
             conditionCorrectionThresholdNs, getAppUpgradeBucketSplit(metric)},
            {containsAnyPositionInDimensionsInWhat, shouldUseNestedDimensions, trackerIndex,
             matcherWizard, metric.dimensions_in_what(), fieldMatchers, aggregationTypes,
             binStartsList},
            {conditionIndex, metric.links(), initialConditionCache, wizard},
            {metric.state_link(), slicedStateAtoms, stateGroupMap},
            {eventActivationMap, eventDeactivationMap}, {dimensionSoftLimit, dimensionHardLimit},
            configMetadataProvider);

    SamplingInfo samplingInfo;
    if (metric.has_dimensional_sampling_info()) {
        handleMetricWithDimensionalSampling(metric.dimensional_sampling_info(), samplingInfo);
        metricProducer->setSamplingInfo(samplingInfo);
    }

    setUidFieldsIfNecessary(metric, metricProducer);

    return metricProducer;
}

optional<InvalidConfigReason> isNewKllMetricValid(
        const StatsdConfig& config, const KllMetric& metric,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason = checkCommonMetricFields(
            config, metric, atomMatchingTrackerMap, conditionTrackerMap, stateAtomIdMap,
            metricToActivationMap, atomsAllowedFromAnyUid, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (!metric.has_kll_field()) {
        ALOGE("cannot find \"kll_field\" in KllMetric \"%lld\"", (long long)metric.id());
        return InvalidConfigReason(INVALID_CONFIG_REASON_KLL_METRIC_MISSING_KLL_FIELD, metric.id());
    }
    if (HasPositionALL(metric.kll_field())) {
        ALOGE("kll field with position ALL is not supported. KllMetric \"%lld\"",
              (long long)metric.id());
        return InvalidConfigReason(INVALID_CONFIG_REASON_KLL_METRIC_KLL_FIELD_HAS_POSITION_ALL,
                                   metric.id());
    }
    std::vector<Matcher> fieldMatchers;
    translateFieldMatcher(metric.kll_field(), &fieldMatchers);
    if (fieldMatchers.empty()) {
        ALOGE("incorrect \"kll_field\" in KllMetric \"%lld\"", (long long)metric.id());
        return InvalidConfigReason(INVALID_CONFIG_REASON_KLL_METRIC_HAS_INCORRECT_KLL_FIELD,
                                   metric.id());
    }

    invalidConfigReason = checkMetricAtomMatchingTrackers(metric.what(), metric.id(), true,
                                                          allAtomMatchingTrackers,
                                                          atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    // Check that all metric state links are a subset of dimensions_in_what fields.
    std::vector<Matcher> dimensionsInWhat;
    translateFieldMatcher(metric.dimensions_in_what(), &dimensionsInWhat);
    for (const auto& stateLink : metric.state_link()) {
        invalidConfigReason =
                checkMetricWithStateLink(metric.id(), stateLink.fields_in_what(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            ALOGW("KllMetric's MetricStateLinks must be a subset of the dimensions in what");
            return nullopt;
        }
    }

    if (metric.has_dimensional_sampling_info()) {
        invalidConfigReason = checkMetricWithDimensionalSampling(
                metric.id(), metric.dimensional_sampling_info(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return nullopt;
}

sp<MetricProducer> createKllMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const KllMetric& metric, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const sp<EventMatcherWizard>& matcherWizard,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider) {
    std::vector<Matcher> fieldMatchers;
    translateFieldMatcher(metric.kll_field(), &fieldMatchers);

    int trackerIndex;
    handleMetricWithAtomMatchingTrackers(metric.what(), metricIndex, atomMatchingTrackerMap,
                                         trackerToMetricMap, trackerIndex);

    int conditionIndex = -1;
    if (metric.has_condition()) {
        handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                   conditionIndex, conditionToMetricMap);
    }

    std::vector<int> slicedStateAtoms;
    unordered_map<int, unordered_map<int, int64_t>> stateGroupMap;
    if (metric.slice_by_state_size() > 0) {
        handleMetricWithStates(metric.slice_by_state(), stateAtomIdMap, allStateGroupMaps,
                               slicedStateAtoms, stateGroupMap);
    }

    unordered_map<int, shared_ptr<Activation>> eventActivationMap;
    unordered_map<int, vector<shared_ptr<Activation>>> eventDeactivationMap;
    handleMetricActivation(config, metric.id(), metricIndex, metricToActivationMap,
                           atomMatchingTrackerMap, activationAtomTrackerToMetricMap,
                           deactivationAtomTrackerToMetricMap, metricsWithActivation,
                           eventActivationMap, eventDeactivationMap);

    uint64_t metricHash;
    getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);

    const TimeUnit bucketSizeTimeUnit =
            metric.bucket() == TIME_UNIT_UNSPECIFIED ? ONE_HOUR : metric.bucket();
    const int64_t bucketSizeNs =
            MillisToNano(TimeUnitToBucketSizeInMillisGuardrailed(key.GetUid(), bucketSizeTimeUnit));

    const bool containsAnyPositionInDimensionsInWhat = HasPositionANY(metric.dimensions_in_what());
    const bool shouldUseNestedDimensions = ShouldUseNestedDimensions(metric.dimensions_in_what());

    const sp<AtomMatchingTracker>& atomMatcher = allAtomMatchingTrackers.at(trackerIndex);
    const int atomTagId = *(atomMatcher->getAtomIds().begin());
    const auto [dimensionSoftLimit, dimensionHardLimit] =
            StatsdStats::getAtomDimensionKeySizeLimits(
                    atomTagId,
                    StatsdStats::clampDimensionKeySizeLimit(metric.max_dimensions_per_bucket()));

    sp<MetricProducer> metricProducer = new KllMetricProducer(
            key, metric, metricHash, {/*pullTagId=*/-1, pullerManager},
            {timeBaseNs, currentTimeNs, bucketSizeNs, metric.min_bucket_size_nanos(),
             /*conditionCorrectionThresholdNs=*/nullopt, getAppUpgradeBucketSplit(metric)},
            {containsAnyPositionInDimensionsInWhat,
             shouldUseNestedDimensions,
             trackerIndex,
             matcherWizard,
             metric.dimensions_in_what(),
             fieldMatchers,
             {}},
            {conditionIndex, metric.links(), initialConditionCache, wizard},
            {metric.state_link(), slicedStateAtoms, stateGroupMap},
            {eventActivationMap, eventDeactivationMap}, {dimensionSoftLimit, dimensionHardLimit},
            configMetadataProvider);

    SamplingInfo samplingInfo;
    if (metric.has_dimensional_sampling_info()) {
        handleMetricWithDimensionalSampling(metric.dimensional_sampling_info(), samplingInfo);
        metricProducer->setSamplingInfo(samplingInfo);
    }

    setUidFieldsIfNecessary(metric, metricProducer);
    return metricProducer;
}

optional<InvalidConfigReason> isNewGaugeMetricValid(
        const StatsdConfig& config, const GaugeMetric& metric,
        const sp<StatsPullerManager>& pullerManager,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const unordered_map<int64_t, int>& stateAtomIdMap,
        const unordered_map<int64_t, int>& metricToActivationMap,
        const set<int>& atomsAllowedFromAnyUid,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason = checkCommonMetricFields(
            config, metric, atomMatchingTrackerMap, conditionTrackerMap, stateAtomIdMap,
            metricToActivationMap, atomsAllowedFromAnyUid, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (metric.has_gauge_fields_filter()) {
        const FieldFilter& filter = metric.gauge_fields_filter();
        if ((filter.has_fields() && !hasLeafNode(filter.fields())) ||
            (filter.has_omit_fields() && !hasLeafNode(filter.omit_fields()))) {
            ALOGW("Incorrect field filter setting in GaugeMetric %lld", (long long)metric.id());
            return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_FIELD_FILTER,
                                       metric.id());
        }
    }

    invalidConfigReason = checkMetricAtomMatchingTrackers(metric.what(), metric.id(), true,
                                                          allAtomMatchingTrackers,
                                                          atomMatchingTrackerMap, invalidEntities);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    const int trackerIndex = atomMatchingTrackerMap.at(metric.what());
    const sp<AtomMatchingTracker>& atomMatcher = allAtomMatchingTrackers.at(trackerIndex);
    int atomTagId = *(atomMatcher->getAtomIds().begin());
    int pullTagId = pullerManager->PullerForMatcherExists(atomTagId) ? atomTagId : -1;

    if (metric.has_trigger_event()) {
        if (pullTagId == -1) {
            ALOGW("Pull atom not specified for trigger");
            return InvalidConfigReason(INVALID_CONFIG_REASON_GAUGE_METRIC_TRIGGER_NO_PULL_ATOM,
                                       metric.id());
        }
        // trigger_event should be used with FIRST_N_SAMPLES
        if (metric.sampling_type() != GaugeMetric::FIRST_N_SAMPLES) {
            ALOGW("Gauge Metric with trigger event must have sampling type FIRST_N_SAMPLES");
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_GAUGE_METRIC_TRIGGER_NO_FIRST_N_SAMPLES, metric.id());
        }
        invalidConfigReason = checkMetricAtomMatchingTrackers(
                metric.trigger_event(), metric.id(), true, allAtomMatchingTrackers,
                atomMatchingTrackerMap, invalidEntities);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    if (pullTagId != -1 && metric.sampling_percentage() != 100) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_GAUGE_METRIC_PULLED_WITH_SAMPLING,
                                   metric.id());
    }

    if (metric.sampling_percentage() < 1 || metric.sampling_percentage() > 100) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_SAMPLING_PERCENTAGE,
                                   metric.id());
    }

    if (metric.pull_probability() < 1 || metric.pull_probability() > 100) {
        return InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_PULL_PROBABILITY,
                                   metric.id());
    }

    if (metric.pull_probability() != 100) {
        if (pullTagId == -1) {
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_GAUGE_METRIC_PUSHED_WITH_PULL_PROBABILITY, metric.id());
        }
        if (metric.sampling_type() == GaugeMetric::RANDOM_ONE_SAMPLE) {
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_GAUGE_METRIC_RANDOM_ONE_SAMPLE_WITH_PULL_PROBABILITY,
                    metric.id());
        }
    }

    std::vector<Matcher> dimensionsInWhat;
    translateFieldMatcher(metric.dimensions_in_what(), &dimensionsInWhat);
    if (metric.has_dimensional_sampling_info()) {
        invalidConfigReason = checkMetricWithDimensionalSampling(
                metric.id(), metric.dimensional_sampling_info(), dimensionsInWhat);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }

    return invalidConfigReason;
}

sp<MetricProducer> createGaugeMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const GaugeMetric& metric, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const unordered_map<int64_t, int>& conditionTrackerMap,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const sp<EventMatcherWizard>& matcherWizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider) {
    int trackerIndex;
    handleMetricWithAtomMatchingTrackers(metric.what(), metricIndex, atomMatchingTrackerMap,
                                         trackerToMetricMap, trackerIndex);

    const sp<AtomMatchingTracker>& atomMatcher = allAtomMatchingTrackers.at(trackerIndex);
    int atomTagId = *(atomMatcher->getAtomIds().begin());
    int pullTagId = pullerManager->PullerForMatcherExists(atomTagId) ? atomTagId : -1;

    int triggerTrackerIndex;
    int triggerAtomId = -1;
    if (metric.has_trigger_event()) {
        handleMetricWithAtomMatchingTrackers(metric.trigger_event(), metricIndex,
                                             atomMatchingTrackerMap, trackerToMetricMap,
                                             triggerTrackerIndex);
        const sp<AtomMatchingTracker>& triggerAtomMatcher =
                allAtomMatchingTrackers.at(triggerTrackerIndex);
        triggerAtomId = *(triggerAtomMatcher->getAtomIds().begin());
    }

    int conditionIndex = -1;
    if (metric.has_condition()) {
        handleMetricWithConditions(metric.condition(), metricIndex, conditionTrackerMap,
                                   conditionIndex, conditionToMetricMap);
    }

    std::vector<int> slicedStateAtoms;
    std::unordered_map<int, std::unordered_map<int, int64_t>> stateGroupMap;
    if (metric.slice_by_state_size() > 0) {
        handleMetricWithStates(metric.slice_by_state(), stateAtomIdMap, allStateGroupMaps,
                               slicedStateAtoms, stateGroupMap);
    }

    unordered_map<int, shared_ptr<Activation>> eventActivationMap;
    unordered_map<int, vector<shared_ptr<Activation>>> eventDeactivationMap;
    handleMetricActivation(config, metric.id(), metricIndex, metricToActivationMap,
                           atomMatchingTrackerMap, activationAtomTrackerToMetricMap,
                           deactivationAtomTrackerToMetricMap, metricsWithActivation,
                           eventActivationMap, eventDeactivationMap);

    uint64_t metricHash;
    getMetricProtoHash(config, metric, metric.id(), metricToActivationMap, metricHash);

    const auto [dimensionSoftLimit, dimensionHardLimit] =
            StatsdStats::getAtomDimensionKeySizeLimits(
                    pullTagId,
                    StatsdStats::clampDimensionKeySizeLimit(metric.max_dimensions_per_bucket()));

    sp<MetricProducer> metricProducer = new GaugeMetricProducer(
            key, metric, conditionIndex, initialConditionCache, wizard, metricHash, trackerIndex,
            matcherWizard, pullTagId, triggerAtomId, atomTagId, timeBaseNs, currentTimeNs,
            pullerManager, configMetadataProvider, eventActivationMap, eventDeactivationMap,
            slicedStateAtoms, stateGroupMap, dimensionSoftLimit, dimensionHardLimit);

    SamplingInfo samplingInfo;
    if (metric.has_dimensional_sampling_info()) {
        handleMetricWithDimensionalSampling(metric.dimensional_sampling_info(), samplingInfo);
        metricProducer->setSamplingInfo(samplingInfo);
    }

    setUidFieldsIfNecessary(metric, metricProducer);

    return metricProducer;
}

optional<InvalidConfigReason> isNewAlertValid(
        const Alert& alert, const unordered_map<int64_t, int>& metricProducerMap,
        const vector<sp<MetricProducer>>& allMetricProducers,
        const unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    if (invalidEntities.contains({alert.id(), INVALID_ENTITY_TYPE_ALERT})) {
        return invalidEntities.at({alert.id(), INVALID_ENTITY_TYPE_ALERT});
    }
    if (invalidEntities.contains({alert.metric_id(), INVALID_ENTITY_TYPE_METRIC})) {
        return createInvalidConfigReasonWithAlert(
                INVALID_CONFIG_REASON_ALERT_INVALID_METRIC_DEPENDENCY, alert.metric_id(),
                alert.id());
    }
    const auto& itr = metricProducerMap.find(alert.metric_id());
    if (itr == metricProducerMap.end()) {
        ALOGW("alert \"%lld\" has unknown metric id: \"%lld\"", (long long)alert.id(),
              (long long)alert.metric_id());
        return createInvalidConfigReasonWithAlert(INVALID_CONFIG_REASON_ALERT_METRIC_NOT_FOUND,
                                                  alert.metric_id(), alert.id());
    }
    if (!alert.has_trigger_if_sum_gt()) {
        ALOGW("invalid alert: missing threshold");
        return createInvalidConfigReasonWithAlert(INVALID_CONFIG_REASON_ALERT_THRESHOLD_MISSING,
                                                  alert.id());
    }
    if (alert.trigger_if_sum_gt() < 0 || alert.num_buckets() <= 0) {
        ALOGW("invalid alert: threshold=%f num_buckets= %d", alert.trigger_if_sum_gt(),
              alert.num_buckets());
        return createInvalidConfigReasonWithAlert(
                INVALID_CONFIG_REASON_ALERT_INVALID_TRIGGER_OR_NUM_BUCKETS, alert.id());
    }
    return nullopt;
}

sp<AnomalyTracker> createAnomalyTracker(const Alert& alert,
                                        const sp<AlarmMonitor>& anomalyAlarmMonitor,
                                        const UpdateStatus& updateStatus,
                                        const int64_t currentTimeNs,
                                        const unordered_map<int64_t, int>& metricProducerMap,
                                        vector<sp<MetricProducer>>& allMetricProducers) {
    const auto& itr = metricProducerMap.find(alert.metric_id());
    const int metricIndex = itr->second;
    sp<MetricProducer> metric = allMetricProducers[metricIndex];
    sp<AnomalyTracker> anomalyTracker =
            metric->addAnomalyTracker(alert, anomalyAlarmMonitor, updateStatus, currentTimeNs);
    return {anomalyTracker};
}

bool initAtomMatchingTrackers(
        const StatsdConfig& config, const sp<UidMap>& uidMap,
        unordered_map<int64_t, int>& validAtomMatchingTrackerMap,
        vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        unordered_map<int, vector<int>>& allTagIdsToMatchersMap,
        unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    bool allMatchersValid = true;
    unordered_map<int64_t, AtomMatcherValue> allAtomMatcherMap;
    optional<InvalidConfigReason> invalidConfigReason;

    for (int i = 0; i < config.atom_matcher_size(); i++) {
        const AtomMatcher& logMatcher = config.atom_matcher(i);
        if (allAtomMatcherMap.find(logMatcher.id()) != allAtomMatcherMap.end()) {
            ALOGE("Duplicate AtomMatcher found!");
            allMatchersValid = false;
            invalidEntities[{logMatcher.id(), INVALID_ENTITY_TYPE_MATCHER}] =
                    createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_DUPLICATE,
                                                         logMatcher.id());
            continue;
        }
        sp<AtomMatchingTracker> tracker =
                createAtomMatchingTracker(logMatcher, uidMap, invalidConfigReason);
        if (tracker == nullptr) {
            allMatchersValid = false;
            if (invalidConfigReason.has_value()) {
                invalidEntities[{logMatcher.id(), INVALID_ENTITY_TYPE_MATCHER}] =
                        invalidConfigReason.value();
            } else {
                // Should never happen
                invalidEntities[{logMatcher.id(), INVALID_ENTITY_TYPE_MATCHER}] =
                        createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_UNKNOWN,
                                                             logMatcher.id());
            }
            continue;
        }
        allAtomMatcherMap[logMatcher.id()] = {logMatcher, tracker};
    }

    unordered_set<int64_t> stackTracker;
    for (const auto& [id, atomMatcherValue] : allAtomMatcherMap) {
        stackTracker.clear();
        const auto [invalidReason, _] = atomMatcherValue.atomMatchingTracker->isTrackerValid(
                allAtomMatcherMap, stackTracker);
        if (invalidReason.has_value()) {
            allMatchersValid = false;
            invalidEntities[{id, INVALID_ENTITY_TYPE_MATCHER}] = invalidReason.value();
        }
    }

    // Reserve the maximum amount since invalid entities are rare.
    allAtomMatchingTrackers.reserve(config.atom_matcher_size());
    // Iterate through the matchers from the original config to guarantee consistent order.
    int outIndex = 0;
    for (const auto& matcher : config.atom_matcher()) {
        if (invalidEntities.find({matcher.id(), INVALID_ENTITY_TYPE_MATCHER}) ==
            invalidEntities.end()) {
            validAtomMatchingTrackerMap[matcher.id()] = outIndex;
            allAtomMatchingTrackers.push_back(allAtomMatcherMap[matcher.id()].atomMatchingTracker);
            ++outIndex;
        }
    }

    for (size_t matcherIndex = 0; matcherIndex < allAtomMatchingTrackers.size(); matcherIndex++) {
        auto& matcher = allAtomMatchingTrackers[matcherIndex];
        matcher->init(allAtomMatcherMap, validAtomMatchingTrackerMap);
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

    return allMatchersValid;
}

bool initConditions(const ConfigKey& key, const StatsdConfig& config,
                    const unordered_map<int64_t, int>& atomMatchingTrackerMap,
                    unordered_map<int64_t, int>& conditionTrackerMap,
                    vector<sp<ConditionTracker>>& allConditionTrackers,
                    unordered_map<int, std::vector<int>>& trackerToConditionMap,
                    vector<ConditionState>& initialConditionCache,
                    unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
                    unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    bool allConditionsValid = true;
    optional<InvalidConfigReason> invalidConfigReason;

    for (int i = 0; i < config.predicate_size(); i++) {
        const Predicate& condition = config.predicate(i);
        if (allConditionsMap.find(condition.id()) != allConditionsMap.end()) {
            ALOGE("Duplicate Predicate found!");
            allConditionsValid = false;
            invalidEntities[{condition.id(), INVALID_ENTITY_TYPE_PREDICATE}] =
                    createInvalidConfigReasonWithPredicate(
                            INVALID_CONFIG_REASON_CONDITION_DUPLICATE, condition.id());
            continue;
        }
        sp<ConditionTracker> tracker =
                createConditionTracker(key, condition, invalidEntities, invalidConfigReason);
        if (tracker == nullptr) {
            allConditionsValid = false;
            if (invalidConfigReason.has_value()) {
                invalidEntities[{condition.id(), INVALID_ENTITY_TYPE_PREDICATE}] =
                        invalidConfigReason.value();
            } else {
                invalidEntities[{condition.id(), INVALID_ENTITY_TYPE_PREDICATE}] =
                        createInvalidConfigReasonWithPredicate(INVALID_CONFIG_REASON_UNKNOWN,
                                                               condition.id());
            }
            continue;
        }
        allConditionsMap[condition.id()] = {condition, tracker};
    }

    unordered_set<int64_t> stackTracker;
    for (const auto& [id, conditionValue] : allConditionsMap) {
        stackTracker.clear();
        const auto& invalidReason =
                conditionValue.conditionTracker->isTrackerValid(allConditionsMap, stackTracker);
        if (invalidReason.has_value()) {
            allConditionsValid = false;
            invalidEntities[{id, INVALID_ENTITY_TYPE_PREDICATE}] = invalidReason.value();
        }
    }

    // Reserve the maximum amount since invalid entities are rare.
    allConditionTrackers.reserve(config.predicate_size());
    initialConditionCache.reserve(config.predicate_size());
    // Iterate through the conditions in the original config to guarantee consistent order.
    int outIndex = 0;
    for (const auto& condition : config.predicate()) {
        if (!invalidEntities.contains({condition.id(), INVALID_ENTITY_TYPE_PREDICATE})) {
            conditionTrackerMap[condition.id()] = outIndex;
            allConditionTrackers.push_back(allConditionsMap[condition.id()].conditionTracker);
            initialConditionCache.push_back(ConditionState::kNotEvaluated);
            ++outIndex;
        }
    }

    unordered_set<int64_t> initializedTrackers;
    for (size_t i = 0; i < allConditionTrackers.size(); i++) {
        auto& conditionTracker = allConditionTrackers[i];
        conditionTracker->init(i, allConditionsMap, allConditionTrackers, conditionTrackerMap,
                               atomMatchingTrackerMap, initializedTrackers, initialConditionCache);
        for (const int trackerIndex : conditionTracker->getAtomMatchingTrackerIndex()) {
            auto& conditionList = trackerToConditionMap[trackerIndex];
            conditionList.push_back(i);
        }
    }
    return allConditionsValid;
}

bool initStates(const StatsdConfig& config, unordered_map<int64_t, int>& stateAtomIdMap,
                unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
                map<int64_t, uint64_t>& stateProtoHashes,
                unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    bool allStatesValid = true;
    for (int i = 0; i < config.state_size(); i++) {
        const State& state = config.state(i);
        const int64_t stateId = state.id();
        string serializedState;
        if (!state.SerializeToString(&serializedState)) {
            allStatesValid = false;
            ALOGE("Unable to serialize state %lld", (long long)stateId);
            invalidEntities[{stateId, INVALID_ENTITY_TYPE_STATE}] =
                    createInvalidConfigReasonWithState(
                            INVALID_CONFIG_REASON_STATE_SERIALIZATION_FAILED, state.id(),
                            state.atom_id());
            continue;
        }
        stateProtoHashes[stateId] = Hash64(serializedState);

        stateAtomIdMap[stateId] = state.atom_id();

        const StateMap& stateMap = state.map();
        for (const auto& group : stateMap.group()) {
            for (const auto& value : group.value()) {
                allStateGroupMaps[stateId][value] = group.group_id();
            }
        }
    }

    return allStatesValid;
}

bool initMetrics(const ConfigKey& key, const StatsdConfig& config, const int64_t timeBaseTimeNs,
                 const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
                 const unordered_map<int64_t, int>& atomMatchingTrackerMap,
                 const unordered_map<int64_t, int>& conditionTrackerMap,
                 const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
                 const unordered_map<int64_t, int>& stateAtomIdMap,
                 const unordered_map<int64_t, unordered_map<int, int64_t>>& allStateGroupMaps,
                 const unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
                 vector<sp<ConditionTracker>>& allConditionTrackers,
                 const vector<ConditionState>& initialConditionCache,
                 vector<sp<MetricProducer>>& allMetricProducers,
                 unordered_map<int, vector<int>>& conditionToMetricMap,
                 unordered_map<int, vector<int>>& trackerToMetricMap,
                 unordered_map<int64_t, int>& metricMap, std::set<int64_t>& noReportMetricIds,
                 unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
                 unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
                 vector<int>& metricsWithActivation,
                 const wp<ConfigMetadataProvider> configMetadataProvider,
                 unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    sp<ConditionWizard> wizard = new ConditionWizard(allConditionTrackers);
    sp<EventMatcherWizard> matcherWizard = new EventMatcherWizard(allAtomMatchingTrackers);
    const int allMetricsCount = config.count_metric_size() + config.duration_metric_size() +
                                config.event_metric_size() + config.gauge_metric_size() +
                                config.value_metric_size() + config.kll_metric_size();
    if (config.has_restricted_metrics_delegate_package_name() &&
        allMetricsCount != config.event_metric_size()) {
        ALOGE("Restricted metrics only support event metric");
        invalidEntities[{key.GetId(), INVALID_ENTITY_TYPE_CONFIG}] =
                InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED);
        return false;
    }

    bool allMetricsValid = true;
    // Construct map from metric id to metric activation index. The map will be used to determine
    // the metric activation corresponding to a metric.
    unordered_map<int64_t, int> metricToActivationMap;
    for (int i = 0; i < config.metric_activation_size(); i++) {
        const MetricActivation& metricActivation = config.metric_activation(i);
        int64_t metricId = metricActivation.metric_id();
        if (metricToActivationMap.find(metricId) != metricToActivationMap.end()) {
            ALOGE("Metric %lld has multiple MetricActivations", (long long)metricId);
            allMetricsValid = false;
            invalidEntities[{metricId, INVALID_ENTITY_TYPE_METRIC}] = InvalidConfigReason(
                    INVALID_CONFIG_REASON_METRIC_HAS_MULTIPLE_ACTIVATIONS, metricId);
            continue;
        }
        metricToActivationMap.insert({metricId, i});
    }

    // Reserve the maximum amount because invalid metrics are rare
    allMetricProducers.reserve(allMetricsCount);
    optional<InvalidConfigReason> invalidConfigReason;
    int metricIndex = 0;
    const set<int> atomsAllowedFromAnyUid(config.whitelisted_atom_ids().begin(),
                                          config.whitelisted_atom_ids().end());
    // Build MetricProducers for each metric defined in config.
    // build CountMetricProducer
    for (int i = 0; i < config.count_metric_size(); i++) {
        const CountMetric& metric = config.count_metric(i);
        invalidConfigReason = isNewCountMetricValid(config, metric, allAtomMatchingTrackers,
                                                    atomMatchingTrackerMap, conditionTrackerMap,
                                                    stateAtomIdMap, metricToActivationMap,
                                                    atomsAllowedFromAnyUid, invalidEntities);
        if (invalidConfigReason.has_value()) {
            allMetricsValid = false;
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}] =
                    invalidConfigReason.value();
            continue;
        }
        metricMap.insert({metric.id(), metricIndex});
        sp<MetricProducer> producer = createCountMetricProducerAndUpdateMetadata(
                key, config, timeBaseTimeNs, currentTimeNs, metric, metricIndex,
                atomMatchingTrackerMap, conditionTrackerMap, initialConditionCache, wizard,
                stateAtomIdMap, allStateGroupMaps, metricToActivationMap, trackerToMetricMap,
                conditionToMetricMap, activationAtomTrackerToMetricMap,
                deactivationAtomTrackerToMetricMap, metricsWithActivation, configMetadataProvider);
        allMetricProducers.push_back(producer);
        ++metricIndex;
    }

    // build DurationMetricProducer
    for (int i = 0; i < config.duration_metric_size(); i++) {
        const DurationMetric& metric = config.duration_metric(i);
        invalidConfigReason = isNewDurationMetricValid(
                config, metric, allAtomMatchingTrackers, atomMatchingTrackerMap,
                conditionTrackerMap, stateAtomIdMap, metricToActivationMap, allConditionsMap,
                atomsAllowedFromAnyUid, invalidEntities);
        if (invalidConfigReason.has_value()) {
            allMetricsValid = false;
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}] =
                    invalidConfigReason.value();
            continue;
        }
        metricMap.insert({metric.id(), metricIndex});
        sp<MetricProducer> producer = createDurationMetricProducerAndUpdateMetadata(
                key, config, timeBaseTimeNs, currentTimeNs, metric, metricIndex,
                atomMatchingTrackerMap, conditionTrackerMap, initialConditionCache, wizard,
                stateAtomIdMap, allStateGroupMaps, metricToActivationMap, allConditionsMap,
                trackerToMetricMap, conditionToMetricMap, activationAtomTrackerToMetricMap,
                deactivationAtomTrackerToMetricMap, metricsWithActivation, configMetadataProvider);
        allMetricProducers.push_back(producer);
        ++metricIndex;
    }

    // build EventMetricProducer
    for (int i = 0; i < config.event_metric_size(); i++) {
        const EventMetric& metric = config.event_metric(i);
        invalidConfigReason = isNewEventMetricValid(config, metric, allAtomMatchingTrackers,
                                                    atomMatchingTrackerMap, conditionTrackerMap,
                                                    stateAtomIdMap, metricToActivationMap,
                                                    atomsAllowedFromAnyUid, invalidEntities);
        if (invalidConfigReason.has_value()) {
            ALOGE("event metric is invalid %d", invalidConfigReason.value().reason);
            allMetricsValid = false;
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}] =
                    invalidConfigReason.value();
            continue;
        }
        metricMap.insert({metric.id(), metricIndex});
        sp<MetricProducer> producer = createEventMetricProducerAndUpdateMetadata(
                key, config, timeBaseTimeNs, config.has_restricted_metrics_delegate_package_name(),
                metric, metricIndex, atomMatchingTrackerMap, conditionTrackerMap,
                initialConditionCache, wizard, stateAtomIdMap, allStateGroupMaps,
                metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
                activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                metricsWithActivation, configMetadataProvider);
        allMetricProducers.push_back(producer);
        ++metricIndex;
    }

    // build NumericValueMetricProducer
    for (int i = 0; i < config.value_metric_size(); i++) {
        const ValueMetric& metric = config.value_metric(i);
        invalidConfigReason = isNewNumericValueMetricValid(
                config, metric, allAtomMatchingTrackers, atomMatchingTrackerMap,
                conditionTrackerMap, stateAtomIdMap, metricToActivationMap, atomsAllowedFromAnyUid,
                invalidEntities);
        if (invalidConfigReason.has_value()) {
            allMetricsValid = false;
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}] =
                    invalidConfigReason.value();
            continue;
        }
        metricMap.insert({metric.id(), metricIndex});
        sp<MetricProducer> producer = createNumericValueMetricProducerAndUpdateMetadata(
                key, config, timeBaseTimeNs, currentTimeNs, pullerManager, metric, metricIndex,
                allAtomMatchingTrackers, atomMatchingTrackerMap, conditionTrackerMap,
                initialConditionCache, wizard, matcherWizard, stateAtomIdMap, allStateGroupMaps,
                metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
                activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                metricsWithActivation, configMetadataProvider);
        allMetricProducers.push_back(producer);
        ++metricIndex;
    }

    // build KllMetricProducer
    for (int i = 0; i < config.kll_metric_size(); i++) {
        const KllMetric& metric = config.kll_metric(i);
        invalidConfigReason =
                isNewKllMetricValid(config, metric, allAtomMatchingTrackers, atomMatchingTrackerMap,
                                    conditionTrackerMap, stateAtomIdMap, metricToActivationMap,
                                    atomsAllowedFromAnyUid, invalidEntities);
        if (invalidConfigReason.has_value()) {
            allMetricsValid = false;
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}] =
                    invalidConfigReason.value();
            continue;
        }
        metricMap.insert({metric.id(), metricIndex});
        sp<MetricProducer> producer = createKllMetricProducerAndUpdateMetadata(
                key, config, timeBaseTimeNs, currentTimeNs, pullerManager, metric, metricIndex,
                allAtomMatchingTrackers, atomMatchingTrackerMap, conditionTrackerMap,
                initialConditionCache, wizard, matcherWizard, stateAtomIdMap, allStateGroupMaps,
                metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
                activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                metricsWithActivation, configMetadataProvider);
        allMetricProducers.push_back(producer);
        ++metricIndex;
    }

    // Gauge metrics.
    for (int i = 0; i < config.gauge_metric_size(); i++) {
        const GaugeMetric& metric = config.gauge_metric(i);
        invalidConfigReason = isNewGaugeMetricValid(
                config, metric, pullerManager, allAtomMatchingTrackers, atomMatchingTrackerMap,
                conditionTrackerMap, stateAtomIdMap, metricToActivationMap, atomsAllowedFromAnyUid,
                invalidEntities);
        if (invalidConfigReason.has_value()) {
            allMetricsValid = false;
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}] =
                    invalidConfigReason.value();
            continue;
        }
        metricMap.insert({metric.id(), metricIndex});
        sp<MetricProducer> producer = createGaugeMetricProducerAndUpdateMetadata(
                key, config, timeBaseTimeNs, currentTimeNs, pullerManager, metric, metricIndex,
                allAtomMatchingTrackers, atomMatchingTrackerMap, conditionTrackerMap,
                initialConditionCache, wizard, matcherWizard, stateAtomIdMap, allStateGroupMaps,
                metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
                activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                metricsWithActivation, configMetadataProvider);
        allMetricProducers.push_back(producer);
        ++metricIndex;
    }
    for (int i = 0; i < config.no_report_metric_size(); ++i) {
        const auto no_report_metric = config.no_report_metric(i);
        if (metricMap.find(no_report_metric) == metricMap.end()) {
            ALOGW("no_report_metric %" PRId64 " not exist", no_report_metric);
            // This validity check can happen after metric creation because it only checks for
            // the existence of the metric. Non-validity here does not affect vector indexing.
            allMetricsValid = false;
            invalidEntities[{no_report_metric, INVALID_ENTITY_TYPE_METRIC}] = InvalidConfigReason(
                    INVALID_CONFIG_REASON_NO_REPORT_METRIC_NOT_FOUND, no_report_metric);
            continue;
        }
        noReportMetricIds.insert(no_report_metric);
    }

    for (const auto& it : allMetricProducers) {
        // Register metrics to StateTrackers
        for (int atomId : it->getSlicedStateAtoms()) {
            StateManager::getInstance().registerListener(atomId, it);
        }
    }
    return allMetricsValid;
}

bool initAlerts(const StatsdConfig& config, const int64_t currentTimeNs,
                const unordered_map<int64_t, int>& metricProducerMap,
                unordered_map<int64_t, int>& alertTrackerMap,
                const sp<AlarmMonitor>& anomalyAlarmMonitor,
                vector<sp<MetricProducer>>& allMetricProducers,
                vector<sp<AnomalyTracker>>& allAnomalyTrackers,
                unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    optional<InvalidConfigReason> invalidConfigReason;
    bool allAlertsValid = true;
    for (int i = 0; i < config.alert_size(); i++) {
        const Alert& alert = config.alert(i);
        invalidConfigReason =
                isNewAlertValid(alert, metricProducerMap, allMetricProducers, invalidEntities);
        if (invalidConfigReason.has_value()) {
            invalidEntities[{alert.id(), INVALID_ENTITY_TYPE_ALERT}] = invalidConfigReason.value();
            allAlertsValid = false;
            continue;
        }
        alertTrackerMap.insert(std::make_pair(alert.id(), allAnomalyTrackers.size()));
        sp<AnomalyTracker> anomalyTracker =
                createAnomalyTracker(alert, anomalyAlarmMonitor, UpdateStatus::UPDATE_NEW,
                                     currentTimeNs, metricProducerMap, allMetricProducers);
        allAnomalyTrackers.push_back(anomalyTracker);
    }

    allAlertsValid &= initSubscribersForSubscriptionType(
            config, Subscription::ALERT, alertTrackerMap, allAnomalyTrackers, invalidEntities);

    return allAlertsValid;
}

bool initAlarms(const StatsdConfig& config, const ConfigKey& key,
                const sp<AlarmMonitor>& periodicAlarmMonitor, const int64_t timeBaseNs,
                const int64_t currentTimeNs, unordered_map<int64_t, int>& alarmTrackerMap,
                vector<sp<AlarmTracker>>& allAlarmTrackers,
                unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    int64_t startMillis = timeBaseNs / 1000 / 1000;
    int64_t currentTimeMillis = currentTimeNs / 1000 / 1000;
    bool allAlarmsValid = true;
    for (int i = 0; i < config.alarm_size(); i++) {
        const Alarm& alarm = config.alarm(i);
        if (alarm.offset_millis() <= 0) {
            ALOGW("Alarm offset_millis should be larger than 0.");
            invalidEntities[{alarm.id(), INVALID_ENTITY_TYPE_ALARM}] =
                    createInvalidConfigReasonWithAlarm(
                            INVALID_CONFIG_REASON_ALARM_OFFSET_LESS_THAN_OR_EQUAL_ZERO, alarm.id());
            allAlarmsValid = false;
            continue;
        }
        if (alarm.period_millis() <= 0) {
            ALOGW("Alarm period_millis should be larger than 0.");
            invalidEntities[{alarm.id(), INVALID_ENTITY_TYPE_ALARM}] =
                    createInvalidConfigReasonWithAlarm(
                            INVALID_CONFIG_REASON_ALARM_PERIOD_LESS_THAN_OR_EQUAL_ZERO, alarm.id());
            allAlarmsValid = false;
            continue;
        }
        alarmTrackerMap.insert(std::make_pair(alarm.id(), allAlarmTrackers.size()));
        allAlarmTrackers.push_back(
                new AlarmTracker(startMillis, currentTimeMillis, alarm, key, periodicAlarmMonitor));
    }

    allAlarmsValid &= initSubscribersForSubscriptionType(
            config, Subscription::ALARM, alarmTrackerMap, allAlarmTrackers, invalidEntities);

    return allAlarmsValid;
}

unordered_map<InvalidEntityKey, InvalidConfigReason> initStatsdConfig(
        const ConfigKey& key, const StatsdConfig& config, const sp<UidMap>& uidMap,
        const sp<StatsPullerManager>& pullerManager, const sp<AlarmMonitor>& anomalyAlarmMonitor,
        const sp<AlarmMonitor>& periodicAlarmMonitor, const int64_t timeBaseNs,
        const int64_t currentTimeNs, const wp<ConfigMetadataProvider> configMetadataProvider,
        std::unordered_map<int, std::vector<int>>& allTagIdsToMatchersMap,
        vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        unordered_map<int64_t, int>& atomMatchingTrackerMap,
        vector<sp<ConditionTracker>>& allConditionTrackers,
        unordered_map<int64_t, int>& conditionTrackerMap,
        vector<sp<MetricProducer>>& allMetricProducers,
        unordered_map<int64_t, int>& metricProducerMap,
        vector<sp<AnomalyTracker>>& allAnomalyTrackers,
        vector<sp<AlarmTracker>>& allPeriodicAlarmTrackers,
        unordered_map<int, std::vector<int>>& conditionToMetricMap,
        unordered_map<int, std::vector<int>>& trackerToMetricMap,
        unordered_map<int, std::vector<int>>& trackerToConditionMap,
        unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        unordered_map<int64_t, int>& alertTrackerMap, vector<int>& metricsWithActivation,
        map<int64_t, uint64_t>& stateProtoHashes, set<int64_t>& noReportMetricIds) {
    vector<ConditionState> initialConditionCache;
    unordered_map<int64_t, int> stateAtomIdMap;
    unordered_map<int64_t, unordered_map<int, int64_t>> allStateGroupMaps;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    if (config.package_certificate_hash_size_bytes() > UINT8_MAX) {
        ALOGE("Invalid value for package_certificate_hash_size_bytes: %d",
              config.package_certificate_hash_size_bytes());
        invalidEntities[{key.GetId(), INVALID_ENTITY_TYPE_CONFIG}] =
                InvalidConfigReason(INVALID_CONFIG_REASON_PACKAGE_CERT_HASH_SIZE_TOO_LARGE);
        return invalidEntities;
    }

    bool allMatchersValid = initAtomMatchingTrackers(config, uidMap, atomMatchingTrackerMap,
                                                     allAtomMatchingTrackers,
                                                     allTagIdsToMatchersMap, invalidEntities);
    if (!allMatchersValid) {
        ALOGE("initAtomMatchingTrackers has invalid matchers");
    }
    VLOG("initAtomMatchingTrackers succeed...");

    optional<InvalidConfigReason> invalidConfigReason;
    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    bool allConditionsValid = initConditions(
            key, config, atomMatchingTrackerMap, conditionTrackerMap, allConditionTrackers,
            trackerToConditionMap, initialConditionCache, allConditionsMap, invalidEntities);
    if (!allConditionsValid) {
        ALOGE("initConditionTrackers failed");
    }

    bool allStatesValid = initStates(config, stateAtomIdMap, allStateGroupMaps, stateProtoHashes,
                                     invalidEntities);
    if (!allStatesValid) {
        ALOGE("initStates failed");
    }

    bool allMetricsValid = initMetrics(
            key, config, timeBaseNs, currentTimeNs, pullerManager, atomMatchingTrackerMap,
            conditionTrackerMap, allAtomMatchingTrackers, stateAtomIdMap, allStateGroupMaps,
            allConditionsMap, allConditionTrackers, initialConditionCache, allMetricProducers,
            conditionToMetricMap, trackerToMetricMap, metricProducerMap, noReportMetricIds,
            activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
            metricsWithActivation, configMetadataProvider, invalidEntities);
    if (!allMetricsValid) {
        ALOGE("initMetricProducers failed");
    }

    bool allAlertsValid = initAlerts(config, currentTimeNs, metricProducerMap, alertTrackerMap,
                                     anomalyAlarmMonitor, allMetricProducers, allAnomalyTrackers,
                                     invalidEntities);
    if (!allAlertsValid) {
        ALOGE("initAlerts failed");
    }

    unordered_map<int64_t, int> alarmTrackerMap;
    bool allAlarmsValid = initAlarms(config, key, periodicAlarmMonitor, timeBaseNs, currentTimeNs,
                                     alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);
    if (!allAlarmsValid) {
        ALOGE("initAlarms failed");
    }

    return invalidEntities;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
