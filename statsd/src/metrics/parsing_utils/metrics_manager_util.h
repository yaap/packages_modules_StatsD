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

#pragma once

#include <utils/JenkinsHash.h>

#include <set>
#include <unordered_map>
#include <vector>

#include "anomaly/AlarmTracker.h"
#include "condition/ConditionTracker.h"
#include "config/ConfigMetadataProvider.h"
#include "external/StatsPullerManager.h"
#include "matchers/AtomMatchingTracker.h"
#include "metrics/MetricProducer.h"

namespace android {
namespace os {
namespace statsd {

// Helper functions for creating, validating, and updating config components from StatsdConfig.
// Should only be called from metrics_manager_util and config_update_utils.

// Create a AtomMatchingTracker.
// input:
// [logMatcher]: the input AtomMatcher from the StatsdConfig
// [invalidConfigReason]: logging ids if config is invalid
// output:
// new AtomMatchingTracker, or null if the tracker is unable to be created
sp<AtomMatchingTracker> createAtomMatchingTracker(
        const AtomMatcher& logMatcher, const sp<UidMap>& uidMap,
        std::optional<InvalidConfigReason>& invalidConfigReason);

// Create a ConditionTracker.
// input:
// [predicate]: the input Predicate from the StatsdConfig
// [index]: the index of the condition tracker
// [invalidConfigReason]: logging ids if config is invalid
// output:
// new ConditionTracker, or null if the tracker is unable to be created
sp<ConditionTracker> createConditionTracker(
        const ConfigKey& key, const Predicate& predicate,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities,
        std::optional<InvalidConfigReason>& invalidConfigReason);

// Get the hash of a metric, combining the activation if the metric has one.
std::optional<InvalidConfigReason> getMetricProtoHash(
        const StatsdConfig& config, const google::protobuf::MessageLite& metric, int64_t id,
        const std::unordered_map<int64_t, int>& metricToActivationMap, uint64_t& metricHash);

// Gets matcher index and updates tracker to metric map
void handleMetricWithAtomMatchingTrackers(
        const int64_t matcherId, int metricIndex,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap, int& logTrackerIndex);

// Gets condition index and updates condition to metric map
void handleMetricWithConditions(const int64_t condition, int metricIndex,
                                const std::unordered_map<int64_t, int>& conditionTrackerMap,
                                int& conditionIndex,
                                std::unordered_map<int, std::vector<int>>& conditionToMetricMap);

// Validates a metricActivation.
std::optional<InvalidConfigReason> checkMetricActivationOnConfigUpdate(
        const StatsdConfig& config, const int64_t metricId,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const std::unordered_map<int, std::shared_ptr<Activation>>& oldEventActivationMap);

// Fills the new event activation/deactivation maps, preserving the existing activations.
void handleMetricActivationOnConfigUpdate(
        const StatsdConfig& config, const int64_t metricId, const int metricIndex,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const std::unordered_map<int, std::shared_ptr<Activation>>& oldEventActivationMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        std::unordered_map<int, std::shared_ptr<Activation>>& newEventActivationMap,
        std::unordered_map<int, std::vector<std::shared_ptr<Activation>>>& newEventDeactivationMap);

// Determines whether the duration metric from the config is valid.
std::optional<InvalidConfigReason> isNewCountMetricValid(
        const StatsdConfig& config, const CountMetric& metric,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::set<int>& atomsAllowedFromAnyUid,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates a CountMetricProducer and updates the vectors/maps used by MetricsManager with
// the appropriate indices. Returns an sp to the producer
sp<MetricProducer> createCountMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseNs,
        const int64_t currentTimeNs, const CountMetric& metric, int metricIndex,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider);

// Determines whether the duration metric from the config is valid.
std::optional<InvalidConfigReason> isNewDurationMetricValid(
        const StatsdConfig& config, const DurationMetric& metric,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        const std::set<int>& atomsAllowedFromAnyUid,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates a DurationMetricProducer and updates the vectors/maps used by MetricsManager with
// the appropriate indices. Returns an sp to the producer.
sp<MetricProducer> createDurationMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseNs,
        const int64_t currentTimeNs, const DurationMetric& metric, int metricIndex,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider);

std::optional<InvalidConfigReason> isNewEventMetricValid(
        const StatsdConfig& config, const EventMetric& metric,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::set<int>& atomsAllowedFromAnyUid,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates an EventMetricProducer and updates the vectors/maps used by MetricsManager with
// the appropriate indices.
sp<MetricProducer> createEventMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseNs,
        const bool isRestrictedMetric, const EventMetric& metric, int metricIndex,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider);

std::optional<InvalidConfigReason> isNewNumericValueMetricValid(
        const StatsdConfig& config, const ValueMetric& metric,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::set<int>& atomsAllowedFromAnyUid,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates a NumericValueMetricProducer and updates the vectors/maps used by MetricsManager with
// the appropriate indices.
sp<MetricProducer> createNumericValueMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const ValueMetric& metric, int metricIndex,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const sp<EventMatcherWizard>& matcherWizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider);

std::optional<InvalidConfigReason> isNewGaugeMetricValid(
        const StatsdConfig& config, const GaugeMetric& metric,
        const sp<StatsPullerManager>& pullerManager,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::set<int>& atomsAllowedFromAnyUid,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates a GaugeMetricProducer and updates the vectors/maps used by MetricsManager with
// the appropriate indices.
sp<MetricProducer> createGaugeMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const GaugeMetric& metric, int metricIndex,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const sp<EventMatcherWizard>& matcherWizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider);

std::optional<InvalidConfigReason> isNewKllMetricValid(
        const StatsdConfig& config, const KllMetric& metric,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        const std::set<int>& atomsAllowedFromAnyUid,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates a KllMetricProducer and updates the vectors/maps used by MetricsManager with
// the appropriate indices.
sp<MetricProducer> createKllMetricProducerAndUpdateMetadata(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const KllMetric& metric, int metricIndex,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const sp<EventMatcherWizard>& matcherWizard,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, int>& metricToActivationMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider);

// Checks whether the alert is valid
std::optional<InvalidConfigReason> isNewAlertValid(
        const Alert& alert, const std::unordered_map<int64_t, int>& metricProducerMap,
        const std::vector<sp<MetricProducer>>& allMetricProducers,
        const std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Creates an AnomalyTracker and adds it to the appropriate metric.
// Returns an sp to the AnomalyTracker.
sp<AnomalyTracker> createAnomalyTracker(const Alert& alert,
                                        const sp<AlarmMonitor>& anomalyAlarmMonitor,
                                        const UpdateStatus& updateStatus, int64_t currentTimeNs,
                                        const std::unordered_map<int64_t, int>& metricProducerMap,
                                        std::vector<sp<MetricProducer>>& allMetricProducers);

// Templated function for adding subscriptions to alarms or alerts. Returns whether all the
// subscriptions for the ruleType is valid.
template <typename T>
bool initSubscribersForSubscriptionType(
        const StatsdConfig& config, const Subscription_RuleType ruleType,
        const std::unordered_map<int64_t, int>& ruleMap, std::vector<T>& allRules,
        std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    bool allSubscribersValid = true;
    for (int i = 0; i < config.subscription_size(); ++i) {
        const Subscription& subscription = config.subscription(i);
        if (subscription.rule_type() != ruleType) {
            continue;
        }
        if (subscription.subscriber_information_case() ==
            Subscription::SubscriberInformationCase::SUBSCRIBER_INFORMATION_NOT_SET) {
            ALOGW("subscription \"%lld\" has no subscriber info.\"", (long long)subscription.id());
            invalidEntities[{subscription.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}] =
                    createInvalidConfigReasonWithSubscription(
                            INVALID_CONFIG_REASON_SUBSCRIPTION_SUBSCRIBER_INFO_MISSING,
                            subscription.id());
            allSubscribersValid = false;
            continue;
        }
        if (ruleType == Subscription_RuleType_ALERT &&
            invalidEntities.contains({subscription.rule_id(), INVALID_ENTITY_TYPE_ALERT})) {
            invalidEntities[{subscription.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}] =
                    createInvalidConfigReasonWithSubscriptionAndAlert(
                            INVALID_CONFIG_REASON_SUBSCRIPTION_INVALID_ALERT_DEPENDENCY,
                            subscription.id(), subscription.rule_id());
            allSubscribersValid = false;
            continue;
        }
        if (ruleType == Subscription_RuleType_ALARM &&
            invalidEntities.contains({subscription.rule_id(), INVALID_ENTITY_TYPE_ALARM})) {
            invalidEntities[{subscription.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}] =
                    createInvalidConfigReasonWithSubscriptionAndAlarm(
                            INVALID_CONFIG_REASON_SUBSCRIPTION_INVALID_ALARM_DEPENDENCY,
                            subscription.id(), subscription.rule_id());
            allSubscribersValid = false;
            continue;
        }
        const auto& itr = ruleMap.find(subscription.rule_id());
        if (itr == ruleMap.end()) {
            ALOGW("subscription \"%lld\" has unknown rule id: \"%lld\"",
                  (long long)subscription.id(), (long long)subscription.rule_id());
            switch (subscription.rule_type()) {
                case Subscription::ALARM:
                    invalidEntities[{subscription.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}] =
                            createInvalidConfigReasonWithSubscriptionAndAlarm(
                                    INVALID_CONFIG_REASON_SUBSCRIPTION_RULE_NOT_FOUND,
                                    subscription.id(), subscription.rule_id());
                    allSubscribersValid = false;
                    continue;
                case Subscription::ALERT:
                    invalidEntities[{subscription.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}] =
                            createInvalidConfigReasonWithSubscriptionAndAlert(
                                    INVALID_CONFIG_REASON_SUBSCRIPTION_RULE_NOT_FOUND,
                                    subscription.id(), subscription.rule_id());
                    allSubscribersValid = false;
                    continue;
                case Subscription::RULE_TYPE_UNSPECIFIED:
                    invalidEntities[{subscription.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}] =
                            createInvalidConfigReasonWithSubscription(
                                    INVALID_CONFIG_REASON_SUBSCRIPTION_RULE_NOT_FOUND,
                                    subscription.id());
                    allSubscribersValid = false;
                    continue;
            }
        }
        const int ruleIndex = itr->second;
        allRules[ruleIndex]->addSubscription(subscription);
    }
    return allSubscribersValid;
}

// Helper functions for MetricsManager to initialize from StatsdConfig.
// *Note*: only initStatsdConfig() should be called from outside.
// All other functions are intermediate
// steps, created to make unit tests easier. And most of the parameters in these
// functions are temporary objects in the initialization phase.

// Initialize the AtomMatchingTrackers.
// input:
// [key]: the config key that this config belongs to
// [config]: the input StatsdConfig
// output:
// [atomMatchingTrackerMap]: this map should contain matcher name to index mapping
// [allAtomMatchingTrackers]: should store the sp to all the AtomMatchingTracker
// [allTagIdsToMatchersMap]: maps of tag ids to atom matchers
// [invalidEntities]: map of entity id to a reason why the entity is invalid
// Returns true if all matchers are valid
bool initAtomMatchingTrackers(
        const StatsdConfig& config, const sp<UidMap>& uidMap,
        std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        std::unordered_map<int, std::vector<int>>& allTagIdsToMatchersMap,
        std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Initialize ConditionTrackers
// input:
// [key]: the config key that this config belongs to
// [config]: the input config
// [atomMatchingTrackerMap]: AtomMatchingTracker name to index mapping from previous step.
// output:
// [conditionTrackerMap]: this map should contain condition name to index mapping
// [allConditionTrackers]: stores the sp to all the ConditionTrackers
// [trackerToConditionMap]: contain the mapping from index of
//                        log tracker to condition trackers that use the log tracker
// [initialConditionCache]: stores the initial conditions for each ConditionTracker
// [allConditionsMap]: stores the condition id to the config predicate and ConditionTracker. Used
//                     for downstream processing.
// [invalidEntities]: a map of entity id to the reason why the entity is invalid
// Returns whether all conditions are valid
bool initConditions(const ConfigKey& key, const StatsdConfig& config,
                    const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
                    std::unordered_map<int64_t, int>& conditionTrackerMap,
                    std::vector<sp<ConditionTracker>>& allConditionTrackers,
                    std::unordered_map<int, std::vector<int>>& trackerToConditionMap,
                    std::vector<ConditionState>& initialConditionCache,
                    std::unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
                    std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Initialize State maps using State protos in the config. These maps will
// eventually be passed to MetricProducers to initialize their state info.
// input:
// [config]: the input config
// output:
// [stateAtomIdMap]: this map should contain the mapping from state ids to atom ids
// [allStateGroupMaps]: this map should contain the mapping from states ids and state
//                      values to state group ids for all states
// [stateProtoHashes]: contains a map of state id to the hash of the State proto from the config
// [invalidEntities]: map of entity id to the reason why it is invalid.
// Returns true if all states are valid.
bool initStates(const StatsdConfig& config, std::unordered_map<int64_t, int>& stateAtomIdMap,
                std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
                std::map<int64_t, uint64_t>& stateProtoHashes,
                std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Initialize MetricProducers.
// input:
// [key]: the config key that this config belongs to
// [config]: the input config
// [timeBaseSec]: start time base for all metrics
// [atomMatchingTrackerMap]: AtomMatchingTracker name to index mapping from previous step.
// [conditionTrackerMap]: condition name to index mapping
// [stateAtomIdMap]: contains the mapping from state ids to atom ids
// [allStateGroupMaps]: contains the mapping from atom ids and state values to
//                      state group ids for all states
// [allConditionsMap] map of condition id to the original predicate from the config.
//                    Used to initialize DurationMetric.
// output:
// [allMetricProducers]: contains the list of sp to the MetricProducers created.
// [conditionToMetricMap]: contains the mapping from condition tracker index to
//                          the list of MetricProducer index
// [trackerToMetricMap]: contains the mapping from log tracker to MetricProducer index.
// [invalidEntities]: map of entity id to the reason why it is invalid.
// Returns whether all metrics are valid
bool initMetrics(
        const ConfigKey& key, const StatsdConfig& config, int64_t timeBaseTimeNs,
        const int64_t currentTimeNs, const sp<StatsPullerManager>& pullerManager,
        const std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        const std::unordered_map<int64_t, int>& conditionTrackerMap,
        const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const std::unordered_map<int64_t, int>& stateAtomIdMap,
        const std::unordered_map<int64_t, std::unordered_map<int, int64_t>>& allStateGroupMaps,
        const std::unordered_map<int64_t, ConditionProtoAndTracker>& allConditionsMap,
        std::vector<sp<ConditionTracker>>& allConditionTrackers,
        const std::vector<ConditionState>& initialConditionCache,
        std::vector<sp<MetricProducer>>& allMetricProducers,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int64_t, int>& metricMap, std::set<int64_t>& noReportMetricIds,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::vector<int>& metricsWithActivation,
        const wp<ConfigMetadataProvider> configMetadataProvider,
        std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Initialize alerts
bool initAlerts(const StatsdConfig& config, const int64_t currentTimeNs,
                const std::unordered_map<int64_t, int>& metricProducerMap,
                std::unordered_map<int64_t, int>& alertTrackerMap,
                const sp<AlarmMonitor>& anomalyAlarmMonitor,
                std::vector<sp<MetricProducer>>& allMetricProducers,
                std::vector<sp<AnomalyTracker>>& allAnomalyTrackers,
                std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Initialize alarms
// Is called both on initialize new configs and config updates since alarms do not have any state.
bool initAlarms(const StatsdConfig& config, const ConfigKey& key,
                const sp<AlarmMonitor>& periodicAlarmMonitor, const int64_t timeBaseNs,
                int64_t currentTimeNs, std::unordered_map<int64_t, int>& alarmTrackerMap,
                std::vector<sp<AlarmTracker>>& allAlarmTrackers,
                std::unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities);

// Initialize MetricsManager from StatsdConfig.
// Parameters are the members of MetricsManager. See MetricsManager for declaration.
std::unordered_map<InvalidEntityKey, InvalidConfigReason> initStatsdConfig(
        const ConfigKey& key, const StatsdConfig& config, const sp<UidMap>& uidMap,
        const sp<StatsPullerManager>& pullerManager, const sp<AlarmMonitor>& anomalyAlarmMonitor,
        const sp<AlarmMonitor>& periodicAlarmMonitor, int64_t timeBaseNs,
        const int64_t currentTimeNs, const wp<ConfigMetadataProvider> configMetadataProvider,
        std::unordered_map<int, std::vector<int>>& allTagIdsToMatchersMap,
        std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        std::unordered_map<int64_t, int>& atomMatchingTrackerMap,
        std::vector<sp<ConditionTracker>>& allConditionTrackers,
        std::unordered_map<int64_t, int>& conditionTrackerMap,
        std::vector<sp<MetricProducer>>& allMetricProducers,
        std::unordered_map<int64_t, int>& metricProducerMap,
        std::vector<sp<AnomalyTracker>>& allAnomalyTrackers,
        std::vector<sp<AlarmTracker>>& allPeriodicAlarmTrackers,
        std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
        std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& trackerToConditionMap,
        std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
        std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
        std::unordered_map<int64_t, int>& alertTrackerMap, std::vector<int>& metricsWithActivation,
        std::map<int64_t, uint64_t>& stateProtoHashes, std::set<int64_t>& noReportMetricIds);

}  // namespace statsd
}  // namespace os
}  // namespace android
