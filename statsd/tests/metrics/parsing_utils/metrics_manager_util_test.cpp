// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/metrics/parsing_utils/metrics_manager_util.h"

#include <com_android_os_statsd_flags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <private/android_filesystem_config.h>
#include <stdio.h>

#include <numeric>
#include <set>
#include <unordered_map>
#include <vector>

#include "src/condition/ConditionTracker.h"
#include "src/matchers/AtomMatchingTracker.h"
#include "src/metrics/CountMetricProducer.h"
#include "src/metrics/DurationMetricProducer.h"
#include "src/metrics/GaugeMetricProducer.h"
#include "src/metrics/KllMetricProducer.h"
#include "src/metrics/MetricProducer.h"
#include "src/metrics/NumericValueMetricProducer.h"
#include "src/state/StateManager.h"
#include "src/statsd_config.pb.h"
#include "tests/metrics/metrics_test_helper.h"
#include "tests/metrics/parsing_utils/parsing_test_utils.h"
#include "tests/statsd_test_util.h"

using namespace testing;
using android::sp;
using android::os::statsd::Predicate;
using std::map;
using std::nullopt;
using std::optional;
using std::set;
using std::shared_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;

namespace flags = com::android::os::statsd::flags;

#ifdef __ANDROID__

namespace android {
namespace os {
namespace statsd {

namespace {

StatsdConfig buildCircleMatchers() {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_IS_ON"));

    SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(SCREEN_STATE_ATOM_ID);
    simpleAtomMatcher->add_field_value_matcher()->set_field(
            1 /*SCREEN_STATE_CHANGE__DISPLAY_STATE*/);
    simpleAtomMatcher->mutable_field_value_matcher(0)->set_eq_int(
            2 /*SCREEN_STATE_CHANGE__DISPLAY_STATE__STATE_ON*/);

    eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_ON_OR_OFF"));

    AtomMatcher_Combination* combination = eventMatcher->mutable_combination();
    combination->set_operation(LogicalOperation::OR);
    combination->add_matcher(StringToId("SCREEN_IS_ON"));
    // Circle dependency
    combination->add_matcher(StringToId("SCREEN_ON_OR_OFF"));

    return config;
}

StatsdConfig buildAlertWithUnknownMetric() {
    StatsdConfig config;
    config.set_id(12345);

    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    CountMetric* metric = config.add_count_metric();
    metric->set_id(3);
    metric->set_what(StringToId("ScreenTurnedOn"));
    metric->set_bucket(ONE_MINUTE);
    metric->mutable_dimensions_in_what()->set_field(SCREEN_STATE_ATOM_ID);
    metric->mutable_dimensions_in_what()->add_child()->set_field(1);

    auto alert = config.add_alert();
    alert->set_id(3);
    alert->set_metric_id(2);
    alert->set_num_buckets(10);
    alert->set_refractory_period_secs(100);
    alert->set_trigger_if_sum_gt(100);
    return config;
}

StatsdConfig buildMissingMatchers() {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_IS_ON"));

    SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(SCREEN_STATE_ATOM_ID);
    simpleAtomMatcher->add_field_value_matcher()->set_field(
            1 /*SCREEN_STATE_CHANGE__DISPLAY_STATE*/);
    simpleAtomMatcher->mutable_field_value_matcher(0)->set_eq_int(
            2 /*SCREEN_STATE_CHANGE__DISPLAY_STATE__STATE_ON*/);

    eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_ON_OR_OFF"));

    AtomMatcher_Combination* combination = eventMatcher->mutable_combination();
    combination->set_operation(LogicalOperation::OR);
    combination->add_matcher(StringToId("SCREEN_IS_ON"));
    // undefined matcher
    combination->add_matcher(StringToId("ABC"));

    return config;
}

StatsdConfig buildMissingPredicate() {
    StatsdConfig config;
    config.set_id(12345);

    CountMetric* metric = config.add_count_metric();
    metric->set_id(3);
    metric->set_what(StringToId("SCREEN_EVENT"));
    metric->set_bucket(ONE_MINUTE);
    metric->set_condition(StringToId("SOME_CONDITION"));

    AtomMatcher* eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_EVENT"));

    SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(2);

    return config;
}

StatsdConfig buildDimensionMetricsWithMultiTags() {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("BATTERY_VERY_LOW"));
    SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(2);

    eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("BATTERY_VERY_VERY_LOW"));
    simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(3);

    eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("BATTERY_LOW"));

    AtomMatcher_Combination* combination = eventMatcher->mutable_combination();
    combination->set_operation(LogicalOperation::OR);
    combination->add_matcher(StringToId("BATTERY_VERY_LOW"));
    combination->add_matcher(StringToId("BATTERY_VERY_VERY_LOW"));

    // Count process state changes, slice by uid, while SCREEN_IS_OFF
    CountMetric* metric = config.add_count_metric();
    metric->set_id(3);
    metric->set_what(StringToId("BATTERY_LOW"));
    metric->set_bucket(ONE_MINUTE);
    // This case is interesting. We want to dimension across two atoms.
    metric->mutable_dimensions_in_what()->add_child()->set_field(1);

    auto alert = config.add_alert();
    alert->set_id(kAlertId);
    alert->set_metric_id(3);
    alert->set_num_buckets(10);
    alert->set_refractory_period_secs(100);
    alert->set_trigger_if_sum_gt(100);
    return config;
}

StatsdConfig buildCirclePredicates() {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_IS_ON"));

    SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(SCREEN_STATE_ATOM_ID);
    simpleAtomMatcher->add_field_value_matcher()->set_field(
            1 /*SCREEN_STATE_CHANGE__DISPLAY_STATE*/);
    simpleAtomMatcher->mutable_field_value_matcher(0)->set_eq_int(
            2 /*SCREEN_STATE_CHANGE__DISPLAY_STATE__STATE_ON*/);

    eventMatcher = config.add_atom_matcher();
    eventMatcher->set_id(StringToId("SCREEN_IS_OFF"));

    simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(SCREEN_STATE_ATOM_ID);
    simpleAtomMatcher->add_field_value_matcher()->set_field(
            1 /*SCREEN_STATE_CHANGE__DISPLAY_STATE*/);
    simpleAtomMatcher->mutable_field_value_matcher(0)->set_eq_int(
            1 /*SCREEN_STATE_CHANGE__DISPLAY_STATE__STATE_OFF*/);

    auto condition = config.add_predicate();
    condition->set_id(StringToId("SCREEN_IS_ON"));
    SimplePredicate* simplePredicate = condition->mutable_simple_predicate();
    simplePredicate->set_start(StringToId("SCREEN_IS_ON"));
    simplePredicate->set_stop(StringToId("SCREEN_IS_OFF"));

    condition = config.add_predicate();
    condition->set_id(StringToId("SCREEN_IS_EITHER_ON_OFF"));

    Predicate_Combination* combination = condition->mutable_combination();
    combination->set_operation(LogicalOperation::OR);
    combination->add_predicate(StringToId("SCREEN_IS_ON"));
    combination->add_predicate(StringToId("SCREEN_IS_EITHER_ON_OFF"));

    return config;
}

StatsdConfig buildConfigWithDifferentPredicates() {
    StatsdConfig config;
    config.set_id(12345);

    auto pulledAtomMatcher =
            CreateSimpleAtomMatcher("SUBSYSTEM_SLEEP", util::SUBSYSTEM_SLEEP_STATE);
    *config.add_atom_matcher() = pulledAtomMatcher;
    auto screenOnAtomMatcher = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = screenOnAtomMatcher;
    auto screenOffAtomMatcher = CreateScreenTurnedOffAtomMatcher();
    *config.add_atom_matcher() = screenOffAtomMatcher;
    auto batteryNoneAtomMatcher = CreateBatteryStateNoneMatcher();
    *config.add_atom_matcher() = batteryNoneAtomMatcher;
    auto batteryUsbAtomMatcher = CreateBatteryStateUsbMatcher();
    *config.add_atom_matcher() = batteryUsbAtomMatcher;

    // Simple condition with InitialValue set to default (unknown).
    auto screenOnUnknownPredicate = CreateScreenIsOnPredicate();
    *config.add_predicate() = screenOnUnknownPredicate;

    // Simple condition with InitialValue set to false.
    auto screenOnFalsePredicate = config.add_predicate();
    screenOnFalsePredicate->set_id(StringToId("ScreenIsOnInitialFalse"));
    SimplePredicate* simpleScreenOnFalsePredicate =
            screenOnFalsePredicate->mutable_simple_predicate();
    simpleScreenOnFalsePredicate->set_start(screenOnAtomMatcher.id());
    simpleScreenOnFalsePredicate->set_stop(screenOffAtomMatcher.id());
    simpleScreenOnFalsePredicate->set_initial_value(SimplePredicate_InitialValue_FALSE);

    // Simple condition with InitialValue set to false.
    auto onBatteryFalsePredicate = config.add_predicate();
    onBatteryFalsePredicate->set_id(StringToId("OnBatteryInitialFalse"));
    SimplePredicate* simpleOnBatteryFalsePredicate =
            onBatteryFalsePredicate->mutable_simple_predicate();
    simpleOnBatteryFalsePredicate->set_start(batteryNoneAtomMatcher.id());
    simpleOnBatteryFalsePredicate->set_stop(batteryUsbAtomMatcher.id());
    simpleOnBatteryFalsePredicate->set_initial_value(SimplePredicate_InitialValue_FALSE);

    // Combination condition with both simple condition InitialValues set to false.
    auto screenOnFalseOnBatteryFalsePredicate = config.add_predicate();
    screenOnFalseOnBatteryFalsePredicate->set_id(StringToId("ScreenOnFalseOnBatteryFalse"));
    screenOnFalseOnBatteryFalsePredicate->mutable_combination()->set_operation(
            LogicalOperation::AND);
    addPredicateToPredicateCombination(*screenOnFalsePredicate,
                                       screenOnFalseOnBatteryFalsePredicate);
    addPredicateToPredicateCombination(*onBatteryFalsePredicate,
                                       screenOnFalseOnBatteryFalsePredicate);

    // Combination condition with one simple condition InitialValue set to unknown and one set to
    // false.
    auto screenOnUnknownOnBatteryFalsePredicate = config.add_predicate();
    screenOnUnknownOnBatteryFalsePredicate->set_id(StringToId("ScreenOnUnknowneOnBatteryFalse"));
    screenOnUnknownOnBatteryFalsePredicate->mutable_combination()->set_operation(
            LogicalOperation::AND);
    addPredicateToPredicateCombination(screenOnUnknownPredicate,
                                       screenOnUnknownOnBatteryFalsePredicate);
    addPredicateToPredicateCombination(*onBatteryFalsePredicate,
                                       screenOnUnknownOnBatteryFalsePredicate);

    // Simple condition metric with initial value false.
    ValueMetric* metric1 = config.add_value_metric();
    metric1->set_id(StringToId("ValueSubsystemSleepWhileScreenOnInitialFalse"));
    metric1->set_what(pulledAtomMatcher.id());
    *metric1->mutable_value_field() =
            CreateDimensions(util::SUBSYSTEM_SLEEP_STATE, {4 /* time sleeping field */});
    metric1->set_bucket(FIVE_MINUTES);
    metric1->set_condition(screenOnFalsePredicate->id());

    // Simple condition metric with initial value unknown.
    ValueMetric* metric2 = config.add_value_metric();
    metric2->set_id(StringToId("ValueSubsystemSleepWhileScreenOnInitialUnknown"));
    metric2->set_what(pulledAtomMatcher.id());
    *metric2->mutable_value_field() =
            CreateDimensions(util::SUBSYSTEM_SLEEP_STATE, {4 /* time sleeping field */});
    metric2->set_bucket(FIVE_MINUTES);
    metric2->set_condition(screenOnUnknownPredicate.id());

    // Combination condition metric with initial values false and false.
    ValueMetric* metric3 = config.add_value_metric();
    metric3->set_id(StringToId("ValueSubsystemSleepWhileScreenOnFalseDeviceUnpluggedFalse"));
    metric3->set_what(pulledAtomMatcher.id());
    *metric3->mutable_value_field() =
            CreateDimensions(util::SUBSYSTEM_SLEEP_STATE, {4 /* time sleeping field */});
    metric3->set_bucket(FIVE_MINUTES);
    metric3->set_condition(screenOnFalseOnBatteryFalsePredicate->id());

    // Combination condition metric with initial values unknown and false.
    ValueMetric* metric4 = config.add_value_metric();
    metric4->set_id(StringToId("ValueSubsystemSleepWhileScreenOnUnknownDeviceUnpluggedFalse"));
    metric4->set_what(pulledAtomMatcher.id());
    *metric4->mutable_value_field() =
            CreateDimensions(util::SUBSYSTEM_SLEEP_STATE, {4 /* time sleeping field */});
    metric4->set_bucket(FIVE_MINUTES);
    metric4->set_condition(screenOnUnknownOnBatteryFalsePredicate->id());

    return config;
}

void initConfigAndDependencies(
        StatsdConfig& config, unordered_map<int64_t, int>& newMetricProducerMap,
        vector<sp<MetricProducer>>& newMetricProducers,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation, unordered_map<int64_t, int>& alertTrackerMap,
        vector<sp<AnomalyTracker>>& allAnomalyTrackers,
        unordered_map<int64_t, int>& alarmTrackerMap,
        vector<sp<AlarmTracker>>& allPeriodicAlarmTrackers,
        unordered_map<InvalidEntityKey, InvalidConfigReason>& invalidEntities) {
    unordered_map<int, vector<int>> tagIds;
    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    vector<sp<AtomMatchingTracker>> newAtomMatchingTrackers;
    sp<UidMap> uidMap = new UidMap();
    int oldInvalidEntityCount = 0;
    bool isValid = initAtomMatchingTrackers(config, uidMap, newAtomMatchingTrackerMap,
                                            newAtomMatchingTrackers, tagIds, invalidEntities);
    int newInvalidEntityCount = invalidEntities.size();
    if (newInvalidEntityCount == oldInvalidEntityCount) {
        EXPECT_TRUE(isValid);
    } else {
        EXPECT_FALSE(isValid);
    }

    const ConfigKey key(123, 456);
    unordered_map<int64_t, int> newConditionTrackerMap;
    vector<sp<ConditionTracker>> newConditionTrackers;
    unordered_map<int, vector<int>> trackerToConditionMap;
    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    vector<ConditionState> conditionCache;
    oldInvalidEntityCount = newInvalidEntityCount;
    isValid = initConditions(key, config, newAtomMatchingTrackerMap, newConditionTrackerMap,
                             newConditionTrackers, trackerToConditionMap, conditionCache,
                             allConditionsMap, invalidEntities);
    newInvalidEntityCount = invalidEntities.size();
    if (newInvalidEntityCount == oldInvalidEntityCount) {
        EXPECT_TRUE(isValid);
    } else {
        EXPECT_FALSE(isValid);
    }

    unordered_map<int64_t, int> stateAtomIdMap;
    unordered_map<int64_t, unordered_map<int, int64_t>> allStateGroupMaps;
    map<int64_t, uint64_t> stateProtoHashes;
    oldInvalidEntityCount = newInvalidEntityCount;
    isValid = initStates(config, stateAtomIdMap, allStateGroupMaps, stateProtoHashes,
                         invalidEntities);
    newInvalidEntityCount = invalidEntities.size();
    if (newInvalidEntityCount == oldInvalidEntityCount) {
        EXPECT_TRUE(isValid);
    } else {
        EXPECT_FALSE(isValid);
    }

    set<int64_t> noReportMetricIds;
    sp<MockConfigMetadataProvider> provider = makeMockConfigMetadataProvider(/*enabled=*/false);
    oldInvalidEntityCount = newInvalidEntityCount;
    isValid = initMetrics(
            key, config, /*timeBaseNs=*/123, /*currentTimeNs=*/12345, new StatsPullerManager(),
            newAtomMatchingTrackerMap, newConditionTrackerMap, newAtomMatchingTrackers,
            stateAtomIdMap, allStateGroupMaps, allConditionsMap, newConditionTrackers,
            conditionCache, newMetricProducers, conditionToMetricMap, trackerToMetricMap,
            newMetricProducerMap, noReportMetricIds, activationAtomTrackerToMetricMap,
            deactivationAtomTrackerToMetricMap, metricsWithActivation, provider, invalidEntities);
    newInvalidEntityCount = invalidEntities.size();
    if (newInvalidEntityCount == oldInvalidEntityCount) {
        EXPECT_TRUE(isValid);
    } else {
        EXPECT_FALSE(isValid);
    }

    sp<AlarmMonitor> anomalyAlarmMonitor;
    oldInvalidEntityCount = newInvalidEntityCount;
    isValid = initAlerts(config, /*currentTimeNs=*/12345, newMetricProducerMap, alertTrackerMap,
                         anomalyAlarmMonitor, newMetricProducers, allAnomalyTrackers,
                         invalidEntities);
    newInvalidEntityCount = invalidEntities.size();
    if (newInvalidEntityCount == oldInvalidEntityCount) {
        EXPECT_TRUE(isValid);
    } else {
        EXPECT_FALSE(isValid);
    }

    sp<AlarmMonitor> periodicAlarmMonitor = new AlarmMonitor(
            /*minDiffToUpdateRegisteredAlarmTimeSec=*/0,
            [](const shared_ptr<IStatsCompanionService>&, int64_t) {},
            [](const shared_ptr<IStatsCompanionService>&) {});
    oldInvalidEntityCount = newInvalidEntityCount;
    isValid = initAlarms(config, key, periodicAlarmMonitor, /*timeBaseNs=*/123,
                         /*currentTimeNs=*/12345, alarmTrackerMap, allPeriodicAlarmTrackers,
                         invalidEntities);
    newInvalidEntityCount = invalidEntities.size();
    if (newInvalidEntityCount == oldInvalidEntityCount) {
        EXPECT_TRUE(isValid);
    } else {
        EXPECT_FALSE(isValid);
    }
}

using MetricsManagerUtilTest = InitConfigTest;

struct DimLimitTestCase {
    int configLimit;
    int actualLimit;

    friend void PrintTo(const DimLimitTestCase& testCase, std::ostream* os) {
        *os << testCase.configLimit;
    }
};

class MetricsManagerUtilDimLimitTest : public MetricsManagerUtilTest,
                                       public WithParamInterface<DimLimitTestCase> {};

const vector<DimLimitTestCase> dimLimitTestCases = {{900, 900}, {799, 800}, {3001, 3000}, {0, 800}};

INSTANTIATE_TEST_SUITE_P(DimLimit, MetricsManagerUtilDimLimitTest, ValuesIn(dimLimitTestCases),
                         PrintToStringParamName());

}  // anonymous namespace

TEST_F(MetricsManagerUtilTest, TestInitialConditions) {
    EXPECT_TRUE(initConfig(buildConfigWithDifferentPredicates()).empty());
    ASSERT_EQ(4u, allMetricProducers.size());
    ASSERT_EQ(5u, allConditionTrackers.size());

    ConditionKey queryKey;
    vector<ConditionState> conditionCache(5, ConditionState::kNotEvaluated);

    allConditionTrackers[3]->isConditionMet(queryKey, allConditionTrackers, false, conditionCache);
    allConditionTrackers[4]->isConditionMet(queryKey, allConditionTrackers, false, conditionCache);
    EXPECT_EQ(ConditionState::kUnknown, conditionCache[0]);
    EXPECT_EQ(ConditionState::kFalse, conditionCache[1]);
    EXPECT_EQ(ConditionState::kFalse, conditionCache[2]);
    EXPECT_EQ(ConditionState::kFalse, conditionCache[3]);
    EXPECT_EQ(ConditionState::kUnknown, conditionCache[4]);

    EXPECT_EQ(ConditionState::kFalse, allMetricProducers[0]->mCondition);
    EXPECT_EQ(ConditionState::kUnknown, allMetricProducers[1]->mCondition);
    EXPECT_EQ(ConditionState::kFalse, allMetricProducers[2]->mCondition);
    EXPECT_EQ(ConditionState::kUnknown, allMetricProducers[3]->mCondition);

    EXPECT_EQ(allTagIdsToMatchersMap.size(), 3);
    EXPECT_EQ(allTagIdsToMatchersMap[SCREEN_STATE_ATOM_ID].size(), 2);
    EXPECT_EQ(allTagIdsToMatchersMap[util::PLUGGED_STATE_CHANGED].size(), 2);
    EXPECT_EQ(allTagIdsToMatchersMap[util::SUBSYSTEM_SLEEP_STATE].size(), 1);
}

