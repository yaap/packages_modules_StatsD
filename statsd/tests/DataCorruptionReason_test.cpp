// Copyright (C) 2024 The Android Open Source Project
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <vector>

#include "src/metrics/parsing_utils/metrics_manager_util.h"
#include "src/stats_log.pb.h"
#include "src/statsd_config.pb.h"
#include "statsd_test_util.h"

using namespace testing;
using android::sp;
using std::map;
using std::set;
using std::vector;

#ifdef __ANDROID__

namespace android {
namespace os {
namespace statsd {

namespace {
constexpr int kConfigId = 12345;
const ConfigKey kConfigKey(0, kConfigId);

constexpr long kTimeBaseSec = 1000;
constexpr int64_t kBucketStartTimeNs = 10000000000;
constexpr int64_t kAtomsLogTimeNs = kBucketStartTimeNs + 10;
constexpr int64_t kReportRequestTimeNs = kBucketStartTimeNs + 100;

constexpr int32_t kAppUid = AID_APP_START + 1;
constexpr int32_t kAtomId = 2;
constexpr int32_t kInterestAtomId = 3;
constexpr int32_t kNotInterestedMetricId = 3;
constexpr int32_t kInterestedMetricId = 4;
constexpr int32_t kUnusedAtomId = kInterestAtomId + 100;

const string kAppName = "TestApp";
const set<int32_t> kAppUids = {kAppUid, kAppUid + 10000};
const map<string, set<int32_t>> kPkgToUids = {{kAppName, kAppUids}};

StatsdConfig buildGoodEventConfig() {
    StatsdConfig config;
    config.set_id(kConfigId);

    {
        AtomMatcher* eventMatcher = config.add_atom_matcher();
        eventMatcher->set_id(StringToId("SCREEN_IS_ON"));
        SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
        simpleAtomMatcher->set_atom_id(2 /*SCREEN_STATE_CHANGE*/);

        EventMetric* metric = config.add_event_metric();
        metric->set_id(kNotInterestedMetricId);
        metric->set_what(StringToId("SCREEN_IS_ON"));
    }

    {
        const int64_t matcherId = StringToId("CUSTOM_EVENT" + std::to_string(kInterestAtomId));
        AtomMatcher* eventMatcher = config.add_atom_matcher();
        eventMatcher->set_id(matcherId);
        SimpleAtomMatcher* simpleAtomMatcher = eventMatcher->mutable_simple_atom_matcher();
        simpleAtomMatcher->set_atom_id(kInterestAtomId);

        EventMetric* metric = config.add_event_metric();
        metric->set_id(kInterestedMetricId);
        metric->set_what(matcherId);
    }

    return config;
}

ConfigMetricsReport getMetricsReport(MetricsManager& metricsManager) {
    ProtoOutputStream output;
    metricsManager.onDumpReport(kReportRequestTimeNs, kReportRequestTimeNs,
                                /*include_current_partial_bucket*/ true, /*erase_data*/ true,
                                /*dumpLatency*/ NO_TIME_CONSTRAINTS, nullptr, &output);

    ConfigMetricsReport metricsReport;
    outputStreamToProto(&output, &metricsReport);
    return metricsReport;
}

}  // anonymous namespace

/**
 * @brief Variety of test parameters combination represents always allowed atom (true/false)
 * logged by allowed package uid (true, false) to be tested with SocketLossInfo atom propagation
 */

enum class AllowedFromAnyUidFlag { NOT_ALLOWED = 0, ALLOWED };

enum class AllowedFromSpecificUidFlag { NOT_ALLOWED = 0, ALLOWED };

template <typename T>
struct TypedParam {
    T value;
    string label;

