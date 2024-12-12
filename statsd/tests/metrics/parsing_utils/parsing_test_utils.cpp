
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

#include "parsing_test_utils.h"

#include <optional>
#include <vector>

#include "src/external/StatsPullerManager.h"
#include "src/guardrail/StatsdStats.h"
#include "src/metrics/parsing_utils/metrics_manager_util.h"
#include "src/packages/UidMap.h"
#include "src/state/StateManager.h"
#include "src/stats_util.h"
#include "src/statsd_config.pb.h"
#include "tests/statsd_test_util.h"

namespace android {
namespace os {
namespace statsd {

InitConfigTest::InitConfigTest() : uidMap(new UidMap()), pullerManager(new StatsPullerManager()) {
}

void InitConfigTest::clearData() {
    allTagIdsToMatchersMap.clear();
    allAtomMatchingTrackers.clear();
    atomMatchingTrackerMap.clear();
    allConditionTrackers.clear();
    conditionTrackerMap.clear();
    allMetricProducers.clear();
    metricProducerMap.clear();
    allAnomalyTrackers.clear();
    allAlarmTrackers.clear();
    conditionToMetricMap.clear();
    trackerToMetricMap.clear();
    trackerToConditionMap.clear();
    activationAtomTrackerToMetricMap.clear();
    deactivationAtomTrackerToMetricMap.clear();
    alertTrackerMap.clear();
    metricsWithActivation.clear();
    stateProtoHashes.clear();
    noReportMetricIds.clear();
}

std::optional<InvalidConfigReason> InitConfigTest::initConfig(const StatsdConfig& config) {
    // initStatsdConfig returns nullopt if config is valid
    return initStatsdConfig(
            kConfigKey, config, uidMap, pullerManager, anomalyAlarmMonitor, periodicAlarmMonitor,
            timeBaseSec, timeBaseSec, configMetadataProvider, allTagIdsToMatchersMap,
            allAtomMatchingTrackers, atomMatchingTrackerMap, allConditionTrackers,
            conditionTrackerMap, allMetricProducers, metricProducerMap, allAnomalyTrackers,
            allAlarmTrackers, conditionToMetricMap, trackerToMetricMap, trackerToConditionMap,
            activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap, alertTrackerMap,
            metricsWithActivation, stateProtoHashes, noReportMetricIds);
}

void InitConfigTest::SetUp() {
    clearData();
    StateManager::getInstance().clear();
}

StatsdConfig createHistogramStatsdConfig() {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateSimpleAtomMatcher("matcher", /* atomId */ 1);
    *config.add_value_metric() =
            createValueMetric("ValueMetric", config.atom_matcher(0), /* valueField */ 1,
                              /* condition */ nullopt, /* states */ {});
    config.mutable_value_metric(0)->set_aggregation_type(ValueMetric::HISTOGRAM);

    return config;
}

StatsdConfig createExplicitHistogramStatsdConfig(BinStarts bins) {
    StatsdConfig config = createHistogramStatsdConfig();
    *config.mutable_value_metric(0)->add_histogram_bin_configs() =
            createExplicitBinConfig(/* id */ 1, bins);

    return config;
}

StatsdConfig createGeneratedHistogramStatsdConfig(
        float binsMin, float binsMax, int binsCount,
        HistogramBinConfig::GeneratedBins::Strategy binStrategy) {
    StatsdConfig config = createHistogramStatsdConfig();
    *config.mutable_value_metric(0)->add_histogram_bin_configs() =
            createGeneratedBinConfig(/* id */ 1, binsMin, binsMax, binsCount, binStrategy);

    return config;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