TEST_F(MetricsManagerUtilTest, TestGoodConfig) {
    StatsdConfig config = buildGoodConfig(kConfigId, kAlertId);
    config.add_no_report_metric(config.count_metric(0).id());
    EXPECT_TRUE(initConfig(config).empty());
    ASSERT_EQ(5u, allMetricProducers.size());
    EXPECT_THAT(metricProducerMap, UnorderedElementsAre(Key(config.count_metric(0).id()),
                                                        Key(config.duration_metric(0).id()),
                                                        Key(config.value_metric(0).id()),
                                                        Key(config.kll_metric(0).id()),
                                                        Key(config.gauge_metric(0).id())));
    ASSERT_EQ(1u, allAnomalyTrackers.size());
    ASSERT_EQ(1u, noReportMetricIds.size());
    ASSERT_EQ(1u, alertTrackerMap.size());
    EXPECT_NE(alertTrackerMap.find(kAlertId), alertTrackerMap.end());
    EXPECT_EQ(alertTrackerMap.find(kAlertId)->second, 0);
}

TEST_F(MetricsManagerUtilTest, TestDimensionMetricsWithMultiTags) {
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities =
            initConfig(buildDimensionMetricsWithMultiTags());
    EXPECT_EQ(invalidEntities[(InvalidEntityKey{/*metric id =*/3, INVALID_ENTITY_TYPE_METRIC})],
              createInvalidConfigReasonWithMatcher(
                      INVALID_CONFIG_REASON_METRIC_MATCHER_MORE_THAN_ONE_ATOM, /*metric id=*/3,
                      StringToId("BATTERY_LOW")));
}

TEST_F(MetricsManagerUtilTest, TestCircleLogMatcherDependency) {
    optional<InvalidConfigReason> expectedInvalidConfigReason =
            createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_CYCLE,
                                                 StringToId("SCREEN_ON_OR_OFF"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities =
            initConfig(buildCircleMatchers());

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("SCREEN_ON_OR_OFF"),
                                                INVALID_ENTITY_TYPE_MATCHER})],
              expectedInvalidConfigReason);
}

TEST_F(MetricsManagerUtilTest, TestMissingMatchers) {
    optional<InvalidConfigReason> expectedInvalidConfigReason =
            createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_CHILD_NOT_FOUND,
                                                 StringToId("SCREEN_ON_OR_OFF"));
    expectedInvalidConfigReason->matcherIds.push_back(StringToId("ABC"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities =
            initConfig(buildMissingMatchers());

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("SCREEN_ON_OR_OFF"),
                                                INVALID_ENTITY_TYPE_MATCHER})],
              expectedInvalidConfigReason);
}

TEST_F(MetricsManagerUtilTest, TestMissingPredicate) {
    optional<InvalidConfigReason> expectedInvalidConfigReason =
            createInvalidConfigReasonWithPredicate(INVALID_CONFIG_REASON_METRIC_CONDITION_NOT_FOUND,
                                                   /*metric id=*/3, StringToId("SOME_CONDITION"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities =
            initConfig(buildMissingPredicate());

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{/*metric id=*/3, INVALID_ENTITY_TYPE_METRIC})],
              expectedInvalidConfigReason);
}

TEST_F(MetricsManagerUtilTest, TestCirclePredicateDependency) {
    optional<InvalidConfigReason> expectedInvalidConfigReason =
            createInvalidConfigReasonWithPredicate(INVALID_CONFIG_REASON_CONDITION_CYCLE,
                                                   StringToId("SCREEN_IS_EITHER_ON_OFF"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities =
            initConfig(buildCirclePredicates());

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("SCREEN_IS_EITHER_ON_OFF"),
                                                INVALID_ENTITY_TYPE_PREDICATE})],
              expectedInvalidConfigReason);
}

TEST_F(MetricsManagerUtilTest, TestAlertWithUnknownMetric) {
    optional<InvalidConfigReason> expectedInvalidConfigReason =
            createInvalidConfigReasonWithAlert(INVALID_CONFIG_REASON_ALERT_METRIC_NOT_FOUND,
                                               /*metric id=*/2, /*matcher id=*/3);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities =
            initConfig(buildAlertWithUnknownMetric());

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{/*alert id=*/3, INVALID_ENTITY_TYPE_ALERT})],
              expectedInvalidConfigReason);
}

TEST_F(MetricsManagerUtilTest, TestMetricWithMultipleActivations) {
    StatsdConfig config;
    int64_t metricId = 1;
    auto metric_activation1 = config.add_metric_activation();
    metric_activation1->set_metric_id(metricId);
    metric_activation1->set_activation_type(ACTIVATE_IMMEDIATELY);
    auto metric_activation2 = config.add_metric_activation();
    metric_activation2->set_metric_id(metricId);
    metric_activation2->set_activation_type(ACTIVATE_IMMEDIATELY);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_HAS_MULTIPLE_ACTIVATIONS, metricId));
}

TEST_F(MetricsManagerUtilTest, TestCountMetricMissingIdOrWhat) {
    StatsdConfig config;
    int64_t metricId = 1;
    CountMetric* metric = config.add_count_metric();
    metric->set_id(metricId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metricId));
}

TEST_F(MetricsManagerUtilTest, TestCountMetricConditionlinkNoCondition) {
    StatsdConfig config;
    CountMetric* metric = config.add_count_metric();
    *metric = createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    auto link = metric->add_links();
    link->set_condition(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                  StringToId("Count")));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricMissingIdOrWhat) {
    StatsdConfig config;
    int64_t metricId = 1;
    DurationMetric* metric = config.add_duration_metric();
    metric->set_id(metricId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metricId));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricConditionlinkNoCondition) {
    StatsdConfig config;
    DurationMetric* metric = config.add_duration_metric();
    *metric = createDurationMetric(/*name=*/"Duration", /*what=*/StringToId("ScreenIsOn"),
                                   /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOffAtomMatcher();
    *config.add_predicate() = CreateScreenIsOnPredicate();

    auto link = metric->add_links();
    link->set_condition(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                  StringToId("Duration")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricMissingIdOrWhat) {
    StatsdConfig config;
    int64_t metricId = 1;
    GaugeMetric* metric = config.add_gauge_metric();
    metric->set_id(metricId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metricId));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricConditionlinkNoCondition) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("ScreenTurnedOn"),
                                /*samplingType=*/GaugeMetric_SamplingType_FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    auto link = metric->add_links();
    link->set_condition(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestEventMetricMissingIdOrWhat) {
    StatsdConfig config;
    int64_t metricId = 1;
    EventMetric* metric = config.add_event_metric();
    metric->set_id(metricId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metricId));
}

TEST_F(MetricsManagerUtilTest, TestEventMetricConditionlinkNoCondition) {
    StatsdConfig config;
    EventMetric* metric = config.add_event_metric();
    *metric = createEventMetric(/*name=*/"Event", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    auto link = metric->add_links();
    link->set_condition(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                  StringToId("Event")));
}

TEST_F(MetricsManagerUtilTest, TestEventMetricInvalidSamplingPercentage) {
    StatsdConfig config;
    EventMetric* metric = config.add_event_metric();
    *metric = createEventMetric(/*name=*/"Event", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt);
    metric->set_sampling_percentage(101);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_SAMPLING_PERCENTAGE,
                                  StringToId("Event")));
}

TEST_F(MetricsManagerUtilTest, TestEventMetricInvalidSamplingPercentageZero) {
    StatsdConfig config;
    EventMetric* metric = config.add_event_metric();
    *metric = createEventMetric(/*name=*/"Event", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt);
    metric->set_sampling_percentage(0);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_SAMPLING_PERCENTAGE,
                                  StringToId("Event")));
}

TEST_F(MetricsManagerUtilTest, TestEventMetricValidSamplingPercentage) {
    StatsdConfig config;
    EventMetric* metric = config.add_event_metric();
    *metric = createEventMetric(/*name=*/"Event", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt);
    metric->set_sampling_percentage(50);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestEventMetricIncorrectFieldFilter) {
    StatsdConfig config;
    int64_t metricId = 1;
    EventMetric* metric = config.add_event_metric();
    metric->set_id(metricId);
    metric->set_what(1);
    metric->mutable_fields_filter()->mutable_fields();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_FIELD_FILTER, metricId));
}

TEST_F(MetricsManagerUtilTest, TestEventMetricIncorrectFieldFilterOmitNoLeafValues) {
    StatsdConfig config;
    int64_t metricId = 1;
    EventMetric* metric = config.add_event_metric();
    metric->set_id(metricId);
    metric->set_what(1);
    metric->mutable_fields_filter()->mutable_omit_fields();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_FIELD_FILTER, metricId));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricInvalidSamplingPercentage) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("ScreenTurnedOn"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_sampling_percentage(101);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_SAMPLING_PERCENTAGE,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricInvalidSamplingPercentageZero) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("ScreenTurnedOn"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_sampling_percentage(0);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_SAMPLING_PERCENTAGE,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricValidSamplingPercentage) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("ScreenTurnedOn"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_sampling_percentage(50);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestPulledGaugeMetricWithSamplingPercentage) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("SubsystemSleep"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_sampling_percentage(50);
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_GAUGE_METRIC_PULLED_WITH_SAMPLING,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricInvalidPullProbability) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("SubsystemSleep"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_pull_probability(101);
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_PULL_PROBABILITY,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricInvalidPullProbabilityZero) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("SubsystemSleep"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_pull_probability(0);
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_PULL_PROBABILITY,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricValidPullProbability) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("SubsystemSleep"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_pull_probability(50);
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestPushedGaugeMetricWithPullProbability) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("ScreenTurnedOn"),
                                GaugeMetric::FIRST_N_SAMPLES,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_pull_probability(50);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_GAUGE_METRIC_PUSHED_WITH_PULL_PROBABILITY,
                                  StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricRandomOneSampleWithPullProbability) {
    StatsdConfig config;
    GaugeMetric* metric = config.add_gauge_metric();
    *metric = createGaugeMetric(/*name=*/"Gauge", /*what=*/StringToId("SubsystemSleep"),
                                GaugeMetric::RANDOM_ONE_SAMPLE,
                                /*condition=*/nullopt, /*triggerEvent=*/nullopt);
    metric->set_pull_probability(50);
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_GAUGE_METRIC_RANDOM_ONE_SAMPLE_WITH_PULL_PROBABILITY,
                      StringToId("Gauge")));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricMissingIdOrWhat) {
    StatsdConfig config;
    int64_t metricId = 1;
    ValueMetric* metric = config.add_value_metric();
    metric->set_id(metricId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metricId));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricConditionlinkNoCondition) {
    StatsdConfig config;
    ValueMetric* metric = config.add_value_metric();
    *metric = createValueMetric(/*name=*/"NumericValue", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                                /*valueField=*/2, /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    auto link = metric->add_links();
    link->set_condition(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                  StringToId("NumericValue")));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricHasBothSingleAndMultipleAggTypes) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    ValueMetric* metric = config.add_value_metric();
    *metric = createValueMetric(/*name=*/"NumericValue", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                                /*valueField=*/2, /*condition=*/nullopt, /*states=*/{});
    metric->set_aggregation_type(ValueMetric::SUM);
    metric->add_aggregation_types(ValueMetric::SUM);
    metric->add_aggregation_types(ValueMetric::MIN);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_DEFINES_SINGLE_AND_MULTIPLE_AGG_TYPES,
                      StringToId("NumericValue")));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricMoreAggTypesThanValueFields) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    ValueMetric* metric = config.add_value_metric();
    *metric = createValueMetric(/*name=*/"NumericValue", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                                /*valueField=*/2, /*condition=*/nullopt, /*states=*/{});
    metric->add_aggregation_types(ValueMetric::SUM);
    metric->add_aggregation_types(ValueMetric::MIN);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_AGG_TYPES_DNE_VALUE_FIELDS_SIZE,
                                StringToId("NumericValue")));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricMoreValueFieldsThanAggTypes) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    ValueMetric* metric = config.add_value_metric();
    *metric = createValueMetric(/*name=*/"NumericValue", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                                /*valueField=*/2, /*condition=*/nullopt, /*states=*/{});
    // This only fails if the repeated aggregation field is used. If the single field is used,
    // we will apply this aggregation type to all value fields.
    metric->add_aggregation_types(ValueMetric::SUM);
    metric->add_aggregation_types(ValueMetric::MIN);
    *metric->mutable_value_field() = CreateDimensions(
            util::SUBSYSTEM_SLEEP_STATE, {3 /* count */, 4 /* time_millis */, 3 /* count */});

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_AGG_TYPES_DNE_VALUE_FIELDS_SIZE,
                                StringToId("NumericValue")));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricDefaultAggTypeOutOfOrderFields) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    ValueMetric* metric = config.add_value_metric();
    *metric = createValueMetric(/*name=*/"NumericValue", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                                /*valueField=*/2, /*condition=*/nullopt, /*states=*/{});
    *metric->mutable_value_field() =
            CreateDimensions(util::SUBSYSTEM_SLEEP_STATE, {4 /* time_millis */, 3 /* count */});

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricMultipleAggTypesOutOfOrderFields) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    ValueMetric* metric = config.add_value_metric();
    *metric = createValueMetric(/*name=*/"NumericValue", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                                /*valueField=*/2, /*condition=*/nullopt, /*states=*/{});
    metric->add_aggregation_types(ValueMetric::SUM);
    metric->add_aggregation_types(ValueMetric::MIN);
    metric->add_aggregation_types(ValueMetric::SUM);
    *metric->mutable_value_field() = CreateDimensions(
            util::SUBSYSTEM_SLEEP_STATE, {3 /* count */, 4 /* time_millis */, 3 /* count */});

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestKllMetricMissingIdOrWhat) {
    StatsdConfig config;
    int64_t metricId = 1;
    KllMetric* metric = config.add_kll_metric();
    metric->set_id(metricId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_MISSING_ID_OR_WHAT, metricId));
}

TEST_F(MetricsManagerUtilTest, TestKllMetricConditionlinkNoCondition) {
    StatsdConfig config;
    KllMetric* metric = config.add_kll_metric();
    *metric = createKllMetric(/*name=*/"Kll", /*what=*/CreateScreenTurnedOnAtomMatcher(),
                              /*valueField=*/2, /*condition=*/nullopt);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    auto link = metric->add_links();
    link->set_condition(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_CONDITIONLINK_NO_CONDITION,
                                  StringToId("Kll")));
}

TEST_F(MetricsManagerUtilTest, TestMetricMatcherNotFound) {
    StatsdConfig config;
    *config.add_count_metric() =
            createCountMetric(/*name=*/"Count", /*what=*/StringToId("SOME MATCHER"),
                              /*condition=*/nullopt, /*states=*/{});

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{StringToId("Count"), INVALID_ENTITY_TYPE_METRIC})],
            createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_METRIC_MATCHER_NOT_FOUND,
                                                 StringToId("Count"), StringToId("SOME MATCHER")));
}

TEST_F(MetricsManagerUtilTest, TestMetricConditionLinkNotFound) {
    StatsdConfig config;
    CountMetric* metric = config.add_count_metric();
    *metric = createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/StringToId("ScreenIsOn"), /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_predicate() = CreateScreenIsOnPredicate();

    auto link = metric->add_links();
    link->set_condition(StringToId("SOME CONDITION"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              createInvalidConfigReasonWithPredicate(
                      INVALID_CONFIG_REASON_METRIC_CONDITION_LINK_NOT_FOUND, StringToId("Count"),
                      StringToId("SOME CONDITION")));
}

TEST_F(MetricsManagerUtilTest, TestMetricStateNotFound) {
    StatsdConfig config;
    *config.add_count_metric() =
            createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                              /*condition=*/nullopt, /*states=*/{StringToId("SOME STATE")});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Count"), INVALID_ENTITY_TYPE_METRIC})],
              createInvalidConfigReasonWithState(INVALID_CONFIG_REASON_METRIC_STATE_NOT_FOUND,
                                                 StringToId("Count"), StringToId("SOME STATE")));
}

TEST_F(MetricsManagerUtilTest, TestMetricStatelinkNoState) {
    StatsdConfig config;
    CountMetric* metric = config.add_count_metric();
    *metric = createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    auto link = metric->add_state_link();
    link->set_state_atom_id(2);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_STATELINK_NO_STATE,
                                  StringToId("Count")));
}

TEST_F(MetricsManagerUtilTest, TestMetricBadThreshold) {
    StatsdConfig config;
    CountMetric* metric = config.add_count_metric();
    *metric = createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    metric->mutable_threshold()->set_lt_float(1.0);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_BAD_THRESHOLD, StringToId("Count")));
}

TEST_F(MetricsManagerUtilTest, TestMetricActivationMatcherNotFound) {
    StatsdConfig config;
    *config.add_count_metric() =
            createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                              /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    auto metric_activation = config.add_metric_activation();
    metric_activation->set_metric_id(StringToId("Count"));
    metric_activation->set_activation_type(ACTIVATE_IMMEDIATELY);
    auto event_activation = metric_activation->add_event_activation();

    event_activation->set_atom_matcher_id(StringToId("SOME_MATCHER"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Count"), INVALID_ENTITY_TYPE_METRIC})],
              createInvalidConfigReasonWithMatcher(
                      INVALID_CONFIG_REASON_METRIC_ACTIVATION_MATCHER_NOT_FOUND,
                      StringToId("Count"), StringToId("SOME_MATCHER")));
}

TEST_F(MetricsManagerUtilTest, TestMetricDeactivationMatcherNotFound) {
    StatsdConfig config;
    *config.add_count_metric() =
            createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                              /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    auto metric_activation = config.add_metric_activation();
    metric_activation->set_metric_id(StringToId("Count"));
    metric_activation->set_activation_type(ACTIVATE_IMMEDIATELY);
    auto event_activation = metric_activation->add_event_activation();
    event_activation->set_atom_matcher_id(StringToId("ScreenTurnedOn"));

    event_activation->set_deactivation_atom_matcher_id(StringToId("SOME_MATCHER"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Count"), INVALID_ENTITY_TYPE_METRIC})],
              createInvalidConfigReasonWithMatcher(
                      INVALID_CONFIG_REASON_METRIC_DEACTIVATION_MATCHER_NOT_FOUND,
                      StringToId("Count"), StringToId("SOME_MATCHER")));
}

TEST_F(MetricsManagerUtilTest, TestMetricSlicedStateAtomAllowedFromAnyUid) {
    StatsdConfig config;
    CountMetric* metric = config.add_count_metric();
    *metric = createCountMetric(/*name=*/"Count", /*what=*/StringToId("ScreenTurnedOn"),
                                /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    *config.add_state() = CreateScreenState();
    metric->add_slice_by_state(StringToId("ScreenState"));
    config.add_whitelisted_atom_ids(util::SCREEN_STATE_CHANGED);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
            InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SLICED_STATE_ATOM_ALLOWED_FROM_ANY_UID,
                                StringToId("Count")));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricWhatNotSimple) {
    StatsdConfig config;
    *config.add_duration_metric() =
            createDurationMetric(/*name=*/"Duration", /*what=*/StringToId("ScreenIsEitherOnOff"),
                                 /*condition=*/nullopt, /*states=*/{});
    *config.add_predicate() = CreateScreenIsOnPredicate();
    *config.add_predicate() = CreateScreenIsOffPredicate();

    auto condition = config.add_predicate();
    condition->set_id(StringToId("ScreenIsEitherOnOff"));
    Predicate_Combination* combination = condition->mutable_combination();
    combination->set_operation(LogicalOperation::OR);
    combination->add_predicate(StringToId("ScreenIsOn"));
    combination->add_predicate(StringToId("ScreenIsOff"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{StringToId("Duration"), INVALID_ENTITY_TYPE_METRIC})],
            createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_DURATION_METRIC_WHAT_NOT_SIMPLE, StringToId("Duration"),
                    StringToId("ScreenIsEitherOnOff")));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricWhatNotFound) {
    StatsdConfig config;
    int64_t metricId = 1;
    DurationMetric* metric = config.add_duration_metric();
    metric->set_id(metricId);

    metric->set_what(StringToId("SOME WHAT"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              createInvalidConfigReasonWithPredicate(
                      INVALID_CONFIG_REASON_DURATION_METRIC_WHAT_NOT_FOUND, metricId,
                      StringToId("SOME WHAT")));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricMissingStart) {
    StatsdConfig config;
    *config.add_duration_metric() =
            createDurationMetric(/*name=*/"Duration", /*what=*/StringToId("SCREEN_IS_ON"),
                                 /*condition=*/nullopt, /*states=*/{});
    auto condition = config.add_predicate();
    condition->set_id(StringToId("SCREEN_IS_ON"));

    SimplePredicate* simplePredicate = condition->mutable_simple_predicate();
    simplePredicate->set_stop(StringToId("SCREEN_IS_OFF"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{StringToId("Duration"), INVALID_ENTITY_TYPE_METRIC})],
            createInvalidConfigReasonWithPredicate(
                    INVALID_CONFIG_REASON_DURATION_METRIC_MISSING_START, StringToId("Duration"),
                    StringToId("SCREEN_IS_ON")));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricMaxSparseHasSpliceByState) {
    StatsdConfig config;
    DurationMetric* metric = config.add_duration_metric();
    *metric = createDurationMetric(/*name=*/"Duration", /*what=*/StringToId("ScreenIsOn"),
                                   /*condition=*/nullopt, /*states=*/{});
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOffAtomMatcher();
    *config.add_predicate() = CreateScreenIsOnPredicate();
    *config.add_state() = CreateScreenState();

    metric->add_slice_by_state(StringToId("ScreenState"));
    metric->set_aggregation_type(DurationMetric::MAX_SPARSE);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{metric->id(), INVALID_ENTITY_TYPE_METRIC})],
            InvalidConfigReason(INVALID_CONFIG_REASON_DURATION_METRIC_MAX_SPARSE_HAS_SLICE_BY_STATE,
                                StringToId("Duration")));
}