    operator T() const {
        return value;
    }
};

using AllowedFromAnyUidFlagParam = TypedParam<AllowedFromAnyUidFlag>;
using AllowedFromSpecificUidFlagParam = TypedParam<AllowedFromSpecificUidFlag>;

class SocketLossInfoTest
    : public testing::TestWithParam<
              std::tuple<AllowedFromAnyUidFlagParam, AllowedFromSpecificUidFlagParam>> {
protected:
    std::shared_ptr<MetricsManager> mMetricsManager;

    void SetUp() override {
        StatsdConfig config;

        // creates config with 2 metrics where one dedicated event metric interested in
        // kInterestAtomId
        config = buildGoodEventConfig();

        // Test parametrized on allowed_atom (true/false)
        if (isAtomAllowedFromAnyUid()) {
            config.add_whitelisted_atom_ids(kAtomId);
            config.add_whitelisted_atom_ids(kInterestAtomId);
        }

        // Test parametrized on allowed_package (true/false)
        if (isAtomFromAllowedUid()) {
            config.add_allowed_log_source(kAppName);
        }

        sp<MockUidMap> uidMap = new StrictMock<MockUidMap>();
        EXPECT_CALL(*uidMap, getAppUid(_)).WillRepeatedly(Invoke([](const string& pkg) {
            const auto& it = kPkgToUids.find(pkg);
            if (it != kPkgToUids.end()) {
                return it->second;
            }
            return set<int32_t>();
        }));
        sp<StatsPullerManager> pullerManager = new StatsPullerManager();
        sp<AlarmMonitor> anomalyAlarmMonitor;
        sp<AlarmMonitor> periodicAlarmMonitor;

        mMetricsManager = std::make_shared<MetricsManager>(
                kConfigKey, config, kTimeBaseSec, kTimeBaseSec, uidMap, pullerManager,
                anomalyAlarmMonitor, periodicAlarmMonitor);

        EXPECT_TRUE(mMetricsManager->isConfigValid());
    }

    bool isAtomAllowedFromAnyUid() const {
        return std::get<0>(GetParam()) == AllowedFromAnyUidFlag::ALLOWED;
    }

    bool isAtomFromAllowedUid() const {
        return std::get<1>(GetParam()) == AllowedFromSpecificUidFlag::ALLOWED;
    }

    bool isAtomLoggingAllowed() const {
        return isAtomAllowedFromAnyUid() || isAtomFromAllowedUid();
    }
};

INSTANTIATE_TEST_SUITE_P(
        SocketLossInfoTest, SocketLossInfoTest,
        testing::Combine(testing::ValuesIn<AllowedFromAnyUidFlagParam>({
                                 {AllowedFromAnyUidFlag::NOT_ALLOWED, "NotAllowedFromAnyUid"},
                                 {AllowedFromAnyUidFlag::ALLOWED, "AllowedFromAnyUid"},
                         }),
                         testing::ValuesIn<AllowedFromSpecificUidFlagParam>({
                                 {AllowedFromSpecificUidFlag::NOT_ALLOWED,
                                  "NotAllowedFromSpecificUid"},
                                 {AllowedFromSpecificUidFlag::ALLOWED, "AllowedFromSpecificUid"},
                         })),
        [](const testing::TestParamInfo<SocketLossInfoTest::ParamType>& info) {
            return std::get<0>(info.param).label + std::get<1>(info.param).label;
        });

TEST_P(SocketLossInfoTest, PropagationTest) {
    LogEvent eventOfInterest(kAppUid /* uid */, 0 /* pid */);
    CreateNoValuesLogEvent(&eventOfInterest, kInterestAtomId /* atom id */, 0 /* timestamp */);
    EXPECT_EQ(mMetricsManager->checkLogCredentials(eventOfInterest), isAtomLoggingAllowed());
    EXPECT_EQ(mMetricsManager->mAllMetricProducers[0]->mDataCorruptedDueToSocketLoss,
              MetricProducer::DataCorruptionSeverity::kNone);

    const auto eventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);

    // the STATS_SOCKET_LOSS_REPORTED on its own will not be propagated/consumed by any metric
    EXPECT_EQ(mMetricsManager->checkLogCredentials(*eventSocketLossReported.get()),
              isAtomFromAllowedUid());

    // the loss info for an atom kInterestAtomId will be evaluated even when
    // STATS_SOCKET_LOSS_REPORTED atom is not explicitly in allowed list
    mMetricsManager->onLogEvent(*eventSocketLossReported.get());

