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

#pragma once

#include <gtest/gtest.h>
#include <utils/RefBase.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "src/anomaly/AlarmMonitor.h"
#include "src/anomaly/AlarmTracker.h"
#include "src/anomaly/AnomalyTracker.h"
#include "src/condition/ConditionTracker.h"
#include "src/config/ConfigMetadataProvider.h"
#include "src/external/StatsPullerManager.h"
#include "src/guardrail/StatsdStats.h"
#include "src/matchers/AtomMatchingTracker.h"
#include "src/metrics/MetricProducer.h"
#include "src/packages/UidMap.h"
#include "src/stats_util.h"
#include "src/statsd_config.pb.h"

namespace android {
namespace os {
namespace statsd {

constexpr int kConfigId = 12345;
const ConfigKey kConfigKey(0, kConfigId);
constexpr long timeBaseSec = 1000;
constexpr long kAlertId = 3;

class InitConfigTest : public ::testing::Test {
protected:
    InitConfigTest();

    void clearData();

    std::optional<InvalidConfigReason> initConfig(const StatsdConfig& config);

    void SetUp() override;

    sp<UidMap> uidMap;
    sp<StatsPullerManager> pullerManager;
    sp<AlarmMonitor> anomalyAlarmMonitor;
    sp<AlarmMonitor> periodicAlarmMonitor;
    sp<ConfigMetadataProvider> configMetadataProvider;
    std::unordered_map<int, vector<int>> allTagIdsToMatchersMap;
    std::vector<sp<AtomMatchingTracker>> allAtomMatchingTrackers;
    std::unordered_map<int64_t, int> atomMatchingTrackerMap;
    std::vector<sp<ConditionTracker>> allConditionTrackers;
    std::unordered_map<int64_t, int> conditionTrackerMap;
    std::vector<sp<MetricProducer>> allMetricProducers;
    std::unordered_map<int64_t, int> metricProducerMap;
    std::vector<sp<AnomalyTracker>> allAnomalyTrackers;
    std::unordered_map<int64_t, int> alertTrackerMap;
    std::vector<sp<AlarmTracker>> allAlarmTrackers;
    std::unordered_map<int, std::vector<int>> conditionToMetricMap;
    std::unordered_map<int, std::vector<int>> trackerToMetricMap;
    std::unordered_map<int, std::vector<int>> trackerToConditionMap;
    std::unordered_map<int, std::vector<int>> activationAtomTrackerToMetricMap;
    std::unordered_map<int, std::vector<int>> deactivationAtomTrackerToMetricMap;
    std::vector<int> metricsWithActivation;
    std::map<int64_t, uint64_t> stateProtoHashes;
    std::set<int64_t> noReportMetricIds;
};

StatsdConfig createHistogramStatsdConfig();

StatsdConfig createExplicitHistogramStatsdConfig(BinStarts bins);

StatsdConfig createGeneratedHistogramStatsdConfig(
        float binsMin, float binsMax, int binsCount,
        HistogramBinConfig::GeneratedBins::Strategy binStrategy);

}  // namespace statsd
}  // namespace os
}  // namespace android