TEST_F(MetricsManagerUtilTest, TestValueMetricMissingValueField) {
    StatsdConfig config;
    int64_t metricId = 1;
    ValueMetric* metric = config.add_value_metric();
    metric->set_id(metricId);
    metric->set_what(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_MISSING_VALUE_FIELD, metricId));
}

TEST_F(MetricsManagerUtilTest, TestValueMetricValueFieldHasPositionAll) {
    StatsdConfig config;
    int64_t metricId = 1;
    ValueMetric* metric = config.add_value_metric();
    metric->set_id(metricId);
    metric->set_what(1);

    metric->mutable_value_field()->add_child()->set_field(2);
    metric->mutable_value_field()->mutable_child(0)->set_position(ALL);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_VALUE_FIELD_HAS_POSITION_ALL,
                                  metricId));
}

TEST_F(MetricsManagerUtilTest, TestValueMetricHasIncorrectValueField) {
    StatsdConfig config;
    int64_t metricId = 1;
    ValueMetric* metric = config.add_value_metric();
    metric->set_id(metricId);
    metric->set_what(1);

    metric->mutable_value_field()->set_position(ANY);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HAS_INCORRECT_VALUE_FIELD,
                                  metricId));
}

TEST_F(MetricsManagerUtilTest, TestKllMetricMissingKllField) {
    StatsdConfig config;
    int64_t metricId = 1;
    KllMetric* metric = config.add_kll_metric();
    metric->set_id(metricId);
    metric->set_what(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_KLL_METRIC_MISSING_KLL_FIELD, metricId));
}

TEST_F(MetricsManagerUtilTest, TestKllMetricKllFieldHasPositionAll) {
    StatsdConfig config;
    int64_t metricId = 1;
    KllMetric* metric = config.add_kll_metric();
    metric->set_id(metricId);
    metric->set_what(1);

    metric->mutable_kll_field()->set_position(ALL);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_KLL_METRIC_KLL_FIELD_HAS_POSITION_ALL,
                                  metricId));
}

TEST_F(MetricsManagerUtilTest, TestKllMetricHasIncorrectKllField) {
    StatsdConfig config;
    int64_t metricId = 1;
    KllMetric* metric = config.add_kll_metric();
    metric->set_id(metricId);
    metric->set_what(1);

    metric->mutable_kll_field()->set_position(ANY);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_KLL_METRIC_HAS_INCORRECT_KLL_FIELD,
                                  metricId));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricIncorrectFieldFilterNoLeafValues) {
    StatsdConfig config;
    int64_t metricId = 1;
    GaugeMetric* metric = config.add_gauge_metric();
    metric->set_id(metricId);
    metric->set_what(1);
    metric->mutable_gauge_fields_filter()->mutable_fields();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_FIELD_FILTER, metricId));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricIncorrectFieldFilterOmitNoLeafValues) {
    StatsdConfig config;
    int64_t metricId = 1;
    GaugeMetric* metric = config.add_gauge_metric();
    metric->set_id(metricId);
    metric->set_what(1);
    metric->mutable_gauge_fields_filter()->mutable_omit_fields();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_INCORRECT_FIELD_FILTER, metricId));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricTriggerNoPullAtom) {
    StatsdConfig config;
    int64_t metricId = 1;
    GaugeMetric* metric = config.add_gauge_metric();
    metric->set_id(metricId);
    metric->set_what(StringToId("ScreenTurnedOn"));
    metric->set_trigger_event(1);

    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(
            invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
            InvalidConfigReason(INVALID_CONFIG_REASON_GAUGE_METRIC_TRIGGER_NO_PULL_ATOM, metricId));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricTriggerNoFirstNSamples) {
    StatsdConfig config;
    int64_t metricId = 1;
    GaugeMetric* metric = config.add_gauge_metric();
    metric->set_id(metricId);
    metric->set_what(StringToId("Matcher"));
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher(/*name=*/"Matcher", /*atomId=*/util::SUBSYSTEM_SLEEP_STATE);

    metric->set_trigger_event(StringToId("Matcher"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC})],
              InvalidConfigReason(INVALID_CONFIG_REASON_GAUGE_METRIC_TRIGGER_NO_FIRST_N_SAMPLES,
                                  metricId));
}

TEST_F(MetricsManagerUtilTest, TestMatcherDuplicate) {
    StatsdConfig config;

    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(
                      InvalidEntityKey{StringToId("ScreenTurnedOn"), INVALID_ENTITY_TYPE_MATCHER})],
              createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_DUPLICATE,
                                                   StringToId("ScreenTurnedOn")));
}

TEST_F(MetricsManagerUtilTest, TestMatcherNoOperation) {
    StatsdConfig config;
    int64_t matcherId = 1;

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(matcherId);
    matcher->mutable_combination()->add_matcher(StringToId("ScreenTurnedOn"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{matcherId, INVALID_ENTITY_TYPE_MATCHER})],
              createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_NO_OPERATION,
                                                   matcherId));
}

TEST_F(MetricsManagerUtilTest, TestMatcherNotOperationIsNotUnary) {
    StatsdConfig config;
    int64_t matcherId = 1;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOffAtomMatcher();

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(matcherId);
    matcher->mutable_combination()->set_operation(LogicalOperation::NOT);
    matcher->mutable_combination()->add_matcher(StringToId("ScreenTurnedOn"));
    matcher->mutable_combination()->add_matcher(StringToId("ScreenTurnedOff"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{matcherId, INVALID_ENTITY_TYPE_MATCHER})],
              createInvalidConfigReasonWithMatcher(
                      INVALID_CONFIG_REASON_MATCHER_NOT_OPERATION_IS_NOT_UNARY, matcherId));
}

TEST_F(MetricsManagerUtilTest, TestConditionChildNotFound) {
    StatsdConfig config;
    int64_t conditionId = 1;
    int64_t childConditionId = 2;

    Predicate* condition = config.add_predicate();
    condition->set_id(conditionId);
    condition->mutable_combination()->set_operation(LogicalOperation::NOT);
    condition->mutable_combination()->add_predicate(childConditionId);

    optional<InvalidConfigReason> expectedInvalidConfigReason =
            createInvalidConfigReasonWithPredicate(INVALID_CONFIG_REASON_CONDITION_CHILD_NOT_FOUND,
                                                   conditionId);
    expectedInvalidConfigReason->conditionIds.push_back(childConditionId);
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{conditionId, INVALID_ENTITY_TYPE_PREDICATE})],
              expectedInvalidConfigReason);
}

TEST_F(MetricsManagerUtilTest, TestConditionDuplicate) {
    StatsdConfig config;
    *config.add_predicate() = CreateScreenIsOnPredicate();
    *config.add_predicate() = CreateScreenIsOnPredicate();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(
                      InvalidEntityKey{StringToId("ScreenIsOn"), INVALID_ENTITY_TYPE_PREDICATE})],
              createInvalidConfigReasonWithPredicate(INVALID_CONFIG_REASON_CONDITION_DUPLICATE,
                                                     StringToId("ScreenIsOn")));
}

TEST_F(MetricsManagerUtilTest, TestConditionNoOperation) {
    StatsdConfig config;
    int64_t conditionId = 1;
    *config.add_predicate() = CreateScreenIsOnPredicate();

    Predicate* condition = config.add_predicate();
    condition->set_id(conditionId);
    condition->mutable_combination()->add_predicate(StringToId("ScreenIsOn"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{conditionId, INVALID_ENTITY_TYPE_PREDICATE})],
              createInvalidConfigReasonWithPredicate(INVALID_CONFIG_REASON_CONDITION_NO_OPERATION,
                                                     conditionId));
}

TEST_F(MetricsManagerUtilTest, TestConditionNotOperationIsNotUnary) {
    StatsdConfig config;
    int64_t conditionId = 1;
    *config.add_predicate() = CreateScreenIsOnPredicate();
    *config.add_predicate() = CreateScreenIsOffPredicate();

    Predicate* condition = config.add_predicate();
    condition->set_id(conditionId);
    condition->mutable_combination()->set_operation(LogicalOperation::NOT);
    condition->mutable_combination()->add_predicate(StringToId("ScreenIsOn"));
    condition->mutable_combination()->add_predicate(StringToId("ScreenIsOff"));

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{conditionId, INVALID_ENTITY_TYPE_PREDICATE})],
              createInvalidConfigReasonWithPredicate(
                      INVALID_CONFIG_REASON_CONDITION_NOT_OPERATION_IS_NOT_UNARY, conditionId));
}

TEST_F(MetricsManagerUtilTest, TestSubscriptionRuleNotFoundAlert) {
    StatsdConfig config;
    int64_t alertId = 1;
    *config.add_subscription() = createSubscription("Subscription", Subscription::ALERT, alertId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Subscription"),
                                                INVALID_ENTITY_TYPE_SUBSCRIPTION})],
              createInvalidConfigReasonWithSubscriptionAndAlert(
                      INVALID_CONFIG_REASON_SUBSCRIPTION_RULE_NOT_FOUND, StringToId("Subscription"),
                      alertId));
}

TEST_F(MetricsManagerUtilTest, TestSubscriptionRuleNotFoundAlarm) {
    StatsdConfig config;
    int64_t alarmId = 1;
    *config.add_subscription() = createSubscription("Subscription", Subscription::ALARM, alarmId);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Subscription"),
                                                INVALID_ENTITY_TYPE_SUBSCRIPTION})],
              createInvalidConfigReasonWithSubscriptionAndAlarm(
                      INVALID_CONFIG_REASON_SUBSCRIPTION_RULE_NOT_FOUND, StringToId("Subscription"),
                      alarmId));
}

TEST_F(MetricsManagerUtilTest, TestSubscriptionSubscriberInfoMissing) {
    StatsdConfig config;
    Subscription subscription =
            createSubscription("Subscription", Subscription::ALERT, /*alert id=*/1);
    subscription.clear_subscriber_information();
    *config.add_subscription() = subscription;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Subscription"),
                                                INVALID_ENTITY_TYPE_SUBSCRIPTION})],
              createInvalidConfigReasonWithSubscription(
                      INVALID_CONFIG_REASON_SUBSCRIPTION_SUBSCRIBER_INFO_MISSING,
                      StringToId("Subscription")));
}

TEST_F(MetricsManagerUtilTest, TestAlarmPeriodLessThanOrEqualZero) {
    StatsdConfig config;
    *config.add_alarm() = createAlarm("Alarm", /*offset=*/1, /*period=*/-1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Alarm"), INVALID_ENTITY_TYPE_ALARM})],
              createInvalidConfigReasonWithAlarm(
                      INVALID_CONFIG_REASON_ALARM_PERIOD_LESS_THAN_OR_EQUAL_ZERO,
                      StringToId("Alarm")));
}

TEST_F(MetricsManagerUtilTest, TestAlarmOffsetLessThanOrEqualZero) {
    StatsdConfig config;
    *config.add_alarm() = createAlarm("Alarm", /*offset=*/-1, /*period=*/1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    EXPECT_EQ(invalidEntities[(InvalidEntityKey{StringToId("Alarm"), INVALID_ENTITY_TYPE_ALARM})],
              createInvalidConfigReasonWithAlarm(
                      INVALID_CONFIG_REASON_ALARM_OFFSET_LESS_THAN_OR_EQUAL_ZERO,
                      StringToId("Alarm")));
}

TEST_F(MetricsManagerUtilTest, TestCreateAtomMatchingTrackerInvalidMatcher) {
    sp<UidMap> uidMap = new UidMap();
    AtomMatcher matcher;
    // Matcher has no contents_case (simple/combination), so it is invalid.
    matcher.set_id(21);
    optional<InvalidConfigReason> invalidConfigReason;
    EXPECT_EQ(createAtomMatchingTracker(matcher, uidMap, invalidConfigReason), nullptr);
    EXPECT_EQ(invalidConfigReason,
              createInvalidConfigReasonWithMatcher(
                      INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE, matcher.id()));
}

TEST_F(MetricsManagerUtilTest, TestCreateAtomMatchingTrackerSimple) {
    int64_t id = 123;
    sp<UidMap> uidMap = new UidMap();
    AtomMatcher matcher;
    matcher.set_id(id);
    SimpleAtomMatcher* simpleAtomMatcher = matcher.mutable_simple_atom_matcher();
    simpleAtomMatcher->set_atom_id(SCREEN_STATE_ATOM_ID);
    simpleAtomMatcher->add_field_value_matcher()->set_field(
            1 /*SCREEN_STATE_CHANGE__DISPLAY_STATE*/);
    simpleAtomMatcher->mutable_field_value_matcher(0)->set_eq_int(
            android::view::DisplayStateEnum::DISPLAY_STATE_ON);

    optional<InvalidConfigReason> invalidConfigReason;
    sp<AtomMatchingTracker> tracker =
            createAtomMatchingTracker(matcher, uidMap, invalidConfigReason);
    EXPECT_NE(tracker, nullptr);
    EXPECT_EQ(invalidConfigReason, nullopt);

    EXPECT_EQ(tracker->getId(), id);
    const set<int>& atomIds = tracker->getAtomIds();
    ASSERT_EQ(atomIds.size(), 1);
    EXPECT_EQ(atomIds.count(SCREEN_STATE_ATOM_ID), 1);
}

TEST_F(MetricsManagerUtilTest, TestCreateAtomMatchingTrackerCombination) {
    int64_t id = 123;
    sp<UidMap> uidMap = new UidMap();
    AtomMatcher matcher;
    matcher.set_id(id);
    AtomMatcher_Combination* combination = matcher.mutable_combination();
    combination->set_operation(LogicalOperation::OR);
    combination->add_matcher(123);
    combination->add_matcher(223);

    optional<InvalidConfigReason> invalidConfigReason;
    sp<AtomMatchingTracker> tracker =
            createAtomMatchingTracker(matcher, uidMap, invalidConfigReason);
    EXPECT_NE(tracker, nullptr);
    EXPECT_EQ(invalidConfigReason, nullopt);

    // Combination matchers need to be initialized first.
    EXPECT_EQ(tracker->getId(), id);
    const set<int>& atomIds = tracker->getAtomIds();
    ASSERT_EQ(atomIds.size(), 0);
}

TEST_F(MetricsManagerUtilTest, TestCreateConditionTrackerInvalid) {
    const ConfigKey key(123, 456);
    // Predicate has no contents_case (simple/combination), so it is invalid.
    Predicate predicate;
    predicate.set_id(21);
    optional<InvalidConfigReason> invalidConfigReason;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    EXPECT_EQ(createConditionTracker(key, predicate, invalidEntities, invalidConfigReason),
              nullptr);
    EXPECT_EQ(invalidConfigReason,
              createInvalidConfigReasonWithPredicate(
                      INVALID_CONFIG_REASON_CONDITION_MALFORMED_CONTENTS_CASE, predicate.id()));
}

TEST_F(MetricsManagerUtilTest, TestCreateConditionTrackerSimple) {
    int64_t id = 987;
    const ConfigKey key(123, 456);

    int startMatcherIndex = 2, stopMatcherIndex = 0, stopAllMatcherIndex = 1;
    int64_t startMatcherId = 246, stopMatcherId = 153, stopAllMatcherId = 975;

    Predicate predicate;
    predicate.set_id(id);
    SimplePredicate* simplePredicate = predicate.mutable_simple_predicate();
    simplePredicate->set_start(startMatcherId);
    simplePredicate->set_stop(stopMatcherId);
    simplePredicate->set_stop_all(stopAllMatcherId);

    unordered_map<int64_t, int> atomTrackerMap;
    atomTrackerMap[startMatcherId] = startMatcherIndex;
    atomTrackerMap[stopMatcherId] = stopMatcherIndex;
    atomTrackerMap[stopAllMatcherId] = stopAllMatcherIndex;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    optional<InvalidConfigReason> invalidConfigReason;
    sp<ConditionTracker> tracker =
            createConditionTracker(key, predicate, invalidEntities, invalidConfigReason);

    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    allConditionsMap[predicate.id()] = {predicate, tracker};

    vector<sp<ConditionTracker>> allConditionTrackers = {tracker};

    unordered_map<int64_t, int> conditionIdIndexMap;
    conditionIdIndexMap[predicate.id()] = 0;
    unordered_set<int64_t> initializedConditions;
    vector<ConditionState> conditionCache = {kNotEvaluated};

    tracker->init(/*index=*/0, allConditionsMap, allConditionTrackers, conditionIdIndexMap,
                  atomTrackerMap, initializedConditions, conditionCache);
    EXPECT_EQ(invalidConfigReason, nullopt);
    EXPECT_EQ(tracker->getConditionId(), id);
    EXPECT_EQ(tracker->isSliced(), false);
    EXPECT_TRUE(tracker->IsSimpleCondition());
    const set<int>& interestedMatchers = tracker->getAtomMatchingTrackerIndex();
    ASSERT_EQ(interestedMatchers.size(), 3);
    ASSERT_EQ(interestedMatchers.count(startMatcherIndex), 1);
    ASSERT_EQ(interestedMatchers.count(stopMatcherIndex), 1);
    ASSERT_EQ(interestedMatchers.count(stopAllMatcherIndex), 1);
}

TEST_F(MetricsManagerUtilTest, TestCreateConditionTrackerCombination) {
    int64_t id = 987;
    const ConfigKey key(123, 456);

    Predicate predicate;
    predicate.set_id(id);
    Predicate_Combination* combinationPredicate = predicate.mutable_combination();
    combinationPredicate->set_operation(LogicalOperation::AND);
    combinationPredicate->add_predicate(888);
    combinationPredicate->add_predicate(777);

    // Combination conditions must be initialized to set most state.
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    optional<InvalidConfigReason> invalidConfigReason;
    sp<ConditionTracker> tracker =
            createConditionTracker(key, predicate, invalidEntities, invalidConfigReason);
    EXPECT_EQ(invalidConfigReason, nullopt);
    EXPECT_EQ(tracker->getConditionId(), id);
    EXPECT_FALSE(tracker->IsSimpleCondition());
}

TEST_F(MetricsManagerUtilTest, TestIsNewAlertValidMissingMetric) {
    Alert alert;
    alert.set_id(123);
    alert.set_metric_id(1);
    alert.set_trigger_if_sum_gt(1);
    alert.set_num_buckets(1);

    sp<AlarmMonitor> anomalyAlarmMonitor;
    vector<sp<MetricProducer>> metricProducers;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    // Pass in empty metric producers, causing an error.
    optional<InvalidConfigReason> invalidConfigReason =
            isNewAlertValid(alert, {}, metricProducers, invalidEntities);
    EXPECT_EQ(invalidConfigReason,
              createInvalidConfigReasonWithAlert(INVALID_CONFIG_REASON_ALERT_METRIC_NOT_FOUND,
                                                 alert.metric_id(), alert.id()));
}

TEST_F(MetricsManagerUtilTest, TestIsNewAlertValidNoThreshold) {
    int64_t metricId = 1;
    Alert alert;
    alert.set_id(123);
    alert.set_metric_id(metricId);
    alert.set_num_buckets(1);

    CountMetric metric;
    metric.set_id(metricId);
    metric.set_bucket(ONE_MINUTE);
    sp<MockConditionWizard> wizard = new NaggyMock<MockConditionWizard>();
    sp<MockConfigMetadataProvider> provider = makeMockConfigMetadataProvider(/*enabled=*/false);
    vector<sp<MetricProducer>> metricProducers(
            {new CountMetricProducer(kConfigKey, metric, 0, {ConditionState::kUnknown}, wizard,
                                     0x0123456789, 0, 0, provider)});
    sp<AlarmMonitor> anomalyAlarmMonitor;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    optional<InvalidConfigReason> invalidConfigReason =
            isNewAlertValid(alert, {{1, 0}}, metricProducers, invalidEntities);
    EXPECT_EQ(invalidConfigReason,
              createInvalidConfigReasonWithAlert(INVALID_CONFIG_REASON_ALERT_THRESHOLD_MISSING,
                                                 alert.id()));
}

TEST_F(MetricsManagerUtilTest, TestIsNewAlertValidMissingBuckets) {
    int64_t metricId = 1;
    Alert alert;
    alert.set_id(123);
    alert.set_metric_id(metricId);
    alert.set_trigger_if_sum_gt(1);

    CountMetric metric;
    metric.set_id(metricId);
    metric.set_bucket(ONE_MINUTE);
    sp<MockConditionWizard> wizard = new NaggyMock<MockConditionWizard>();
    sp<MockConfigMetadataProvider> provider = makeMockConfigMetadataProvider(/*enabled=*/false);
    vector<sp<MetricProducer>> metricProducers(
            {new CountMetricProducer(kConfigKey, metric, 0, {ConditionState::kUnknown}, wizard,
                                     0x0123456789, 0, 0, provider)});
    sp<AlarmMonitor> anomalyAlarmMonitor;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    optional<InvalidConfigReason> invalidConfigReason =
            isNewAlertValid(alert, {{1, 0}}, metricProducers, invalidEntities);
    EXPECT_EQ(invalidConfigReason,
              createInvalidConfigReasonWithAlert(
                      INVALID_CONFIG_REASON_ALERT_INVALID_TRIGGER_OR_NUM_BUCKETS, alert.id()));
}

TEST_F(MetricsManagerUtilTest, TestIsNewAlertValidInvalidMetric) {
    int64_t metricId = 1;
    Alert alert;
    alert.set_id(123);
    alert.set_metric_id(metricId);
    alert.set_trigger_if_sum_gt(1);
    alert.set_num_buckets(1);

    CountMetric metric;
    metric.set_id(metricId);
    metric.set_bucket(ONE_MINUTE);
    sp<MockConditionWizard> wizard = new NaggyMock<MockConditionWizard>();
    sp<MockConfigMetadataProvider> provider = makeMockConfigMetadataProvider(/*enabled=*/false);
    vector<sp<MetricProducer>> metricProducers(
            {new CountMetricProducer(kConfigKey, metric, 0, {ConditionState::kUnknown}, wizard,
                                     0x0123456789, 0, 0, provider)});
    sp<AlarmMonitor> anomalyAlarmMonitor;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    invalidEntities[InvalidEntityKey{metricId, INVALID_ENTITY_TYPE_METRIC}] =
            createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_METRIC_MATCHER_NOT_FOUND,
                                                 metricId, /*matcherId=*/0);
    optional<InvalidConfigReason> invalidConfigReason =
            isNewAlertValid(alert, {{1, 0}}, metricProducers, invalidEntities);
    EXPECT_EQ(invalidConfigReason,
              createInvalidConfigReasonWithAlert(
                      INVALID_CONFIG_REASON_ALERT_INVALID_METRIC_DEPENDENCY, metricId, alert.id()));
}