    // check that corresponding event metric was properly updated (or not) with loss info
    for (const auto& metricProducer : mMetricsManager->mAllMetricProducers) {
        if (metricProducer->getMetricId() == kInterestedMetricId) {
            EXPECT_EQ(metricProducer->mDataCorruptedDueToSocketLoss !=
                              MetricProducer::DataCorruptionSeverity::kNone,
                      isAtomLoggingAllowed());
            continue;
        }
        EXPECT_EQ(metricProducer->mDataCorruptedDueToSocketLoss,
                  MetricProducer::DataCorruptionSeverity::kNone);
    }
}

TEST_P(SocketLossInfoTest, TestNotifyOnlyInterestedMetrics) {
    const auto eventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kUnusedAtomId);

    mMetricsManager->onLogEvent(*eventSocketLossReported.get());
    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager);
    EXPECT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(metricsReport.metrics(),
                Each(Property(&StatsLogReport::data_corrupted_reason_size, 0)));

    const auto usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_SOCKET_LOSS);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }
}

TEST_P(SocketLossInfoTest, TestNotifyInterestedMetricsWithNewLoss) {
    auto usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_SOCKET_LOSS);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }

    // new socket loss event as result event metric should be notified about loss again
    usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_SOCKET_LOSS);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }
}

TEST_P(SocketLossInfoTest, TestDoNotNotifyInterestedMetricsIfNoUpdate) {
    auto usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_SOCKET_LOSS);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }

    // no more dropped events as result event metric should not be notified about loss events

    metricsReport = getMetricsReport(*mMetricsManager);
    EXPECT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(metricsReport.metrics(),
                Each(Property(&StatsLogReport::data_corrupted_reason_size, 0)));
}

class DataCorruptionQueueOverflowTest : public testing::Test {
protected:
    std::shared_ptr<MetricsManager> mMetricsManager;

    void SetUp() override {
        StatsdStats::getInstance().reset();

        sp<UidMap> uidMap;
        sp<StatsPullerManager> pullerManager = new StatsPullerManager();
        sp<AlarmMonitor> anomalyAlarmMonitor;
        sp<AlarmMonitor> periodicAlarmMonitor;

        // there will be one event metric interested in kInterestAtomId
        StatsdConfig config = buildGoodEventConfig();

        mMetricsManager = std::make_shared<MetricsManager>(
                kConfigKey, config, kTimeBaseSec, kTimeBaseSec, uidMap, pullerManager,
                anomalyAlarmMonitor, periodicAlarmMonitor);

        EXPECT_TRUE(mMetricsManager->isConfigValid());
    }

    void TearDown() override {
        StatsdStats::getInstance().reset();
    }
};

TEST_F(DataCorruptionQueueOverflowTest, TestNotifyOnlyInterestedMetrics) {
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kInterestAtomId,
                                                      /*isSkipped*/ false);

    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kUnusedAtomId,
                                                      /*isSkipped*/ false);

    EXPECT_TRUE(mMetricsManager->mQueueOverflowAtomsStats.empty());
    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            ASSERT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }
}

TEST_F(DataCorruptionQueueOverflowTest, TestNotifyInterestedMetricsWithNewLoss) {
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kInterestAtomId,
                                                      /*isSkipped*/ false);

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            ASSERT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }

    // new dropped event as result event metric should be notified about loss events
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs + 100, kInterestAtomId,
                                                      /*isSkipped*/ false);

    metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 2);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            ASSERT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }
}

TEST_F(DataCorruptionQueueOverflowTest, TestDoNotNotifyInterestedMetricsIfNoUpdate) {
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kInterestAtomId,
                                                      /*isSkipped*/ false);

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            ASSERT_EQ(statsLogReport.data_corrupted_reason_size(), 1);
            EXPECT_EQ(statsLogReport.data_corrupted_reason(0), DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW);
            continue;
        }
        EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
    }

    // no more dropped events as result event metric should not be notified about loss events

    metricsReport = getMetricsReport(*mMetricsManager);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);
    EXPECT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(metricsReport.metrics(),
                Each(Property(&StatsLogReport::data_corrupted_reason_size, 0)));
}

}  // namespace statsd
}  // namespace os
}  // namespace android

#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