TEST_F(MetricsManagerUtilTest, TestIsNewAlertValidAnomalyTrackerValid) {
    int64_t metricId = 1;
    Alert alert;
    alert.set_id(123);
    alert.set_metric_id(metricId);
    alert.set_trigger_if_sum_gt(1);
    alert.set_num_buckets(1);

    CountMetric metric;
    metric.set_id(metricId);
    metric.set_bucket(ONE_MINUTE);
    sp<MockConditionWizard> wizard = new NaggyMock<MockConditionWizard>();
    sp<MockConfigMetadataProvider> provider = makeMockConfigMetadataProvider(/*enabled=*/false);
    vector<sp<MetricProducer>> metricProducers(
            {new CountMetricProducer(kConfigKey, metric, 0, {ConditionState::kUnknown}, wizard,
                                     0x0123456789, 0, 0, provider)});
    sp<AlarmMonitor> anomalyAlarmMonitor;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    optional<InvalidConfigReason> invalidConfigReason =
            isNewAlertValid(alert, {{1, 0}}, metricProducers, invalidEntities);
    createAnomalyTracker(alert, anomalyAlarmMonitor, UPDATE_NEW, /*updateTime=*/123, {{1, 0}},
                         metricProducers);
    EXPECT_EQ(invalidConfigReason, nullopt);
}

TEST_F(MetricsManagerUtilTest, TestCreateDurationProducerDimensionsInWhatInvalid) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateAcquireWakelockAtomMatcher();
    *config.add_atom_matcher() = CreateReleaseWakelockAtomMatcher();
    *config.add_atom_matcher() = CreateMoveToBackgroundAtomMatcher();
    *config.add_atom_matcher() = CreateMoveToForegroundAtomMatcher();

    Predicate holdingWakelockPredicate = CreateHoldingWakelockPredicate();
    // The predicate is dimensioning by first attribution node by uid.
    FieldMatcher dimensions =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *holdingWakelockPredicate.mutable_simple_predicate()->mutable_dimensions() = dimensions;
    *config.add_predicate() = holdingWakelockPredicate;

    DurationMetric* durationMetric = config.add_duration_metric();
    durationMetric->set_id(StringToId("WakelockDuration"));
    durationMetric->set_what(holdingWakelockPredicate.id());
    durationMetric->set_aggregation_type(DurationMetric::SUM);
    // The metric is dimensioning by first attribution node by uid AND tag.
    // Invalid since the predicate only dimensions by uid.
    *durationMetric->mutable_dimensions_in_what() = CreateAttributionUidAndOtherDimensions(
            util::WAKELOCK_STATE_CHANGED, {Position::FIRST}, {3 /* tag */});
    durationMetric->set_bucket(FIVE_MINUTES);

    ConfigKey key(123, 987);
    uint64_t timeNs = 456;
    sp<StatsPullerManager> pullerManager = new StatsPullerManager();
    sp<AlarmMonitor> anomalyAlarmMonitor;
    sp<AlarmMonitor> periodicAlarmMonitor;
    sp<UidMap> uidMap;
    sp<MetricsManager> metricsManager =
            new MetricsManager(key, config, timeNs, timeNs, uidMap, pullerManager,
                               anomalyAlarmMonitor, periodicAlarmMonitor);

    if (flags::partial_invalid_configs()) {
        // No Metric Initialized.
        EXPECT_TRUE(metricsManager->isConfigValid());
        EXPECT_EQ(metricsManager->getNumMetrics(), 0);
        auto& invalidEntities = metricsManager->mInvalidEntities;
        InvalidConfigReason reason =
                invalidEntities[InvalidEntityKey{durationMetric->id(), INVALID_ENTITY_TYPE_METRIC}];
        EXPECT_EQ(
                reason.reason,
                INVALID_CONFIG_REASON_METRIC_DIMENSIONS_IN_WHAT_NOT_SUBSET_OF_INTERNAL_DIMENSIONS);
        ASSERT_TRUE(reason.metricId.has_value());
        EXPECT_EQ(reason.metricId.value(), durationMetric->id());
    } else {
        EXPECT_FALSE(metricsManager->isConfigValid());
    }
}

TEST_F(MetricsManagerUtilTest, TestSampledMetrics) {
    StatsdConfig config;

    AtomMatcher appCrashMatcher =
            CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);
    *config.add_atom_matcher() = appCrashMatcher;

    *config.add_atom_matcher() = CreateAcquireWakelockAtomMatcher();
    *config.add_atom_matcher() = CreateReleaseWakelockAtomMatcher();

    AtomMatcher bleScanResultReceivedMatcher = CreateSimpleAtomMatcher(
            "BleScanResultReceivedAtomMatcher", util::BLE_SCAN_RESULT_RECEIVED);
    *config.add_atom_matcher() = bleScanResultReceivedMatcher;

    Predicate holdingWakelockPredicate = CreateHoldingWakelockPredicate();
    *holdingWakelockPredicate.mutable_simple_predicate()->mutable_dimensions() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *config.add_predicate() = holdingWakelockPredicate;

    CountMetric sampledCountMetric =
            createCountMetric("CountSampledAppCrashesPerUid", appCrashMatcher.id(), nullopt, {});
    *sampledCountMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *sampledCountMetric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    sampledCountMetric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = sampledCountMetric;

    CountMetric unsampledCountMetric =
            createCountMetric("CountAppCrashesPerUid", appCrashMatcher.id(), nullopt, {});
    *unsampledCountMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *config.add_count_metric() = unsampledCountMetric;

    DurationMetric sampledDurationMetric = createDurationMetric(
            "DurationSampledWakelockPerUid", holdingWakelockPredicate.id(), nullopt, {});
    *sampledDurationMetric.mutable_dimensions_in_what() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *sampledDurationMetric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    sampledDurationMetric.mutable_dimensional_sampling_info()->set_shard_count(4);
    *config.add_duration_metric() = sampledDurationMetric;

    DurationMetric unsampledDurationMetric = createDurationMetric(
            "DurationWakelockPerUid", holdingWakelockPredicate.id(), nullopt, {});
    unsampledDurationMetric.set_aggregation_type(DurationMetric::SUM);
    *unsampledDurationMetric.mutable_dimensions_in_what() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *config.add_duration_metric() = unsampledDurationMetric;

    ValueMetric sampledValueMetric =
            createValueMetric("ValueSampledBleScanResultsPerUid", bleScanResultReceivedMatcher,
                              /*num_results=*/2, nullopt, {});
    *sampledValueMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /* uid */});
    *sampledValueMetric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /*uid*/});
    sampledValueMetric.mutable_dimensional_sampling_info()->set_shard_count(6);
    *config.add_value_metric() = sampledValueMetric;

    ValueMetric unsampledValueMetric =
            createValueMetric("ValueBleScanResultsPerUid", bleScanResultReceivedMatcher,
                              /*num_results=*/2, nullopt, {});
    *unsampledValueMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /* uid */});
    *config.add_value_metric() = unsampledValueMetric;

    KllMetric sampledKllMetric =
            createKllMetric("KllSampledBleScanResultsPerUid", bleScanResultReceivedMatcher,
                            /*num_results=*/2, nullopt);
    *sampledKllMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /* uid */});
    *sampledKllMetric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /*uid*/});
    sampledKllMetric.mutable_dimensional_sampling_info()->set_shard_count(8);
    *config.add_kll_metric() = sampledKllMetric;

    KllMetric unsampledKllMetric = createKllMetric(
            "KllBleScanResultsPerUid", bleScanResultReceivedMatcher, /*num_results=*/2, nullopt);
    *unsampledKllMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /* uid */});
    *config.add_kll_metric() = unsampledKllMetric;

    GaugeMetric sampledGaugeMetric =
            createGaugeMetric("GaugeSampledAppCrashesPerUid", appCrashMatcher.id(),
                              GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    *sampledGaugeMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /* uid */});
    *sampledGaugeMetric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    sampledGaugeMetric.mutable_dimensional_sampling_info()->set_shard_count(10);
    *config.add_gauge_metric() = sampledGaugeMetric;

    GaugeMetric unsampledGaugeMetric =
            createGaugeMetric("GaugeAppCrashesPerUid", appCrashMatcher.id(),
                              GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    *unsampledGaugeMetric.mutable_dimensions_in_what() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /* uid */});
    *config.add_gauge_metric() = unsampledGaugeMetric;

    ConfigKey key(123, 987);
    uint64_t timeNs = 456;
    sp<StatsPullerManager> pullerManager = new StatsPullerManager();
    sp<AlarmMonitor> anomalyAlarmMonitor;
    sp<AlarmMonitor> periodicAlarmMonitor;
    sp<UidMap> uidMap;
    sp<MetricsManager> metricsManager =
            new MetricsManager(key, config, timeNs, timeNs, uidMap, pullerManager,
                               anomalyAlarmMonitor, periodicAlarmMonitor);
    ASSERT_TRUE(metricsManager->isConfigValid());
    ASSERT_EQ(10, metricsManager->mAllMetricProducers.size());

    sp<MetricProducer> sampledCountMetricProducer = metricsManager->mAllMetricProducers[0];
    sp<MetricProducer> unsampledCountMetricProducer = metricsManager->mAllMetricProducers[1];
    sp<MetricProducer> sampledDurationMetricProducer = metricsManager->mAllMetricProducers[2];
    sp<MetricProducer> unsampledDurationMetricProducer = metricsManager->mAllMetricProducers[3];
    sp<MetricProducer> sampledValueMetricProducer = metricsManager->mAllMetricProducers[4];
    sp<MetricProducer> unsampledValueMetricProducer = metricsManager->mAllMetricProducers[5];
    sp<MetricProducer> sampledKllMetricProducer = metricsManager->mAllMetricProducers[6];
    sp<MetricProducer> unsampledKllMetricProducer = metricsManager->mAllMetricProducers[7];
    sp<MetricProducer> sampledGaugeMetricProducer = metricsManager->mAllMetricProducers[8];
    sp<MetricProducer> unsampledGaugeMetricProducer = metricsManager->mAllMetricProducers[9];

    // Check shard count is set correctly for sampled metrics or set to default.
    EXPECT_EQ(2, sampledCountMetricProducer->mShardCount);
    EXPECT_EQ(0, unsampledCountMetricProducer->mShardCount);
    EXPECT_EQ(4, sampledDurationMetricProducer->mShardCount);
    EXPECT_EQ(0, unsampledDurationMetricProducer->mShardCount);
    EXPECT_EQ(6, sampledValueMetricProducer->mShardCount);
    EXPECT_EQ(0, unsampledValueMetricProducer->mShardCount);
    EXPECT_EQ(8, sampledKllMetricProducer->mShardCount);
    EXPECT_EQ(0, unsampledKllMetricProducer->mShardCount);
    EXPECT_EQ(10, sampledGaugeMetricProducer->mShardCount);
    EXPECT_EQ(0, unsampledGaugeMetricProducer->mShardCount);

    // Check sampled what fields is set correctly or empty.
    EXPECT_EQ(1, sampledCountMetricProducer->mSampledWhatFields.size());
    EXPECT_EQ(true, unsampledCountMetricProducer->mSampledWhatFields.empty());
    EXPECT_EQ(1, sampledDurationMetricProducer->mSampledWhatFields.size());
    EXPECT_EQ(true, unsampledDurationMetricProducer->mSampledWhatFields.empty());
    EXPECT_EQ(1, sampledValueMetricProducer->mSampledWhatFields.size());
    EXPECT_EQ(true, unsampledValueMetricProducer->mSampledWhatFields.empty());
    EXPECT_EQ(1, sampledKllMetricProducer->mSampledWhatFields.size());
    EXPECT_EQ(true, unsampledKllMetricProducer->mSampledWhatFields.empty());
    EXPECT_EQ(1, sampledGaugeMetricProducer->mSampledWhatFields.size());
    EXPECT_EQ(true, unsampledGaugeMetricProducer->mSampledWhatFields.empty());
}

TEST_F(MetricsManagerUtilTest, TestMetricHasShardCountButNoSampledField) {
    AtomMatcher appCrashMatcher =
            CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);

    StatsdConfig config;
    *config.add_atom_matcher() = appCrashMatcher;

    CountMetric metric =
            createCountMetric("CountSampledAppCrashesPerUid", appCrashMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_METRIC_DIMENSIONAL_SAMPLING_INFO_MISSING_SAMPLED_FIELD,
                      metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricHasSampledFieldIncorrectShardCount) {
    AtomMatcher appCrashMatcher =
            CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);

    StatsdConfig config;
    *config.add_atom_matcher() = appCrashMatcher;

    CountMetric metric =
            createCountMetric("CountSampledAppCrashesPerUid", appCrashMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_METRIC_DIMENSIONAL_SAMPLING_INFO_INCORRECT_SHARD_COUNT,
                      metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricHasMultipleSampledFields) {
    AtomMatcher appCrashMatcher =
            CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);

    StatsdConfig config;
    *config.add_atom_matcher() = appCrashMatcher;

    CountMetric metric =
            createCountMetric("CountSampledAppCrashesPerUid", appCrashMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/, 2 /*event_type*/});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELD_INCORRECT_SIZE,
                                  metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_PositionALL) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::ALL});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::ALL});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELD_INCORRECT_SIZE,
                                  metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_PositionANY) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::ANY});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::ANY});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELD_INCORRECT_SIZE,
                                  metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricSampledField_DifferentFieldsNotSubsetDimension) {
    AtomMatcher appCrashMatcher =
            CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);

    StatsdConfig config;
    *config.add_atom_matcher() = appCrashMatcher;

    CountMetric metric =
            createCountMetric("CountSampledAppCrashesPerUid", appCrashMatcher.id(), nullopt, {});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(
            expectedInvalidConfigReason,
            InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELDS_NOT_SUBSET_DIM_IN_WHAT,
                                metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_LastNotSubsetDimensionsFirst) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::FIRST});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::LAST});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(
            expectedInvalidConfigReason,
            InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELDS_NOT_SUBSET_DIM_IN_WHAT,
                                metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_FirstNotSubsetDimensionsLast) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::LAST});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::FIRST});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(
            expectedInvalidConfigReason,
            InvalidConfigReason(INVALID_CONFIG_REASON_METRIC_SAMPLED_FIELDS_NOT_SUBSET_DIM_IN_WHAT,
                                metric.id()));
}

// dimensions_in_what position ALL, sampled_what_field position FIRST
TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_FirstSubsetDimensionsAll) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::ALL});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::FIRST});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;
    EXPECT_TRUE(initConfig(config).empty());
}

// dimensions_in_what position ALL, sampled_what_field position LAST
TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_LastSubsetDimensionsAll) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::ALL});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::LAST});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;
    EXPECT_TRUE(initConfig(config).empty());
}

// dimensions_in_what position FIRST, sampled_what_field position FIRST
TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_FirstSubsetDimensionsFirst) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::FIRST});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::FIRST});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;
    EXPECT_TRUE(initConfig(config).empty());
}

// dimensions_in_what position LAST, sampled_what_field position LAST
TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedSampledField_LastSubsetDimensionsLast) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_dimensions_in_what() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::LAST});
    *metric.mutable_dimensional_sampling_info()->mutable_sampled_what_field() =
            CreateRepeatedDimensions(util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/},
                                     {Position::LAST});
    metric.mutable_dimensional_sampling_info()->set_shard_count(2);
    *config.add_count_metric() = metric;
    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestCountMetricHasRestrictedDelegate) {
    StatsdConfig config;
    config.set_id(12345);
    CountMetric* metric = config.add_count_metric();
    config.set_restricted_metrics_delegate_package_name("com.android.app.test");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{/*configId=*/12345, INVALID_ENTITY_TYPE_CONFIG}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED));
}

TEST_F(MetricsManagerUtilTest, TestDurationMetricHasRestrictedDelegate) {
    StatsdConfig config;
    config.set_id(12345);
    DurationMetric* metric = config.add_duration_metric();
    config.set_restricted_metrics_delegate_package_name("com.android.app.test");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{/*configId=*/12345, INVALID_ENTITY_TYPE_CONFIG}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED));
}

TEST_F(MetricsManagerUtilTest, TestGaugeMetricHasRestrictedDelegate) {
    StatsdConfig config;
    config.set_id(12345);
    GaugeMetric* metric = config.add_gauge_metric();
    config.set_restricted_metrics_delegate_package_name("com.android.app.test");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{/*configId=*/12345, INVALID_ENTITY_TYPE_CONFIG}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricHasRestrictedDelegate) {
    StatsdConfig config;
    config.set_id(12345);
    ValueMetric* metric = config.add_value_metric();
    config.set_restricted_metrics_delegate_package_name("com.android.app.test");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{/*configId=*/12345, INVALID_ENTITY_TYPE_CONFIG}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED));
}

TEST_F(MetricsManagerUtilTest, TestKllMetricHasRestrictedDelegate) {
    StatsdConfig config;
    config.set_id(12345);
    KllMetric* metric = config.add_kll_metric();
    config.set_restricted_metrics_delegate_package_name("com.android.app.test");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{/*configId=*/12345, INVALID_ENTITY_TYPE_CONFIG}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_RESTRICTED_METRIC_NOT_SUPPORTED));
}

TEST_P(MetricsManagerUtilDimLimitTest, TestDimLimit) {
    StatsdConfig config = buildGoodConfig(kConfigId, kAlertId);
    const auto& [configLimit, actualLimit] = GetParam();
    if (configLimit > 0) {
        config.mutable_count_metric(0)->set_max_dimensions_per_bucket(configLimit);
        config.mutable_duration_metric(0)->set_max_dimensions_per_bucket(configLimit);
        config.mutable_gauge_metric(0)->set_max_dimensions_per_bucket(configLimit);
        config.mutable_value_metric(0)->set_max_dimensions_per_bucket(configLimit);
        config.mutable_kll_metric(0)->set_max_dimensions_per_bucket(configLimit);
    }

    // initConfig returns nullopt if config is valid
    EXPECT_TRUE(initConfig(config).empty());
    ASSERT_EQ(5u, allMetricProducers.size());

    sp<MetricProducer> producer =
            allMetricProducers[metricProducerMap.at(config.count_metric(0).id())];
    CountMetricProducer* countProducer = static_cast<CountMetricProducer*>(producer.get());
    EXPECT_EQ(countProducer->mDimensionHardLimit, actualLimit);

    producer = allMetricProducers[metricProducerMap.at(config.duration_metric(0).id())];
    DurationMetricProducer* durationProducer = static_cast<DurationMetricProducer*>(producer.get());
    EXPECT_EQ(durationProducer->mDimensionHardLimit, actualLimit);

    producer = allMetricProducers[metricProducerMap.at(config.gauge_metric(0).id())];
    GaugeMetricProducer* gaugeProducer = static_cast<GaugeMetricProducer*>(producer.get());
    EXPECT_EQ(gaugeProducer->mDimensionHardLimit, actualLimit);

    producer = allMetricProducers[metricProducerMap.at(config.value_metric(0).id())];
    NumericValueMetricProducer* numericValueProducer =
            static_cast<NumericValueMetricProducer*>(producer.get());
    EXPECT_EQ(numericValueProducer->mDimensionHardLimit, actualLimit);

    producer = allMetricProducers[metricProducerMap.at(config.kll_metric(0).id())];
    KllMetricProducer* kllProducer = static_cast<KllMetricProducer*>(producer.get());
    EXPECT_EQ(kllProducer->mDimensionHardLimit, actualLimit);
}

TEST_F(MetricsManagerUtilTest, TestMissingValueMatcherAndStringReplacer) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(SCREEN_STATE_ATOM_ID);
    matcher->mutable_simple_atom_matcher()->add_field_value_matcher();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);
    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[(InvalidEntityKey{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER})];
    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_NO_VALUE_MATCHER_NOR_STRING_REPLACER);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestMatcherWithValueMatcherOnly) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(SCREEN_STATE_ATOM_ID);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(2 /*int_field*/);
    fvm->set_eq_int(1);

    ASSERT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestMatcherWithStringReplacerOnly) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(SCREEN_STATE_ATOM_ID);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(5 /*string_field*/);
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("#");

    ASSERT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestValueMatcherWithPositionAll) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(9 /*repeated_int_field*/);
    fvm->set_position(Position::ALL);
    fvm->set_eq_int(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_VALUE_MATCHER_WITH_POSITION_ALL);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestValueMatcherAndStringReplaceWithPositionAll) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(12 /*repeated_string_field*/);
    fvm->set_position(Position::ALL);
    fvm->set_eq_string("foo");
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_VALUE_MATCHER_WITH_POSITION_ALL);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestValueMatcherWithPositionAllNested) {
    StatsdConfig config;
    config.set_id(12345);

    // Match on attribution_node[ALL].uid = 1
    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(1 /*attribution_node*/);
    fvm->set_position(Position::ALL);
    fvm->mutable_matches_tuple()->add_field_value_matcher()->set_field(1 /* uid */);
    fvm->mutable_matches_tuple()->mutable_field_value_matcher(0)->set_eq_int(1);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_VALUE_MATCHER_WITH_POSITION_ALL);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestValueMatcherAndStringReplaceWithPositionAllNested) {
    StatsdConfig config;
    config.set_id(12345);

    // Match on attribution_node[ALL].uid = 1
    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(1 /*attribution_node*/);
    fvm->set_position(Position::ALL);
    fvm->mutable_matches_tuple()->add_field_value_matcher()->set_field(2 /* tag */);
    fvm->mutable_matches_tuple()->mutable_field_value_matcher(0)->set_eq_string("foo");
    fvm->mutable_matches_tuple()
            ->mutable_field_value_matcher(0)
            ->mutable_replace_string()
            ->set_regex(R"([0-9]+$)");
    fvm->mutable_matches_tuple()
            ->mutable_field_value_matcher(0)
            ->mutable_replace_string()
            ->set_replacement("");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_VALUE_MATCHER_WITH_POSITION_ALL);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestStringReplaceWithNoValueMatcherWithPositionAny) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(12 /*repeated_string_field*/);
    fvm->set_position(Position::ANY);
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_STRING_REPLACE_WITH_NO_VALUE_MATCHER_WITH_POSITION_ANY);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestStringReplaceWithNoValueMatcherWithPositionAnyNested) {
    StatsdConfig config;
    config.set_id(12345);

    // Match on attribution_node[ALL].uid = 1
    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(1 /*attribution_node*/);
    fvm->set_position(Position::ANY);
    fvm->mutable_matches_tuple()->add_field_value_matcher()->set_field(2 /* tag */);
    fvm->mutable_matches_tuple()
            ->mutable_field_value_matcher(0)
            ->mutable_replace_string()
            ->set_regex(R"([0-9]+$)");
    fvm->mutable_matches_tuple()
            ->mutable_field_value_matcher(0)
            ->mutable_replace_string()
            ->set_replacement("");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_STRING_REPLACE_WITH_NO_VALUE_MATCHER_WITH_POSITION_ANY);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestStringReplaceWithValueMatcherWithPositionAny) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(12 /*repeated_string_field*/);
    fvm->set_position(Position::ANY);
    fvm->set_eq_string("bar");
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("");

    ASSERT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestStringReplaceWithValueMatcherWithPositionAnyNested) {
    StatsdConfig config;
    config.set_id(12345);

    // Match on attribution_node[ALL].uid = 1
    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(1 /*attribution_node*/);
    fvm->set_position(Position::ANY);
    fvm->mutable_matches_tuple()->add_field_value_matcher()->set_field(2 /* tag */);
    fvm->mutable_matches_tuple()->mutable_field_value_matcher(0)->set_eq_string("bar");
    fvm->mutable_matches_tuple()
            ->mutable_field_value_matcher(0)
            ->mutable_replace_string()
            ->set_regex(R"([0-9]+$)");
    fvm->mutable_matches_tuple()
            ->mutable_field_value_matcher(0)
            ->mutable_replace_string()
            ->set_replacement("");

    ASSERT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestStringReplaceWithPositionAllNested) {
    StatsdConfig config;
    config.set_id(12345);

    // Replace attribution_node[ALL].tag using "[0-9]+$" -> "".
    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(1 /*attribution_node*/);
    fvm->set_position(Position::ALL);
    fvm = fvm->mutable_matches_tuple()->add_field_value_matcher();
    fvm->set_field(2 /* tag */);
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("");

    ASSERT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestMatcherWithStringReplaceAndNonStringValueMatcher) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(2 /*int_field*/);
    fvm->set_eq_int(1);
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("#");

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/111, INVALID_ENTITY_TYPE_MATCHER}];

    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_INVALID_VALUE_MATCHER_WITH_STRING_REPLACE);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(111));
}

TEST_F(MetricsManagerUtilTest, TestCombinationMatcherWithStringReplace) {
    StatsdConfig config;
    config.set_id(12345);

    AtomMatcher* matcher = config.add_atom_matcher();
    matcher->set_id(111);
    matcher->mutable_simple_atom_matcher()->set_atom_id(util::TEST_ATOM_REPORTED);
    FieldValueMatcher* fvm = matcher->mutable_simple_atom_matcher()->add_field_value_matcher();
    fvm->set_field(5 /*string_field*/);
    fvm->mutable_replace_string()->set_regex(R"([0-9]+$)");
    fvm->mutable_replace_string()->set_replacement("#");

    matcher = config.add_atom_matcher();
    matcher->set_id(222);
    matcher->mutable_combination()->set_operation(LogicalOperation::NOT);
    matcher->mutable_combination()->add_matcher(111);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason actualInvalidConfigReason =
            invalidEntities[{/*matcherId=*/222, INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(actualInvalidConfigReason.reason,
              INVALID_CONFIG_REASON_MATCHER_COMBINATION_WITH_STRING_REPLACE);
    EXPECT_THAT(actualInvalidConfigReason.matcherIds, ElementsAre(222));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricMissingHistogramBinConfigSingleAggType) {
    StatsdConfig config = createHistogramStatsdConfig();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_COUNT_DNE_HIST_BIN_CONFIGS_COUNT,
                      config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricMissingHistogramBinConfigMultipleAggTypes) {
    StatsdConfig config = createHistogramStatsdConfig();
    config.mutable_value_metric(0)->clear_aggregation_type();
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::SUM);
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::HISTOGRAM);
    config.mutable_value_metric(0)->mutable_value_field()->add_child()->set_field(2);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_COUNT_DNE_HIST_BIN_CONFIGS_COUNT,
                      config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricExtraHistogramBinConfig) {
    StatsdConfig config = createExplicitHistogramStatsdConfig({5, 10, 12});
    *config.mutable_value_metric(0)->add_histogram_bin_configs() =
            createExplicitBinConfig(/* id */ 1, /* bins */ {5, 10, 20});

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_COUNT_DNE_HIST_BIN_CONFIGS_COUNT,
                      config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricHistogramMultipleValueFields) {
    StatsdConfig config = createExplicitHistogramStatsdConfig({5, 10, 12});
    config.mutable_value_metric(0)->mutable_value_field()->add_child()->set_field(2);

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestNumericValueMetricHistogramWithUploadThreshold) {
    StatsdConfig config = createExplicitHistogramStatsdConfig({5, 10, 12});
    config.mutable_value_metric(0)->mutable_threshold()->set_lt_float(1.0);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_WITH_UPLOAD_THRESHOLD,
                                  config.value_metric(0).id()));

    clearData();
    config.mutable_value_metric(0)->clear_aggregation_type();
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::HISTOGRAM);
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::SUM);
    config.mutable_value_metric(0)->mutable_value_field()->add_child()->set_field(2);

    invalidEntities = initConfig(config);

    expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_WITH_UPLOAD_THRESHOLD,
                                  config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest,
       TestValueMetricValueFieldHasPositionAllWithStatsdAggregatedHistogram) {
    StatsdConfig config = createExplicitHistogramStatsdConfig({5, 10, 12});
    config.mutable_value_metric(0)->mutable_value_field()->mutable_child(0)->set_position(ALL);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_VALUE_FIELD_HAS_POSITION_ALL,
                                  config.value_metric(0).id()));

    clearData();
    config.mutable_value_metric(0)->clear_aggregation_type();
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::HISTOGRAM);
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::SUM);
    config.mutable_value_metric(0)->mutable_value_field()->add_child()->set_field(2);

    invalidEntities = initConfig(config);

    expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_VALUE_FIELD_HAS_POSITION_ALL,
                                  config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest,
       TestValueMetricValueFieldHasNoPositionAllWithClientAggregatedHistogram) {
    StatsdConfig config = createHistogramStatsdConfig();
    config.mutable_value_metric(0)->add_histogram_bin_configs()->set_id(1);
    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_client_aggregated_bins();

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_CLIENT_AGGREGATED_NO_POSITION_ALL,
                      config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest,
       TestValueMetricValueFieldHasPositionAllWithClientAggregatedHistogram) {
    StatsdConfig config = createHistogramStatsdConfig();
    config.mutable_value_metric(0)->add_histogram_bin_configs()->set_id(1);
    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_client_aggregated_bins();
    config.mutable_value_metric(0)->mutable_value_field()->mutable_child(0)->set_position(ALL);

    EXPECT_TRUE(initConfig(config).empty());
}

TEST_F(MetricsManagerUtilTest, TestValueMetricHistogramWithValueDirectionNotIncreasing) {
    StatsdConfig config = createHistogramStatsdConfig();
    config.mutable_value_metric(0)->mutable_value_field()->mutable_child(0)->set_position(ALL);
    config.mutable_value_metric(0)->add_histogram_bin_configs()->set_id(1);
    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_client_aggregated_bins();
    config.mutable_value_metric(0)->set_value_direction(ValueMetric::DECREASING);

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_INVALID_VALUE_DIRECTION,
                                  config.value_metric(0).id()));

    clearData();
    config.mutable_value_metric(0)->clear_aggregation_type();
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::SUM);
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::HISTOGRAM);
    config.mutable_value_metric(0)->mutable_value_field()->mutable_child(0)->clear_position();
    config.mutable_value_metric(0)->mutable_value_field()->add_child()->set_field(2);
    config.mutable_value_metric(0)->mutable_value_field()->mutable_child(1)->set_position(ALL);
    config.mutable_value_metric(0)->set_value_direction(ValueMetric::ANY);

    invalidEntities = initConfig(config);

    expectedInvalidConfigReason =
            invalidEntities[{config.value_metric(0).id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_INVALID_VALUE_DIRECTION,
                                  config.value_metric(0).id()));
}

TEST_F(MetricsManagerUtilTest, TestUidFields) {
    StatsdConfig config;

    AtomMatcher appCrashMatcher =
            CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);
    *config.add_atom_matcher() = appCrashMatcher;

    *config.add_atom_matcher() = CreateAcquireWakelockAtomMatcher();
    *config.add_atom_matcher() = CreateReleaseWakelockAtomMatcher();

    AtomMatcher bleScanResultReceivedMatcher =
            CreateSimpleAtomMatcher("Ble", util::BLE_SCAN_RESULT_RECEIVED);
    *config.add_atom_matcher() = bleScanResultReceivedMatcher;

    Predicate holdingWakelockPredicate = CreateHoldingWakelockPredicate();
    *holdingWakelockPredicate.mutable_simple_predicate()->mutable_dimensions() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *config.add_predicate() = holdingWakelockPredicate;

    CountMetric uidCountMetric = createCountMetric("C1", appCrashMatcher.id(), nullopt, {});
    *uidCountMetric.mutable_uid_fields() = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *config.add_count_metric() = uidCountMetric;

    CountMetric countMetric = createCountMetric("C2", appCrashMatcher.id(), nullopt, {});
    *config.add_count_metric() = countMetric;

    DurationMetric uidDurationMetric =
            createDurationMetric("D1", holdingWakelockPredicate.id(), nullopt, {});
    *uidDurationMetric.mutable_uid_fields() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *config.add_duration_metric() = uidDurationMetric;

    DurationMetric durationMetric =
            createDurationMetric("D2", holdingWakelockPredicate.id(), nullopt, {});
    *config.add_duration_metric() = durationMetric;

    EventMetric uidEventMetric = createEventMetric("E1", appCrashMatcher.id(), nullopt);
    *uidEventMetric.mutable_uid_fields() = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    *config.add_event_metric() = uidEventMetric;

    EventMetric eventMetric = createEventMetric("E2", appCrashMatcher.id(), nullopt);
    *config.add_event_metric() = eventMetric;

    ValueMetric uidValueMetric = createValueMetric("V1", bleScanResultReceivedMatcher,
                                                   /*num_results=*/2, nullopt, {});
    *uidValueMetric.mutable_uid_fields() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /* uid */});
    *config.add_value_metric() = uidValueMetric;

    ValueMetric valueMetric = createValueMetric("V2", bleScanResultReceivedMatcher,
                                                /*num_results=*/2, nullopt, {});
    *config.add_value_metric() = valueMetric;

    KllMetric uidKllMetric = createKllMetric("K1", bleScanResultReceivedMatcher,
                                             /*num_results=*/2, nullopt);
    *uidKllMetric.mutable_uid_fields() =
            CreateDimensions(util::BLE_SCAN_RESULT_RECEIVED, {1 /* uid */});
    *config.add_kll_metric() = uidKllMetric;

    KllMetric kllMetric =
            createKllMetric("K2", bleScanResultReceivedMatcher, /*num_results=*/2, nullopt);
    *config.add_kll_metric() = kllMetric;

    GaugeMetric uidGaugeMetric = createGaugeMetric("G1", appCrashMatcher.id(),
                                                   GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    *uidGaugeMetric.mutable_uid_fields() =
            CreateDimensions(util::APP_CRASH_OCCURRED, {1 /* uid */});
    *config.add_gauge_metric() = uidGaugeMetric;

    GaugeMetric gaugeMetric = createGaugeMetric("G2", appCrashMatcher.id(),
                                                GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    *config.add_gauge_metric() = gaugeMetric;

    ConfigKey key(123, 987);
    uint64_t timeNs = 456;
    sp<StatsPullerManager> pullerManager = new StatsPullerManager();
    sp<AlarmMonitor> anomalyAlarmMonitor;
    sp<AlarmMonitor> periodicAlarmMonitor;
    sp<UidMap> uidMap;
    sp<MetricsManager> metricsManager =
            new MetricsManager(key, config, timeNs, timeNs, uidMap, pullerManager,
                               anomalyAlarmMonitor, periodicAlarmMonitor);
    ASSERT_TRUE(metricsManager->isConfigValid());
    ASSERT_EQ(12, metricsManager->mAllMetricProducers.size());

    sp<MetricProducer> uidCountMetricProducer = metricsManager->mAllMetricProducers[0];
    sp<MetricProducer> countMetricProducer = metricsManager->mAllMetricProducers[1];
    sp<MetricProducer> uidDurationMetricProducer = metricsManager->mAllMetricProducers[2];
    sp<MetricProducer> durationMetricProducer = metricsManager->mAllMetricProducers[3];
    sp<MetricProducer> uidEventMetricProducer = metricsManager->mAllMetricProducers[4];
    sp<MetricProducer> eventMetricProducer = metricsManager->mAllMetricProducers[5];
    sp<MetricProducer> uidValueMetricProducer = metricsManager->mAllMetricProducers[6];
    sp<MetricProducer> valueMetricProducer = metricsManager->mAllMetricProducers[7];
    sp<MetricProducer> uidKllMetricProducer = metricsManager->mAllMetricProducers[8];
    sp<MetricProducer> kllMetricProducer = metricsManager->mAllMetricProducers[9];
    sp<MetricProducer> uidGaugeMetricProducer = metricsManager->mAllMetricProducers[10];
    sp<MetricProducer> gaugeMetricProducer = metricsManager->mAllMetricProducers[11];

    // Check uid what fields is set correctly or empty.
    EXPECT_EQ(1, uidCountMetricProducer->mUidFields.size());
    EXPECT_EQ(true, countMetricProducer->mUidFields.empty());
    EXPECT_EQ(1, uidDurationMetricProducer->mUidFields.size());
    EXPECT_EQ(true, durationMetricProducer->mUidFields.empty());
    EXPECT_EQ(1, uidEventMetricProducer->mUidFields.size());
    EXPECT_EQ(true, eventMetricProducer->mUidFields.empty());
    EXPECT_EQ(1, uidValueMetricProducer->mUidFields.size());
    EXPECT_EQ(true, valueMetricProducer->mUidFields.empty());
    EXPECT_EQ(1, uidKllMetricProducer->mUidFields.size());
    EXPECT_EQ(true, kllMetricProducer->mUidFields.empty());
    EXPECT_EQ(1, uidGaugeMetricProducer->mUidFields.size());
    EXPECT_EQ(true, gaugeMetricProducer->mUidFields.empty());
}

TEST_F(MetricsManagerUtilTest, TestMetricHasRepeatedUidField_PositionANY) {
    AtomMatcher testAtomReportedMatcher =
            CreateSimpleAtomMatcher("TEST_ATOM_REPORTED", util::TEST_ATOM_REPORTED);

    StatsdConfig config;
    *config.add_atom_matcher() = testAtomReportedMatcher;

    CountMetric metric = createCountMetric("CountSampledTestAtomReportedPerRepeatedIntField",
                                           testAtomReportedMatcher.id(), nullopt, {});
    *metric.mutable_uid_fields() = CreateRepeatedDimensions(
            util::TEST_ATOM_REPORTED, {9 /*repeated_int_field*/}, {Position::ANY});
    *config.add_count_metric() = metric;

    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities = initConfig(config);

    InvalidConfigReason expectedInvalidConfigReason =
            invalidEntities[{metric.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(expectedInvalidConfigReason,
              InvalidConfigReason(INVALID_CONFIG_REASON_UID_FIELDS_WITH_POSITION_ANY, metric.id()));
}

TEST_F(MetricsManagerUtilTest, TestInitMatchersNotesMultipleInvalidEntities) {
    StatsdConfig config;

    AtomMatcher simple1 = CreateSimpleAtomMatcher("SIMPLE1", /*atom=*/10);
    int64_t simple1Id = simple1.id();
    *config.add_atom_matcher() = simple1;

    // duplicate to simple1
    AtomMatcher simple2 = CreateSimpleAtomMatcher("SIMPLE1", /*atom=*/10);
    *config.add_atom_matcher() = simple2;

    // No atom matcher id.
    AtomMatcher simple3 = CreateSimpleAtomMatcher("SIMPLE3", /*atom=*/12);
    simple3.mutable_simple_atom_matcher()->clear_atom_id();
    *config.add_atom_matcher() = simple3;

    // valid matcher
    AtomMatcher simple4 = CreateSimpleAtomMatcher("SIMPLE4", /*atom=*/13);
    *config.add_atom_matcher() = simple4;

    unordered_map<int, vector<int>> tagIds;
    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    vector<sp<AtomMatchingTracker>> newAtomMatchingTrackers;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    EXPECT_FALSE(initAtomMatchingTrackers(config, uidMap, newAtomMatchingTrackerMap,
                                          newAtomMatchingTrackers, tagIds, invalidEntities));

    EXPECT_EQ(invalidEntities.size(), 2);
    EXPECT_EQ(invalidEntities[(InvalidEntityKey{simple1.id(), INVALID_ENTITY_TYPE_MATCHER})],
              createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_DUPLICATE,
                                                   simple1.id()));
    EXPECT_EQ(invalidEntities[(InvalidEntityKey{simple3.id(), INVALID_ENTITY_TYPE_MATCHER})],
              createInvalidConfigReasonWithMatcher(
                      INVALID_CONFIG_REASON_MATCHER_TRACKER_NOT_INITIALIZED, simple3.id()));

    EXPECT_EQ(newAtomMatchingTrackerMap.size(), 1);
    EXPECT_EQ(newAtomMatchingTrackerMap[simple4.id()], 0);
    EXPECT_EQ(newAtomMatchingTrackers[0]->getId(), simple4.id());
    EXPECT_EQ(tagIds.size(), 1);
    EXPECT_THAT(tagIds[13], UnorderedElementsAreArray(filterMatcherIndexesById(
                                    newAtomMatchingTrackers, {simple4.id()})));
}

TEST_F(MetricsManagerUtilTest, TestInitMatchersHasCycle) {
    StatsdConfig config;

    AtomMatcher matcher1;
    matcher1.set_id(StringToId("TEST1"));
    AtomMatcher matcher2;
    matcher2.set_id(StringToId("TEST2"));
    AtomMatcher matcher3 = CreateSimpleAtomMatcher("SIMPLE1", /*atom=*/10);

    AtomMatcher_Combination* combination1 = matcher1.mutable_combination();
    combination1->set_operation(LogicalOperation::OR);
    combination1->add_matcher(matcher2.id());
    combination1->add_matcher(matcher3.id());

    AtomMatcher_Combination* combination2 = matcher2.mutable_combination();
    combination2->set_operation(LogicalOperation::OR);
    combination2->add_matcher(matcher1.id());
    combination2->add_matcher(matcher3.id());

    *config.add_atom_matcher() = matcher1;
    *config.add_atom_matcher() = matcher2;
    *config.add_atom_matcher() = matcher3;

    unordered_map<int, vector<int>> tagIds;
    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    vector<sp<AtomMatchingTracker>> newAtomMatchingTrackers;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    EXPECT_FALSE(initAtomMatchingTrackers(config, uidMap, newAtomMatchingTrackerMap,
                                          newAtomMatchingTrackers, tagIds, invalidEntities));

    EXPECT_EQ(invalidEntities.size(), 2);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{matcher1.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_CYCLE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAreArray({matcher1.id(), matcher2.id()}));

    reason = invalidEntities[InvalidEntityKey{matcher2.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_CYCLE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAreArray({matcher1.id(), matcher2.id()}));

    EXPECT_EQ(newAtomMatchingTrackerMap.size(), 1);
    EXPECT_EQ(newAtomMatchingTrackerMap[matcher3.id()], 0);
    EXPECT_EQ(newAtomMatchingTrackers[0]->getId(), matcher3.id());

    EXPECT_EQ(tagIds.size(), 1);
    EXPECT_THAT(tagIds[10], UnorderedElementsAreArray(filterMatcherIndexesById(
                                    newAtomMatchingTrackers, {matcher3.id()})));
}

TEST_F(MetricsManagerUtilTest, TestInitMatchersNoCycle) {
    StatsdConfig config;

    AtomMatcher matcher1 = CreateSimpleAtomMatcher("SIMPLE1", /*atom=*/10);
    AtomMatcher matcher2 = CreateSimpleAtomMatcher("SIMPLE2", /*atom=*/11);
    AtomMatcher matcher3 = CreateSimpleAtomMatcher("SIMPLE3", /*atom=*/12);
    AtomMatcher matcher4;
    matcher4.set_id(StringToId("TEST1"));
    AtomMatcher matcher5;
    matcher5.set_id(StringToId("TEST2"));

    AtomMatcher_Combination* combination1 = matcher4.mutable_combination();
    combination1->set_operation(LogicalOperation::OR);
    combination1->add_matcher(matcher1.id());
    combination1->add_matcher(matcher2.id());

    AtomMatcher_Combination* combination2 = matcher5.mutable_combination();
    combination2->set_operation(LogicalOperation::OR);
    combination2->add_matcher(matcher1.id());
    combination2->add_matcher(matcher3.id());

    *config.add_atom_matcher() = matcher1;
    *config.add_atom_matcher() = matcher2;
    *config.add_atom_matcher() = matcher3;
    *config.add_atom_matcher() = matcher4;
    *config.add_atom_matcher() = matcher5;

    unordered_map<int, vector<int>> tagIds;
    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    vector<sp<AtomMatchingTracker>> newAtomMatchingTrackers;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    EXPECT_TRUE(initAtomMatchingTrackers(config, uidMap, newAtomMatchingTrackerMap,
                                         newAtomMatchingTrackers, tagIds, invalidEntities));

    EXPECT_EQ(newAtomMatchingTrackerMap.size(), 5);
    EXPECT_EQ(newAtomMatchingTrackerMap.at(matcher1.id()), 0);
    EXPECT_EQ(newAtomMatchingTrackerMap.at(matcher2.id()), 1);
    EXPECT_EQ(newAtomMatchingTrackerMap.at(matcher3.id()), 2);
    EXPECT_EQ(newAtomMatchingTrackerMap.at(matcher4.id()), 3);
    EXPECT_EQ(newAtomMatchingTrackerMap.at(matcher5.id()), 4);

    EXPECT_EQ(tagIds.size(), 3);
    EXPECT_THAT(tagIds[10],
                UnorderedElementsAreArray(filterMatcherIndexesById(
                        newAtomMatchingTrackers, {matcher1.id(), matcher4.id(), matcher5.id()})));
    EXPECT_THAT(tagIds[11], UnorderedElementsAreArray(filterMatcherIndexesById(
                                    newAtomMatchingTrackers, {matcher2.id(), matcher4.id()})));
    EXPECT_THAT(tagIds[12], UnorderedElementsAreArray(filterMatcherIndexesById(
                                    newAtomMatchingTrackers, {matcher3.id(), matcher5.id()})));
}

TEST_F(MetricsManagerUtilTest, TestInitConditionsNoCycle) {
    StatsdConfig config;

    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateBatterySaverModeStartAtomMatcher();
    int64_t matcher4Id = matcher3.id();
    *config.add_atom_matcher() = matcher3;

    AtomMatcher matcher4 = CreateBatterySaverModeStopAtomMatcher();
    int64_t matcher3Id = matcher4.id();
    *config.add_atom_matcher() = matcher4;

    Predicate predicate1;
    predicate1.set_id(StringToId("TEST1"));
    Predicate predicate2;
    predicate2.set_id(StringToId("TEST2"));
    Predicate predicate3 = CreateScreenIsOnPredicate();
    Predicate predicate4 = CreateScreenIsOffPredicate();
    Predicate predicate5 = CreateBatterySaverModePredicate();

    Predicate_Combination* combination1 = predicate1.mutable_combination();
    combination1->set_operation(LogicalOperation::OR);
    combination1->add_predicate(predicate3.id());
    combination1->add_predicate(predicate4.id());

    Predicate_Combination* combination2 = predicate2.mutable_combination();
    combination2->set_operation(LogicalOperation::OR);
    combination2->add_predicate(predicate3.id());
    combination2->add_predicate(predicate5.id());

    *config.add_predicate() = predicate1;
    *config.add_predicate() = predicate2;
    *config.add_predicate() = predicate3;
    *config.add_predicate() = predicate4;
    *config.add_predicate() = predicate5;

    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    newAtomMatchingTrackerMap[matcher1Id] = 0;
    newAtomMatchingTrackerMap[matcher2Id] = 1;
    newAtomMatchingTrackerMap[matcher3Id] = 2;
    newAtomMatchingTrackerMap[matcher4Id] = 3;

    const ConfigKey key(123, 456);
    unordered_map<int64_t, int> newConditionTrackerMap;
    vector<sp<ConditionTracker>> newConditionTrackers;
    unordered_map<int, vector<int>> trackerToConditionMap;
    vector<ConditionState> conditionCache;
    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    EXPECT_TRUE(initConditions(key, config, newAtomMatchingTrackerMap, newConditionTrackerMap,
                               newConditionTrackers, trackerToConditionMap, conditionCache,
                               allConditionsMap, invalidEntities));

    EXPECT_EQ(newConditionTrackerMap.size(), 5);
    EXPECT_EQ(newConditionTrackerMap.at(predicate1.id()), 0);
    EXPECT_EQ(newConditionTrackerMap.at(predicate2.id()), 1);
    EXPECT_EQ(newConditionTrackerMap.at(predicate3.id()), 2);
    EXPECT_EQ(newConditionTrackerMap.at(predicate4.id()), 3);
    EXPECT_EQ(newConditionTrackerMap.at(predicate5.id()), 4);
}

TEST_F(MetricsManagerUtilTest, TestInitConditionsChildNotValid) {
    StatsdConfig config;

    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    *config.add_atom_matcher() = matcher2;

    Predicate predicate1;
    predicate1.set_id(StringToId("TEST1"));
    Predicate predicate2;  // Predicate 2 has no fields but id set.
    predicate2.set_id(StringToId("TEST2"));
    Predicate predicate3 = CreateScreenIsOnPredicate();

    Predicate_Combination* combination1 = predicate1.mutable_combination();
    combination1->set_operation(LogicalOperation::OR);
    combination1->add_predicate(predicate2.id());
    combination1->add_predicate(predicate3.id());

    *config.add_predicate() = predicate1;
    *config.add_predicate() = predicate2;
    *config.add_predicate() = predicate3;

    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    newAtomMatchingTrackerMap[matcher1Id] = 0;
    newAtomMatchingTrackerMap[matcher2Id] = 1;

    const ConfigKey key(123, 456);
    unordered_map<int64_t, int> newConditionTrackerMap;
    vector<sp<ConditionTracker>> newConditionTrackers;
    unordered_map<int, vector<int>> trackerToConditionMap;
    vector<ConditionState> conditionCache;
    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;
    EXPECT_FALSE(initConditions(key, config, newAtomMatchingTrackerMap, newConditionTrackerMap,
                                newConditionTrackers, trackerToConditionMap, conditionCache,
                                allConditionsMap, invalidEntities));

    EXPECT_EQ(invalidEntities.size(), 2);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{predicate1.id(), INVALID_ENTITY_TYPE_PREDICATE}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_CONDITION_CHILD_NOT_FOUND);
    EXPECT_THAT(reason.conditionIds, UnorderedElementsAreArray({predicate1.id(), predicate2.id()}));

    reason = invalidEntities[InvalidEntityKey{predicate2.id(), INVALID_ENTITY_TYPE_PREDICATE}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_CONDITION_MALFORMED_CONTENTS_CASE);
    EXPECT_THAT(reason.conditionIds, UnorderedElementsAreArray({predicate2.id()}));

    EXPECT_EQ(newConditionTrackerMap.size(), 1);
    EXPECT_EQ(newConditionTrackerMap[predicate3.id()], 0);
    EXPECT_EQ(newConditionTrackers[0]->getConditionId(), predicate3.id());
}

TEST_F(MetricsManagerUtilTest, TestInitConditionDependentMatcherNotValid) {
    StatsdConfig config;

    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    *config.add_atom_matcher() = matcher2;

    Predicate predicate1 = CreateScreenIsOnPredicate();

    *config.add_predicate() = predicate1;

    unordered_map<int64_t, int> newAtomMatchingTrackerMap;
    newAtomMatchingTrackerMap[matcher1Id] = 0;

    const ConfigKey key(123, 456);
    unordered_map<int64_t, int> newConditionTrackerMap;
    vector<sp<ConditionTracker>> newConditionTrackers;
    unordered_map<int, vector<int>> trackerToConditionMap;
    vector<ConditionState> conditionCache;
    unordered_map<int64_t, ConditionProtoAndTracker> allConditionsMap;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    invalidEntities[InvalidEntityKey{matcher2Id, INVALID_ENTITY_TYPE_MATCHER}] =
            createInvalidConfigReasonWithMatcher(INVALID_CONFIG_REASON_MATCHER_SERIALIZATION_FAILED,
                                                 matcher2Id);

    EXPECT_FALSE(initConditions(key, config, newAtomMatchingTrackerMap, newConditionTrackerMap,
                                newConditionTrackers, trackerToConditionMap, conditionCache,
                                allConditionsMap, invalidEntities));

    EXPECT_EQ(invalidEntities.size(), 2);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{predicate1.id(), INVALID_ENTITY_TYPE_PREDICATE}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_CONDITION_INVALID_MATCHER_DEPENDENCY);
    EXPECT_THAT(reason.conditionIds, UnorderedElementsAreArray({predicate1.id()}));
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAreArray({matcher2.id()}));

    EXPECT_EQ(trackerToConditionMap.size(), 0);
    EXPECT_EQ(newConditionTrackerMap.size(), 0);
}

TEST_F(MetricsManagerUtilTest, TestInitCountMetricsHasInvalidMetrics) {
    StatsdConfig config;

    // Add atom matchers/predicates/states. These are mostly needed for initStatsdConfig.
    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    int matcher1Index = 0;
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    int matcher2Index = 1;
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateStartScheduledJobAtomMatcher();
    int64_t matcher3Id = matcher3.id();
    int matcher3Index = 2;
    *config.add_atom_matcher() = matcher3;

    Predicate predicate1 = CreateScreenIsOnPredicate();
    int64_t predicate1Id = predicate1.id();
    int predicate1Index = 0;
    *config.add_predicate() = predicate1;

    State state1 = CreateScreenStateWithOnOffMap(0x123, 0x321);
    int64_t state1Id = state1.id();
    *config.add_state() = state1;

    // Add a few count metrics.
    CountMetric count1 = createCountMetric("COUNT1", matcher1Id, predicate1Id, {state1Id});
    int64_t count1Id = count1.id();
    int count1Index = 0;
    *config.add_count_metric() = count1;

    // Will be invalid due to unknown predicate
    CountMetric count2 = createCountMetric("COUNT2", matcher2Id, /*predicateId=*/-1, {});
    int64_t count2Id = count2.id();
    *config.add_count_metric() = count2;

    CountMetric count3 = createCountMetric("COUNT3", matcher3Id, nullopt, {});
    int64_t count3Id = count3.id();
    int count3Index = 1;
    *config.add_count_metric() = count3;

    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;

    // Output data structures to validate.
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    unordered_map<int64_t, int> expectedMetricProducerMap = {{count1Id, count1Index},
                                                             {count3Id, count3Index}};
    EXPECT_THAT(newMetricProducerMap, ContainerEq(expectedMetricProducerMap));

    ASSERT_EQ(newMetricProducers.size(), 2);

    // Verify the conditionToMetricMap.
    ASSERT_EQ(conditionToMetricMap.size(), 1);
    const vector<int>& condition1Metrics = conditionToMetricMap[predicate1Index];
    EXPECT_THAT(condition1Metrics, UnorderedElementsAre(count1Index));

    // Verify the trackerToMetricMap.
    ASSERT_EQ(trackerToMetricMap.size(), 2);
    const vector<int>& matcher1Metrics = trackerToMetricMap[matcher1Index];
    EXPECT_THAT(matcher1Metrics, UnorderedElementsAre(count1Index));
    const vector<int>& matcher3Metrics = trackerToMetricMap[matcher3Index];
    EXPECT_THAT(matcher3Metrics, UnorderedElementsAre(count3Index));

    // Verify event activation/deactivation maps.
    ASSERT_EQ(activationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(deactivationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(metricsWithActivation.size(), 0);

    // Verify tracker indices/ids/conditions/states are correct.
    EXPECT_EQ(newMetricProducers[count1Index]->getMetricId(), count1Id);
    EXPECT_EQ(newMetricProducers[count1Index]->mConditionTrackerIndex, predicate1Index);
    EXPECT_EQ(newMetricProducers[count1Index]->mCondition, ConditionState::kUnknown);
    EXPECT_THAT(newMetricProducers[count1Index]->getSlicedStateAtoms(),
                UnorderedElementsAre(util::SCREEN_STATE_CHANGED));
    EXPECT_EQ(newMetricProducers[count3Index]->getMetricId(), count3Id);
    EXPECT_EQ(newMetricProducers[count3Index]->mConditionTrackerIndex, -1);
    EXPECT_EQ(newMetricProducers[count3Index]->mCondition, ConditionState::kTrue);
    EXPECT_TRUE(newMetricProducers[count3Index]->getSlicedStateAtoms().empty());

    EXPECT_EQ(invalidEntities.size(), 1);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{count2.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_CONDITION_NOT_FOUND);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), count2.id());
}

TEST_F(MetricsManagerUtilTest, TestInitGaugeMetricsHasInvalidMetrics) {
    StatsdConfig config;

    // Add atom matchers/predicates/states. These are mostly needed for initStatsdConfig.
    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    int matcher1Index = 0;
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    int matcher2Index = 1;
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateStartScheduledJobAtomMatcher();
    int64_t matcher3Id = matcher3.id();
    int matcher3Index = 2;
    *config.add_atom_matcher() = matcher3;

    // Will be marked as invalid
    AtomMatcher matcher4 = CreateTemperatureAtomMatcher();
    int64_t matcher4Id = matcher4.id();
    matcher4.clear_simple_atom_matcher();
    *config.add_atom_matcher() = matcher4;

    AtomMatcher matcher5 = CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);
    int64_t matcher5Id = matcher5.id();
    int matcher5Index = 3;
    *config.add_atom_matcher() = matcher5;

    Predicate predicate1 = CreateScreenIsOnPredicate();
    int64_t predicate1Id = predicate1.id();
    int predicate1Index = 0;
    *config.add_predicate() = predicate1;

    // Add a few gauge metrics.
    // Will be invalid due to invalid matcher dependency
    GaugeMetric gauge1 = createGaugeMetric("GAUGE1", matcher4Id, GaugeMetric::FIRST_N_SAMPLES,
                                           predicate1Id, matcher1Id);
    int64_t gauge1Id = gauge1.id();
    *config.add_gauge_metric() = gauge1;

    GaugeMetric gauge2 =
            createGaugeMetric("GAUGE2", matcher1Id, GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    int64_t gauge2Id = gauge2.id();
    int gauge2Index = 0;
    *config.add_gauge_metric() = gauge2;

    // Will be invalid due to missing what.
    GaugeMetric gauge3 = createGaugeMetric("GAUGE3", /*matcherId=*/-1, GaugeMetric::FIRST_N_SAMPLES,
                                           nullopt, matcher3Id);
    int64_t gauge3Id = gauge3.id();
    *config.add_gauge_metric() = gauge3;

    GaugeMetric gauge4 = createGaugeMetric("GAUGE4", matcher3Id, GaugeMetric::RANDOM_ONE_SAMPLE,
                                           predicate1Id, nullopt);
    int gauge4Index = 1;
    int64_t gauge4Id = gauge4.id();
    *config.add_gauge_metric() = gauge4;

    GaugeMetric gauge5 =
            createGaugeMetric("GAUGE5", matcher2Id, GaugeMetric::RANDOM_ONE_SAMPLE, nullopt, {});
    int64_t gauge5Id = gauge5.id();
    int gauge5Index = 2;
    *config.add_gauge_metric() = gauge5;

    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;

    // Output data structures to validate.
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    unordered_map<int64_t, int> expectedMetricProducerMap = {
            {gauge2Id, gauge2Index},
            {gauge4Id, gauge4Index},
            {gauge5Id, gauge5Index},
    };
    EXPECT_THAT(newMetricProducerMap, ContainerEq(expectedMetricProducerMap));

    ASSERT_EQ(newMetricProducers.size(), 3);

    // Verify the conditionToMetricMap.
    ASSERT_EQ(conditionToMetricMap.size(), 1);
    const vector<int>& condition1Metrics = conditionToMetricMap[predicate1Index];
    EXPECT_THAT(condition1Metrics, UnorderedElementsAre(gauge4Index));

    // Verify the trackerToMetricMap.
    ASSERT_EQ(trackerToMetricMap.size(), 3);
    const vector<int>& matcher1Metrics = trackerToMetricMap[matcher1Index];
    EXPECT_THAT(matcher1Metrics, UnorderedElementsAre(gauge2Index));
    const vector<int>& matcher2Metrics = trackerToMetricMap[matcher2Index];
    EXPECT_THAT(matcher2Metrics, UnorderedElementsAre(gauge5Index));
    const vector<int>& matcher3Metrics = trackerToMetricMap[matcher3Index];
    EXPECT_THAT(matcher3Metrics, UnorderedElementsAre(gauge4Index));

    // Verify event activation/deactivation maps.
    ASSERT_EQ(activationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(deactivationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(metricsWithActivation.size(), 0);

    // Verify tracker indices/ids/conditions/states are correct.
    GaugeMetricProducer* gaugeProducer2 =
            static_cast<GaugeMetricProducer*>(newMetricProducers[gauge2Index].get());
    EXPECT_EQ(gaugeProducer2->getMetricId(), gauge2Id);
    EXPECT_EQ(gaugeProducer2->mConditionTrackerIndex, -1);
    EXPECT_EQ(gaugeProducer2->mCondition, ConditionState::kTrue);
    EXPECT_EQ(gaugeProducer2->mWhatMatcherIndex, matcher1Index);
    GaugeMetricProducer* gaugeProducer4 =
            static_cast<GaugeMetricProducer*>(newMetricProducers[gauge4Index].get());
    EXPECT_EQ(gaugeProducer4->getMetricId(), gauge4Id);
    EXPECT_EQ(gaugeProducer4->mConditionTrackerIndex, predicate1Index);
    EXPECT_EQ(gaugeProducer4->mCondition, ConditionState::kUnknown);
    EXPECT_EQ(gaugeProducer4->mWhatMatcherIndex, matcher3Index);
    GaugeMetricProducer* gaugeProducer5 =
            static_cast<GaugeMetricProducer*>(newMetricProducers[gauge5Index].get());
    EXPECT_EQ(gaugeProducer5->getMetricId(), gauge5Id);
    EXPECT_EQ(gaugeProducer5->mConditionTrackerIndex, -1);
    EXPECT_EQ(gaugeProducer5->mCondition, ConditionState::kTrue);
    EXPECT_EQ(gaugeProducer5->mWhatMatcherIndex, matcher2Index);

    EXPECT_EQ(invalidEntities.size(), 3);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{gauge3.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_MATCHER_NOT_FOUND);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), gauge3.id());
    reason = invalidEntities[InvalidEntityKey{gauge1.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_INVALID_MATCHER_DEPENDENCY);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), gauge1.id());
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
    reason = invalidEntities[InvalidEntityKey{matcher4.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
}

TEST_F(MetricsManagerUtilTest, TestInitDurationMetricsHasInvalidMetrics) {
    StatsdConfig config;
    // Add atom matchers/predicates/states. These are mostly needed for initStatsdConfig.
    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    int matcher1Index = 0;
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    int matcher2Index = 1;
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateAcquireWakelockAtomMatcher();
    int64_t matcher3Id = matcher3.id();
    int matcher3Index = 2;
    *config.add_atom_matcher() = matcher3;

    // Mark as invalid
    AtomMatcher matcher4 = CreateReleaseWakelockAtomMatcher();
    int64_t matcher4Id = matcher4.id();
    matcher4.clear_simple_atom_matcher();
    *config.add_atom_matcher() = matcher4;

    AtomMatcher matcher5 = CreateMoveToForegroundAtomMatcher();
    int64_t matcher5Id = matcher5.id();
    int matcher5Index = 3;
    *config.add_atom_matcher() = matcher5;

    AtomMatcher matcher6 = CreateMoveToBackgroundAtomMatcher();
    int64_t matcher6Id = matcher6.id();
    int matcher6Index = 4;
    *config.add_atom_matcher() = matcher6;

    AtomMatcher matcher7 = CreateBatteryStateNoneMatcher();
    int64_t matcher7Id = matcher7.id();
    int matcher7Index = 5;
    *config.add_atom_matcher() = matcher7;

    AtomMatcher matcher8 = CreateBatteryStateUsbMatcher();
    int64_t matcher8Id = matcher8.id();
    int matcher8Index = 6;
    *config.add_atom_matcher() = matcher8;

    Predicate predicate1 = CreateScreenIsOnPredicate();
    int64_t predicate1Id = predicate1.id();
    int predicate1Index = 0;
    *config.add_predicate() = predicate1;

    Predicate predicate2 = CreateScreenIsOffPredicate();
    int64_t predicate2Id = predicate2.id();
    int predicate2Index = 1;
    *config.add_predicate() = predicate2;

    Predicate predicate3 = CreateDeviceUnpluggedPredicate();
    int64_t predicate3Id = predicate3.id();
    int predicate3Index = 2;
    *config.add_predicate() = predicate3;

    Predicate predicate4 = CreateIsInBackgroundPredicate();
    *predicate4.mutable_simple_predicate()->mutable_dimensions() =
            CreateDimensions(util::ACTIVITY_FOREGROUND_STATE_CHANGED, {1});
    int64_t predicate4Id = predicate4.id();
    int predicate4Index = 3;
    *config.add_predicate() = predicate4;

    // Invalid due to invalid matcher dependency
    Predicate predicate5 = CreateHoldingWakelockPredicate();
    *predicate5.mutable_simple_predicate()->mutable_dimensions() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    predicate5.mutable_simple_predicate()->set_stop_all(matcher7Id);
    int64_t predicate5Id = predicate5.id();
    *config.add_predicate() = predicate5;

    State state1 = CreateScreenStateWithOnOffMap(0x123, 0x321);
    int64_t state1Id = state1.id();
    *config.add_state() = state1;

    State state2 = CreateScreenState();
    int64_t state2Id = state2.id();
    *config.add_state() = state2;

    // Add a few duration metrics.
    // Will be Invalid due to invalid predicate dependency
    DurationMetric duration1 =
            createDurationMetric("DURATION1", predicate5Id, predicate4Id, {state2Id});
    *duration1.mutable_dimensions_in_what() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    MetricConditionLink* link = duration1.add_links();
    link->set_condition(predicate4Id);
    *link->mutable_fields_in_what() =
            CreateAttributionUidDimensions(util::WAKELOCK_STATE_CHANGED, {Position::FIRST});
    *link->mutable_fields_in_condition() =
            CreateDimensions(util::ACTIVITY_FOREGROUND_STATE_CHANGED, {1} /*uid field*/);
    int64_t duration1Id = duration1.id();
    *config.add_duration_metric() = duration1;

    DurationMetric duration2 = createDurationMetric("DURATION2", predicate1Id, nullopt, {});
    int64_t duration2Id = duration2.id();
    int duration2Index = 0;
    *config.add_duration_metric() = duration2;

    // Will be invalid due to unknown state
    DurationMetric duration3 =
            createDurationMetric("DURATION3", predicate3Id, nullopt, /*stateIds=*/{-1});
    int64_t duration3Id = duration3.id();
    *config.add_duration_metric() = duration3;

    DurationMetric duration4 = createDurationMetric("DURATION4", predicate3Id, predicate2Id, {});
    int64_t duration4Id = duration4.id();
    int duration4Index = 1;
    *config.add_duration_metric() = duration4;

    DurationMetric duration5 = createDurationMetric("DURATION5", predicate2Id, nullopt, {});
    int64_t duration5Id = duration5.id();
    int duration5Index = 2;
    *config.add_duration_metric() = duration5;

    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;

    // Output data structures to validate.
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    unordered_map<int64_t, int> expectedMetricProducerMap = {{duration2Id, duration2Index},
                                                             {duration4Id, duration4Index},
                                                             {duration5Id, duration5Index}};
    EXPECT_THAT(newMetricProducerMap, ContainerEq(expectedMetricProducerMap));
    // Make sure preserved metrics are the same.
    ASSERT_EQ(newMetricProducers.size(), 3);

    // Verify the conditionToMetricMap. Note that the "what" is not in this map.
    ASSERT_EQ(conditionToMetricMap.size(), 1);
    const vector<int>& condition2Metrics = conditionToMetricMap[predicate2Index];
    EXPECT_THAT(condition2Metrics, UnorderedElementsAre(duration4Index));

    // Verify the trackerToMetricMap. The start/stop/stopall indices from the "what" should be here.
    ASSERT_EQ(trackerToMetricMap.size(), 4);
    const vector<int>& matcher1Metrics = trackerToMetricMap[matcher1Index];
    EXPECT_THAT(matcher1Metrics, UnorderedElementsAre(duration2Index, duration5Index));
    const vector<int>& matcher2Metrics = trackerToMetricMap[matcher2Index];
    EXPECT_THAT(matcher2Metrics, UnorderedElementsAre(duration2Index, duration5Index));
    const vector<int>& matcher7Metrics = trackerToMetricMap[matcher7Index];
    EXPECT_THAT(matcher7Metrics, UnorderedElementsAre(duration4Index));
    const vector<int>& matcher8Metrics = trackerToMetricMap[matcher8Index];
    EXPECT_THAT(matcher8Metrics, UnorderedElementsAre(duration4Index));

    // Verify event activation/deactivation maps.
    ASSERT_EQ(activationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(deactivationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(metricsWithActivation.size(), 0);

    // Verify tracker indices/ids/conditions are correct.
    DurationMetricProducer* durationProducer2 =
            static_cast<DurationMetricProducer*>(newMetricProducers[duration2Index].get());
    EXPECT_EQ(durationProducer2->getMetricId(), duration2Id);
    EXPECT_EQ(durationProducer2->mConditionTrackerIndex, -1);
    EXPECT_EQ(durationProducer2->mCondition, ConditionState::kTrue);
    EXPECT_EQ(durationProducer2->mStartIndex, matcher1Index);
    EXPECT_EQ(durationProducer2->mStopIndex, matcher2Index);
    EXPECT_EQ(durationProducer2->mStopAllIndex, -1);
    DurationMetricProducer* durationProducer4 =
            static_cast<DurationMetricProducer*>(newMetricProducers[duration4Index].get());
    EXPECT_EQ(durationProducer4->getMetricId(), duration4Id);
    EXPECT_EQ(durationProducer4->mConditionTrackerIndex, predicate2Index);
    EXPECT_EQ(durationProducer4->mCondition, ConditionState::kUnknown);
    EXPECT_EQ(durationProducer4->mStartIndex, matcher7Index);
    EXPECT_EQ(durationProducer4->mStopIndex, matcher8Index);
    EXPECT_EQ(durationProducer4->mStopAllIndex, -1);
    DurationMetricProducer* durationProducer5 =
            static_cast<DurationMetricProducer*>(newMetricProducers[duration5Index].get());
    EXPECT_EQ(durationProducer5->getMetricId(), duration5Id);
    EXPECT_EQ(durationProducer5->mConditionTrackerIndex, -1);
    EXPECT_EQ(durationProducer5->mCondition, ConditionState::kTrue);
    EXPECT_EQ(durationProducer5->mStartIndex, matcher2Index);
    EXPECT_EQ(durationProducer5->mStopIndex, matcher1Index);
    EXPECT_EQ(durationProducer5->mStopAllIndex, -1);

    EXPECT_EQ(invalidEntities.size(), 4);

    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{duration3.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_STATE_NOT_FOUND);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), duration3.id());
    reason = invalidEntities[InvalidEntityKey{duration1.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_INVALID_PREDICATE_DEPENDENCY);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), duration1.id());
    EXPECT_THAT(reason.conditionIds, UnorderedElementsAre(predicate5Id));
    reason = invalidEntities[InvalidEntityKey{matcher4.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
}

TEST_F(MetricsManagerUtilTest, TestInitEventMetricsHasInvalidMetrics) {
    StatsdConfig config;

    // Add atom matchers/predicates. These are mostly needed for initStatsdConfig
    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    int matcher1Index = 0;
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    int matcher2Index = 1;
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateStartScheduledJobAtomMatcher();
    int64_t matcher3Id = matcher3.id();
    int matcher3Index = 2;
    *config.add_atom_matcher() = matcher3;

    // Make matcher 4 invalid
    AtomMatcher matcher4 = CreateFinishScheduledJobAtomMatcher();
    int64_t matcher4Id = matcher4.id();
    matcher4.clear_simple_atom_matcher();
    *config.add_atom_matcher() = matcher4;

    AtomMatcher matcher5 = CreateBatterySaverModeStartAtomMatcher();
    int64_t matcher5Id = matcher5.id();
    int matcher5Index = 3;
    *config.add_atom_matcher() = matcher5;

    AtomMatcher matcher6 = CreateMoveToBackgroundAtomMatcher();
    int64_t matcher6Id = matcher6.id();
    int matcher6Index = 4;
    *config.add_atom_matcher() = matcher6;

    Predicate predicate1 = CreateScreenIsOnPredicate();
    int64_t predicate1Id = predicate1.id();
    int predicate1Index = 0;
    *config.add_predicate() = predicate1;

    // Invalid because dependent matcher is valid.
    Predicate predicate2 = CreateScheduledJobPredicate();
    int64_t predicate2Id = predicate2.id();
    *config.add_predicate() = predicate2;

    // Add a few event metrics.
    // Invalid due to dependent condition is invalid.
    EventMetric event1 = createEventMetric("EVENT1", matcher1Id, predicate2Id);
    int64_t event1Id = event1.id();
    *config.add_event_metric() = event1;

    EventMetric event2 = createEventMetric("EVENT2", matcher2Id, predicate1Id);
    int64_t event2Id = event2.id();
    int event2Index = 0;
    *config.add_event_metric() = event2;

    EventMetric event3 = createEventMetric("EVENT3", matcher3Id, nullopt);
    int64_t event3Id = event3.id();
    int event3Index = 1;
    *config.add_event_metric() = event3;

    MetricActivation event3Activation;
    event3Activation.set_metric_id(event3Id);
    EventActivation* eventActivation = event3Activation.add_event_activation();
    eventActivation->set_atom_matcher_id(matcher5Id);
    eventActivation->set_ttl_seconds(5);
    *config.add_metric_activation() = event3Activation;

    // Will be invalid due to invalid matcher dependency
    EventMetric event4 = createEventMetric("EVENT4", matcher4Id, predicate1Id);
    int64_t event4Id = event4.id();
    *config.add_event_metric() = event4;

    // Will be invalid due to invalid metric activation
    EventMetric event6 = createEventMetric("EVENT6", matcher6Id, nullopt);
    int64_t event6Id = event6.id();
    *config.add_event_metric() = event6;

    MetricActivation event6Activation;
    event6Activation.set_metric_id(event6Id);
    eventActivation = event6Activation.add_event_activation();
    eventActivation->set_atom_matcher_id(-1);  // set invalid matcher
    eventActivation->set_ttl_seconds(5);
    *config.add_metric_activation() = event6Activation;

    // Will be invalid due to multiple metric activation
    EventMetric event7 = createEventMetric("EVENT7", matcher6Id, nullopt);
    int64_t event7Id = event7.id();
    *config.add_event_metric() = event7;

    MetricActivation event7Activation;
    event7Activation.set_metric_id(event7Id);
    eventActivation = event7Activation.add_event_activation();
    eventActivation->set_atom_matcher_id(matcher5Id);
    eventActivation->set_ttl_seconds(5);

    MetricActivation event7Activation2;
    event7Activation2.set_metric_id(event7Id);
    eventActivation = event7Activation2.add_event_activation();
    eventActivation->set_atom_matcher_id(matcher6Id);
    eventActivation->set_ttl_seconds(10);

    *config.add_metric_activation() = event7Activation;
    *config.add_metric_activation() = event7Activation2;

    EventMetric event5 = createEventMetric("EVENT5", matcher5Id, nullopt);
    int64_t event5Id = event5.id();
    int event5Index = 2;
    *config.add_event_metric() = event5;

    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;

    // Output data structures to validate.
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    unordered_map<int64_t, int> expectedMetricProducerMap = {
            {event2Id, event2Index},
            {event3Id, event3Index},
            {event5Id, event5Index},
    };
    EXPECT_THAT(newMetricProducerMap, ContainerEq(expectedMetricProducerMap));

    ASSERT_EQ(newMetricProducers.size(), 3);

    // Verify the conditionToMetricMap.
    ASSERT_EQ(conditionToMetricMap.size(), 1);
    const vector<int>& condition1Metrics = conditionToMetricMap[predicate1Index];
    EXPECT_THAT(condition1Metrics, UnorderedElementsAre(event2Index));

    // Verify the trackerToMetricMap.
    ASSERT_EQ(trackerToMetricMap.size(), 3);
    const vector<int>& matcher2Metrics = trackerToMetricMap[matcher2Index];
    EXPECT_THAT(matcher2Metrics, UnorderedElementsAre(event2Index));
    const vector<int>& matcher3Metrics = trackerToMetricMap[matcher3Index];
    EXPECT_THAT(matcher3Metrics, UnorderedElementsAre(event3Index));
    const vector<int>& matcher5Metrics = trackerToMetricMap[matcher5Index];
    EXPECT_THAT(matcher5Metrics, UnorderedElementsAre(event5Index));

    // Verify event activation/deactivation maps.
    ASSERT_EQ(activationAtomTrackerToMetricMap.size(), 1);
    EXPECT_THAT(activationAtomTrackerToMetricMap[matcher5Index], UnorderedElementsAre(event3Index));
    ASSERT_EQ(deactivationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(metricsWithActivation.size(), 1);
    EXPECT_THAT(metricsWithActivation, UnorderedElementsAre(event3Index));

    // Verify tracker indices/ids/conditions are correct.
    EXPECT_EQ(newMetricProducers[event2Index]->getMetricId(), event2Id);
    EXPECT_EQ(newMetricProducers[event2Index]->mConditionTrackerIndex, predicate1Index);
    EXPECT_EQ(newMetricProducers[event2Index]->mCondition, ConditionState::kUnknown);
    EXPECT_EQ(newMetricProducers[event3Index]->getMetricId(), event3Id);
    EXPECT_EQ(newMetricProducers[event3Index]->mConditionTrackerIndex, -1);
    EXPECT_EQ(newMetricProducers[event3Index]->mCondition, ConditionState::kTrue);
    EXPECT_EQ(newMetricProducers[event5Index]->getMetricId(), event5Id);
    EXPECT_EQ(newMetricProducers[event5Index]->mConditionTrackerIndex, -1);
    EXPECT_EQ(newMetricProducers[event5Index]->mCondition, ConditionState::kTrue);

    EXPECT_EQ(invalidEntities.size(), 6);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{event4.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_INVALID_MATCHER_DEPENDENCY);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), event4.id());
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
    reason = invalidEntities[InvalidEntityKey{matcher4.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
    reason = invalidEntities[InvalidEntityKey{event1.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_INVALID_PREDICATE_DEPENDENCY);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), event1.id());
    EXPECT_THAT(reason.conditionIds, UnorderedElementsAre(predicate2Id));
    reason = invalidEntities[InvalidEntityKey{event6.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_ACTIVATION_MATCHER_NOT_FOUND);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), event6.id());
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(-1));
    reason = invalidEntities[InvalidEntityKey{event7.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_HAS_MULTIPLE_ACTIVATIONS);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), event7.id());
    reason = invalidEntities[InvalidEntityKey{predicate2.id(), INVALID_ENTITY_TYPE_PREDICATE}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_CONDITION_INVALID_MATCHER_DEPENDENCY);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
}

TEST_F(MetricsManagerUtilTest, TestInitValueMetricsHasInvalidMetrics) {
    StatsdConfig config;

    // Add atom matchers/predicates/states. These are mostly needed for initStatsdConfig.
    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    int matcher1Index = 0;
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    int matcher2Index = 1;
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateStartScheduledJobAtomMatcher();
    int64_t matcher3Id = matcher3.id();
    int matcher3Index = 2;
    *config.add_atom_matcher() = matcher3;

    // Make matcher 4 invalid
    AtomMatcher matcher4 = CreateTemperatureAtomMatcher();
    int64_t matcher4Id = matcher4.id();
    matcher4.clear_simple_atom_matcher();
    *config.add_atom_matcher() = matcher4;

    AtomMatcher matcher5 = CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);
    int64_t matcher5Id = matcher5.id();
    int matcher5Index = 3;
    *config.add_atom_matcher() = matcher5;

    Predicate predicate1 = CreateScreenIsOnPredicate();
    int64_t predicate1Id = predicate1.id();
    *config.add_predicate() = predicate1;

    Predicate predicate2 = CreateScreenIsOffPredicate();
    int64_t predicate2Id = predicate2.id();
    *config.add_predicate() = predicate2;

    State state1 = CreateScreenStateWithOnOffMap(0x123, 0x321);
    int64_t state1Id = state1.id();
    *config.add_state() = state1;

    State state2 = CreateScreenState();
    int64_t state2Id = state2.id();
    *config.add_state() = state2;

    // Add a few value metrics.
    // Note that these will not work as "real" metrics since the value field is always 2.
    // Invalid due to dependent matcher being invalid
    ValueMetric value1 = createValueMetric("VALUE1", matcher4, 2, predicate1Id, {state1Id});
    int64_t value1Id = value1.id();
    *config.add_value_metric() = value1;

    ValueMetric value2 = createValueMetric("VALUE2", matcher1, 2, nullopt, {});
    int64_t value2Id = value2.id();
    int value2Index = 0;
    *config.add_value_metric() = value2;

    // Invalid due to missing bin configs
    ValueMetric value3 = createValueMetric("VALUE3", matcher5, 2, predicate2Id, {});
    int64_t value3Id = value3.id();
    value3.set_aggregation_type(ValueMetric_AggregationType_HISTOGRAM);
    *config.add_value_metric() = value3;

    ValueMetric value4 = createValueMetric("VALUE4", matcher3, 2, nullopt, {state2Id});
    int64_t value4Id = value4.id();
    int value4Index = 1;
    *config.add_value_metric() = value4;

    ValueMetric value5 = createValueMetric("VALUE5", matcher2, 2, nullopt, {});
    int64_t value5Id = value5.id();
    int value5Index = 2;
    *config.add_value_metric() = value5;

    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;

    // Output data structures to validate.
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    unordered_map<int64_t, int> expectedMetricProducerMap = {
            {value2Id, value2Index},
            {value4Id, value4Index},
            {value5Id, value5Index},
    };
    EXPECT_THAT(newMetricProducerMap, ContainerEq(expectedMetricProducerMap));

    ASSERT_EQ(newMetricProducers.size(), 3);

    // Verify the conditionToMetricMap.
    ASSERT_EQ(conditionToMetricMap.size(), 0);

    // Verify the trackerToMetricMap.
    ASSERT_EQ(trackerToMetricMap.size(), 3);
    const vector<int>& matcher1Metrics = trackerToMetricMap[matcher1Index];
    EXPECT_THAT(matcher1Metrics, UnorderedElementsAre(value2Index));
    const vector<int>& matcher2Metrics = trackerToMetricMap[matcher2Index];
    EXPECT_THAT(matcher2Metrics, UnorderedElementsAre(value5Index));
    const vector<int>& matcher3Metrics = trackerToMetricMap[matcher3Index];
    EXPECT_THAT(matcher3Metrics, UnorderedElementsAre(value4Index));

    // Verify event activation/deactivation maps.
    ASSERT_EQ(activationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(deactivationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(metricsWithActivation.size(), 0);

    // Verify tracker indices/ids/conditions/states are correct.
    NumericValueMetricProducer* valueProducer2 =
            static_cast<NumericValueMetricProducer*>(newMetricProducers[value2Index].get());
    EXPECT_EQ(valueProducer2->getMetricId(), value2Id);
    EXPECT_EQ(valueProducer2->mConditionTrackerIndex, -1);
    EXPECT_EQ(valueProducer2->mCondition, ConditionState::kTrue);
    EXPECT_EQ(valueProducer2->mWhatMatcherIndex, matcher1Index);
    NumericValueMetricProducer* valueProducer4 =
            static_cast<NumericValueMetricProducer*>(newMetricProducers[value4Index].get());
    EXPECT_EQ(valueProducer4->getMetricId(), value4Id);
    EXPECT_EQ(valueProducer4->mConditionTrackerIndex, -1);
    EXPECT_EQ(valueProducer4->mCondition, ConditionState::kTrue);
    EXPECT_EQ(valueProducer4->mWhatMatcherIndex, matcher3Index);
    NumericValueMetricProducer* valueProducer5 =
            static_cast<NumericValueMetricProducer*>(newMetricProducers[value5Index].get());
    EXPECT_EQ(valueProducer5->getMetricId(), value5Id);
    EXPECT_EQ(valueProducer5->mConditionTrackerIndex, -1);
    EXPECT_EQ(valueProducer5->mCondition, ConditionState::kTrue);
    EXPECT_EQ(valueProducer5->mWhatMatcherIndex, matcher2Index);

    EXPECT_EQ(invalidEntities.size(), 3);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{value1.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_INVALID_MATCHER_DEPENDENCY);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), value1.id());
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
    reason = invalidEntities[InvalidEntityKey{matcher4.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
    reason = invalidEntities[InvalidEntityKey{value3.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason,
              INVALID_CONFIG_REASON_VALUE_METRIC_HIST_COUNT_DNE_HIST_BIN_CONFIGS_COUNT);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), value3.id());
}

TEST_F(MetricsManagerUtilTest, TestInitKllMetricsHasInvalidMetrics) {
    StatsdConfig config;

    // Add atom matchers/predicates. These are mostly needed for initStatsdConfig.
    AtomMatcher matcher1 = CreateScreenTurnedOnAtomMatcher();
    int64_t matcher1Id = matcher1.id();
    int matcher1Index = 0;
    *config.add_atom_matcher() = matcher1;

    AtomMatcher matcher2 = CreateScreenTurnedOffAtomMatcher();
    int64_t matcher2Id = matcher2.id();
    int matcher2Index = 1;
    *config.add_atom_matcher() = matcher2;

    AtomMatcher matcher3 = CreateStartScheduledJobAtomMatcher();
    int64_t matcher3Id = matcher3.id();
    int matcher3Index = 2;
    *config.add_atom_matcher() = matcher3;

    AtomMatcher matcher4 = CreateAppStartOccurredAtomMatcher();
    int64_t matcher4Id = matcher4.id();
    matcher4.clear_simple_atom_matcher();
    *config.add_atom_matcher() = matcher4;

    AtomMatcher matcher5 = CreateSimpleAtomMatcher("SubsystemSleep", util::SUBSYSTEM_SLEEP_STATE);
    int64_t matcher5Id = matcher5.id();
    int matcher5Index = 3;
    *config.add_atom_matcher() = matcher5;

    Predicate predicate1 = CreateScreenIsOnPredicate();
    int64_t predicate1Id = predicate1.id();
    int predicate1Index = 0;
    *config.add_predicate() = predicate1;

    Predicate predicate2 = CreateScreenIsOffPredicate();
    int64_t predicate2Id = predicate2.id();
    int predicate2Index = 1;
    *config.add_predicate() = predicate2;

    // Add a few kll metrics.
    // Note that these will not work as "real" metrics since the value field is always 2.
    // Will be invalid due to invalid matcher dependency
    KllMetric kll1 = createKllMetric("KLL1", matcher4, /*valueField=*/2, predicate1Id);
    int64_t kll1Id = kll1.id();
    *config.add_kll_metric() = kll1;

    // Will be invalid due to missing kll field
    KllMetric kll2 = createKllMetric("KLL2", matcher1, /*valueField=*/2, nullopt);
    int64_t kll2Id = kll2.id();
    kll2.clear_kll_field();
    *config.add_kll_metric() = kll2;

    KllMetric kll3 = createKllMetric("KLL3", matcher5, /*valueField=*/2, predicate2Id);
    int64_t kll3Id = kll3.id();
    int kll3Index = 0;
    *config.add_kll_metric() = kll3;

    KllMetric kll4 = createKllMetric("KLL", matcher3, /*valueField=*/2, nullopt);
    int64_t kll4Id = kll4.id();
    int kll4Index = 1;
    *config.add_kll_metric() = kll4;

    // Will be deleted.
    KllMetric kll5 = createKllMetric("KLL5", matcher5, /*valueField=*/2, predicate1Id);
    int64_t kll5Id = kll5.id();
    int kll5Index = 2;
    *config.add_kll_metric() = kll5;

    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;

    // Output data structures to validate.
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    unordered_map<int64_t, int> expectedMetricProducerMap = {
            {kll3Id, kll3Index},
            {kll4Id, kll4Index},
            {kll5Id, kll5Index},
    };
    EXPECT_THAT(newMetricProducerMap, ContainerEq(expectedMetricProducerMap));

    ASSERT_EQ(newMetricProducers.size(), 3);

    // Verify the conditionToMetricMap.
    ASSERT_EQ(conditionToMetricMap.size(), 2);
    const vector<int>& condition1Metrics = conditionToMetricMap[predicate1Index];
    EXPECT_THAT(condition1Metrics, UnorderedElementsAre(kll5Index));
    const vector<int>& condition2Metrics = conditionToMetricMap[predicate2Index];
    EXPECT_THAT(condition2Metrics, UnorderedElementsAre(kll3Index));

    // Verify the trackerToMetricMap.
    ASSERT_EQ(trackerToMetricMap.size(), 2);
    const vector<int>& matcher3Metrics = trackerToMetricMap[matcher3Index];
    EXPECT_THAT(matcher3Metrics, UnorderedElementsAre(kll4Index));
    const vector<int>& matcher5Metrics = trackerToMetricMap[matcher5Index];
    EXPECT_THAT(matcher5Metrics, UnorderedElementsAre(kll3Index, kll5Index));

    // Verify event activation/deactivation maps.
    ASSERT_EQ(activationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(deactivationAtomTrackerToMetricMap.size(), 0);
    ASSERT_EQ(metricsWithActivation.size(), 0);

    // Verify tracker indices/ids/conditions are correct.
    KllMetricProducer* kllProducer3 =
            static_cast<KllMetricProducer*>(newMetricProducers[kll3Index].get());
    EXPECT_EQ(kllProducer3->getMetricId(), kll3Id);
    EXPECT_EQ(kllProducer3->mConditionTrackerIndex, predicate2Index);
    EXPECT_EQ(kllProducer3->mCondition, ConditionState::kUnknown);
    EXPECT_EQ(kllProducer3->mWhatMatcherIndex, matcher5Index);
    KllMetricProducer* kllProducer4 =
            static_cast<KllMetricProducer*>(newMetricProducers[kll4Index].get());
    EXPECT_EQ(kllProducer4->getMetricId(), kll4Id);
    EXPECT_EQ(kllProducer4->mConditionTrackerIndex, -1);
    EXPECT_EQ(kllProducer4->mCondition, ConditionState::kTrue);
    EXPECT_EQ(kllProducer4->mWhatMatcherIndex, matcher3Index);
    KllMetricProducer* kllProducer5 =
            static_cast<KllMetricProducer*>(newMetricProducers[kll5Index].get());
    EXPECT_EQ(kllProducer5->getMetricId(), kll5Id);
    EXPECT_EQ(kllProducer5->mConditionTrackerIndex, predicate1Index);
    EXPECT_EQ(kllProducer5->mCondition, ConditionState::kUnknown);
    EXPECT_EQ(kllProducer5->mWhatMatcherIndex, matcher5Index);

    EXPECT_EQ(invalidEntities.size(), 3);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{kll1.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_METRIC_INVALID_MATCHER_DEPENDENCY);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), kll1.id());
    reason = invalidEntities[InvalidEntityKey{matcher4.id(), INVALID_ENTITY_TYPE_MATCHER}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_MATCHER_MALFORMED_CONTENTS_CASE);
    EXPECT_THAT(reason.matcherIds, UnorderedElementsAre(matcher4Id));
    reason = invalidEntities[InvalidEntityKey{kll2.id(), INVALID_ENTITY_TYPE_METRIC}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_KLL_METRIC_MISSING_KLL_FIELD);
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), kll2.id());
}

TEST_F(MetricsManagerUtilTest, TestInitAlertsHasInvalidAlerts) {
    StatsdConfig config;
    // Add atom matchers/predicates/metrics. These are mostly needed for initStatsdConfig
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOffAtomMatcher();
    *config.add_predicate() = CreateScreenIsOnPredicate();

    CountMetric countMetric = createCountMetric("COUNT1", config.atom_matcher(0).id(), nullopt, {});
    int64_t countMetricId = countMetric.id();
    *config.add_count_metric() = countMetric;

    DurationMetric durationMetric =
            createDurationMetric("DURATION1", config.predicate(0).id(), nullopt, {});
    int64_t durationMetricId = durationMetric.id();
    *config.add_duration_metric() = durationMetric;

    // Add alerts.
    Alert alert1 = createAlert("Alert1", durationMetricId, /*buckets*/ 1, /*triggerSum*/ 5000);
    int64_t alert1Id = alert1.id();
    *config.add_alert() = alert1;

    // Invalid due to missing metric Id
    Alert alert2 = createAlert("Alert2", /*metricId=*/0, /*buckets*/ 1, /*triggerSum*/ 2);
    int64_t alert2Id = alert2.id();
    *config.add_alert() = alert2;

    Alert alert3 = createAlert("Alert3", durationMetricId, /*buckets*/ 3, /*triggerSum*/ 5000);
    int64_t alert3Id = alert3.id();
    *config.add_alert() = alert3;

    // Add Subscriptions.
    Subscription subscription1 = createSubscription("S1", Subscription::ALERT, alert1Id);
    *config.add_subscription() = subscription1;
    Subscription subscription2 = createSubscription("S2", Subscription::ALERT, alert1Id);
    *config.add_subscription() = subscription2;
    Subscription subscription3 = createSubscription("S3", Subscription::ALERT, alert2Id);
    *config.add_subscription() = subscription3;

    // Output data structures to validate.
    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    int alert1Index = 0;
    int alert3Index = 1;
    unordered_map<int64_t, int> expectedAlertMap = {
            {alert1Id, alert1Index},
            {alert3Id, alert3Index},
    };
    EXPECT_THAT(alertTrackerMap, ContainerEq(expectedAlertMap));
    ASSERT_EQ(allAnomalyTrackers.size(), 2);

    // Verify metrics have the correct alerts added.
    ASSERT_EQ(newMetricProducers.size(), 2);
    DurationMetricProducer* durationProducer =
            static_cast<DurationMetricProducer*>(newMetricProducers[1].get());
    EXPECT_THAT(
            durationProducer->mAnomalyTrackers,
            UnorderedElementsAre(allAnomalyTrackers[alert1Index], allAnomalyTrackers[alert3Index]));
    for (const auto& durationTrackerIt : durationProducer->mCurrentSlicedDurationTrackerMap) {
        EXPECT_EQ(durationTrackerIt.second->mAnomalyTrackers, durationProducer->mAnomalyTrackers);
    }

    // Verify alerts have the correct subscriptions. Use subscription id as proxy for equivalency.
    vector<int64_t> alert1Subscriptions;
    for (const Subscription& subscription : allAnomalyTrackers[alert1Index]->mSubscriptions) {
        alert1Subscriptions.push_back(subscription.id());
    }
    EXPECT_THAT(alert1Subscriptions, UnorderedElementsAre(subscription1.id(), subscription2.id()));
    EXPECT_THAT(allAnomalyTrackers[alert3Index]->mSubscriptions, IsEmpty());

    EXPECT_EQ(invalidEntities.size(), 2);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{alert2.id(), INVALID_ENTITY_TYPE_ALERT}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_ALERT_METRIC_NOT_FOUND);
    ASSERT_TRUE(reason.alertId.has_value());
    EXPECT_EQ(reason.alertId.value(), alert2.id());
    ASSERT_TRUE(reason.metricId.has_value());
    EXPECT_EQ(reason.metricId.value(), 0);
    reason =
            invalidEntities[InvalidEntityKey{subscription3.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_SUBSCRIPTION_INVALID_ALERT_DEPENDENCY);
    ASSERT_TRUE(reason.alertId.has_value());
    EXPECT_EQ(reason.alertId.value(), alert2.id());
    ASSERT_TRUE(reason.subscriptionId.has_value());
    EXPECT_EQ(reason.subscriptionId.value(), subscription3.id());
}

TEST_F(MetricsManagerUtilTest, TestInitAlarmsHasInvalidAlarms) {
    StatsdConfig config;
    // Add alarms.
    Alarm alarm1 = createAlarm("Alarm1", /*offset*/ 1 * MS_PER_SEC, /*period*/ 50 * MS_PER_SEC);
    int64_t alarm1Id = alarm1.id();
    *config.add_alarm() = alarm1;

    // Invalid due to negative period
    Alarm alarm2 = createAlarm("Alarm2", /*offset*/ 1 * MS_PER_SEC, /*period*/ -2000 * MS_PER_SEC);
    int64_t alarm2Id = alarm2.id();
    *config.add_alarm() = alarm2;

    Alarm alarm3 = createAlarm("Alarm3", /*offset*/ 10 * MS_PER_SEC, /*period*/ 5000 * MS_PER_SEC);
    int64_t alarm3Id = alarm3.id();
    *config.add_alarm() = alarm3;

    // Add Subscriptions.
    Subscription subscription1 = createSubscription("S1", Subscription::ALARM, alarm1Id);
    *config.add_subscription() = subscription1;
    Subscription subscription2 = createSubscription("S2", Subscription::ALARM, alarm1Id);
    *config.add_subscription() = subscription2;
    Subscription subscription3 = createSubscription("S3", Subscription::ALARM, alarm2Id);
    *config.add_subscription() = subscription3;

    // Output data structures to validate.
    unordered_map<int64_t, int> alertTrackerMap;
    vector<sp<AnomalyTracker>> allAnomalyTrackers;
    unordered_map<int64_t, int> alarmTrackerMap;
    vector<sp<AlarmTracker>> allPeriodicAlarmTrackers;
    unordered_map<int64_t, int> newMetricProducerMap;
    vector<sp<MetricProducer>> newMetricProducers;
    unordered_map<int, vector<int>> conditionToMetricMap;
    unordered_map<int, vector<int>> trackerToMetricMap;
    unordered_map<int, vector<int>> activationAtomTrackerToMetricMap;
    unordered_map<int, vector<int>> deactivationAtomTrackerToMetricMap;
    vector<int> metricsWithActivation;
    unordered_map<InvalidEntityKey, InvalidConfigReason> invalidEntities;

    initConfigAndDependencies(config, newMetricProducerMap, newMetricProducers,
                              conditionToMetricMap, trackerToMetricMap,
                              activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
                              metricsWithActivation, alertTrackerMap, allAnomalyTrackers,
                              alarmTrackerMap, allPeriodicAlarmTrackers, invalidEntities);

    int alarm1Index = 0;
    int alarm3Index = 1;
    unordered_map<int64_t, int> expectedAlarmMap = {
            {alarm1Id, alarm1Index},
            {alarm3Id, alarm3Index},
    };
    EXPECT_THAT(alarmTrackerMap, ContainerEq(expectedAlarmMap));
    ASSERT_EQ(allPeriodicAlarmTrackers.size(), 2);

    // Verify alarms have the correct subscriptions. Use subscription id as proxy for equivalency.
    vector<int64_t> alarm1Subscriptions;
    for (const Subscription& subscription : allPeriodicAlarmTrackers[alarm1Index]->mSubscriptions) {
        alarm1Subscriptions.push_back(subscription.id());
    }

    EXPECT_THAT(alarm1Subscriptions, UnorderedElementsAre(subscription1.id(), subscription2.id()));
    EXPECT_THAT(allPeriodicAlarmTrackers[alarm3Index]->mSubscriptions, IsEmpty());

    EXPECT_EQ(invalidEntities.size(), 2);
    InvalidConfigReason reason =
            invalidEntities[InvalidEntityKey{alarm2.id(), INVALID_ENTITY_TYPE_ALARM}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_ALARM_PERIOD_LESS_THAN_OR_EQUAL_ZERO);
    ASSERT_TRUE(reason.alarmId.has_value());
    EXPECT_EQ(reason.alarmId.value(), alarm2.id());
    reason =
            invalidEntities[InvalidEntityKey{subscription3.id(), INVALID_ENTITY_TYPE_SUBSCRIPTION}];
    EXPECT_EQ(reason.reason, INVALID_CONFIG_REASON_SUBSCRIPTION_INVALID_ALARM_DEPENDENCY);
    ASSERT_TRUE(reason.alarmId.has_value());
    EXPECT_EQ(reason.alarmId.value(), alarm2.id());
    ASSERT_TRUE(reason.subscriptionId.has_value());
    EXPECT_EQ(reason.subscriptionId.value(), subscription3.id());
}

}  // namespace statsd
}  // namespace os
}  // namespace android

#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
