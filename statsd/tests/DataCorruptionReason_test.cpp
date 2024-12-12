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
constexpr int32_t kInterestAtomId = 3;
constexpr int32_t kNotInterestedMetricId = 3;
constexpr int32_t kUnusedAtomId = kInterestAtomId + 100;

const string kInterestAtomMatcherName = "CUSTOM_EVENT" + std::to_string(kInterestAtomId);
const string kInterestedMetricName = "EVENT_METRIC_INTERESTED_IN_" + kInterestAtomMatcherName;

const int64_t kInterestedMetricId = StringToId(kInterestedMetricName);

const string kAppName = "TestApp";
const set<int32_t> kAppUids = {kAppUid, kAppUid + 10000};
const map<string, set<int32_t>> kPkgToUids = {{kAppName, kAppUids}};

StatsdConfig buildGoodEventConfig() {
    StatsdConfig config;
    config.set_id(kConfigId);

    {
        auto atomMatcher = CreateSimpleAtomMatcher("SCREEN_IS_ON", SCREEN_STATE_ATOM_ID);
        *config.add_atom_matcher() = atomMatcher;
        *config.add_event_metric() =
                createEventMetric("EVENT_METRIC_SCREEN_IS_ON", atomMatcher.id(), std::nullopt);
    }

    {
        auto atomMatcher = CreateSimpleAtomMatcher(kInterestAtomMatcherName, kInterestAtomId);
        *config.add_atom_matcher() = atomMatcher;
        auto eventMetric = createEventMetric(kInterestedMetricName, atomMatcher.id(), std::nullopt);
        *config.add_event_metric() = eventMetric;
        EXPECT_EQ(eventMetric.id(), kInterestedMetricId);
    }

    return config;
}

ConfigMetricsReport getMetricsReport(MetricsManager& metricsManager, int64_t reportRequestTs) {
    ProtoOutputStream output;
    set<int32_t> usedUids;
    metricsManager.onDumpReport(reportRequestTs, reportRequestTs,
                                /*include_current_partial_bucket*/ true, /*erase_data*/ true,
                                /*dumpLatency*/ NO_TIME_CONSTRAINTS, nullptr, usedUids, &output);

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
            config.add_whitelisted_atom_ids(SCREEN_STATE_ATOM_ID);
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

public:
    void doPropagationTest() __INTRODUCED_IN(__ANDROID_API_T__);
    void doTestNotifyOnlyInterestedMetrics() __INTRODUCED_IN(__ANDROID_API_T__);
    void doTestNotifyInterestedMetricsWithNewLoss() __INTRODUCED_IN(__ANDROID_API_T__);
    void doTestDoNotNotifyInterestedMetricsIfNoUpdate() __INTRODUCED_IN(__ANDROID_API_T__);
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

TEST_P_GUARDED(SocketLossInfoTest, PropagationTest, __ANDROID_API_T__) {
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
        } else {
            EXPECT_EQ(metricProducer->mDataCorruptedDueToSocketLoss,
                      MetricProducer::DataCorruptionSeverity::kNone);
        }
    }
}

TEST_P_GUARDED(SocketLossInfoTest, TestNotifyOnlyInterestedMetrics, __ANDROID_API_T__) {
    const auto eventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kUnusedAtomId);

    mMetricsManager->onLogEvent(*eventSocketLossReported.get());
    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    EXPECT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(metricsReport.metrics(),
                Each(Property(&StatsLogReport::data_corrupted_reason_size, 0)));

    const auto usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs + 100);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_SOCKET_LOSS));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }
}

TEST_P_GUARDED(SocketLossInfoTest, TestNotifyInterestedMetricsWithNewLoss, __ANDROID_API_T__) {
    auto usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_SOCKET_LOSS));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }

    // new socket loss event as result event metric should be notified about loss again
    usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs + 100);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_SOCKET_LOSS));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }
}

TEST_P_GUARDED(SocketLossInfoTest, TestDoNotNotifyInterestedMetricsIfNoUpdate, __ANDROID_API_T__) {
    auto usedEventSocketLossReported = createSocketLossInfoLogEvent(kAppUid, kInterestAtomId);
    mMetricsManager->onLogEvent(*usedEventSocketLossReported.get());

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId && isAtomLoggingAllowed()) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_SOCKET_LOSS));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }

    // no more dropped events as result event metric should not be notified about loss events

    metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs + 100);
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

        // there will be one event metric interested in kInterestAtomId
        StatsdConfig config = buildGoodEventConfig();

        mMetricsManager =
                std::make_shared<MetricsManager>(kConfigKey, config, kTimeBaseSec, kTimeBaseSec,
                                                 uidMap, pullerManager, nullptr, nullptr);

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
    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(mMetricsManager->mQueueOverflowAtomsStats,
                UnorderedElementsAre(std::make_pair(kInterestAtomId, 1),
                                     std::make_pair(kUnusedAtomId, 1)));

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }
}

TEST_F(DataCorruptionQueueOverflowTest, TestNotifyInterestedMetricsWithNewLoss) {
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kInterestAtomId,
                                                      /*isSkipped*/ false);

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }

    // new dropped event as result event metric should be notified about loss events
    StatsdStats::getInstance().noteEventQueueOverflow(kReportRequestTimeNs + 100, kInterestAtomId,
                                                      /*isSkipped*/ false);

    metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs + 200);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 2);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }
}

TEST_F(DataCorruptionQueueOverflowTest, TestDoNotNotifyInterestedMetricsIfNoUpdate) {
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kInterestAtomId,
                                                      /*isSkipped*/ false);

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }

    // no more dropped events as result event metric should not be notified about loss events

    metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs + 100);
    ASSERT_EQ(mMetricsManager->mQueueOverflowAtomsStats.size(), 1);
    EXPECT_EQ(mMetricsManager->mQueueOverflowAtomsStats[kInterestAtomId], 1);
    EXPECT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(metricsReport.metrics(),
                Each(Property(&StatsLogReport::data_corrupted_reason_size, 0)));
}

TEST_F(DataCorruptionQueueOverflowTest, TestDoNotNotifyNewInterestedMetricsIfNoUpdate) {
    const int32_t kNewInterestAtomId = kUnusedAtomId + 1;

    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kInterestAtomId,
                                                      /*isSkipped*/ false);
    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, kNewInterestAtomId,
                                                      /*isSkipped*/ false);

    ConfigMetricsReport metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 2);
    EXPECT_THAT(mMetricsManager->mQueueOverflowAtomsStats,
                UnorderedElementsAre(std::make_pair(kInterestAtomId, 1),
                                     std::make_pair(kNewInterestAtomId, 1)));

    for (const auto& statsLogReport : metricsReport.metrics()) {
        if (statsLogReport.metric_id() == kInterestedMetricId) {
            EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                        ElementsAre(DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW));
        } else {
            EXPECT_EQ(statsLogReport.data_corrupted_reason_size(), 0);
        }
    }

    // adding 2 more metrics interested in atoms to update existing config
    // new metrics should not be updated with loss atom info from queue overflow
    // since atom loss events happen before metrics were added
    {
        StatsdConfig config = buildGoodEventConfig();
        const int64_t matcherId = StringToId(kInterestAtomMatcherName);
        *config.add_event_metric() =
                createEventMetric("EVENT_METRIC_FOR_EXISTING_ATOM", matcherId, std::nullopt);

        // adding new metric which is interested on unused atom before
        // for which lost event was detected
        auto atomMatcher = CreateSimpleAtomMatcher("NewTestMatcher", kNewInterestAtomId);
        *config.add_atom_matcher() = atomMatcher;
        *config.add_event_metric() =
                createEventMetric("EVENT_METRIC_FOR_NEW_ATOM", atomMatcher.id(), std::nullopt);

        mMetricsManager->updateConfig(config, kReportRequestTimeNs + 100,
                                      kReportRequestTimeNs + 100, nullptr, nullptr);
    }

    // no more dropped events as result event metric should not be notified about loss events

    metricsReport = getMetricsReport(*mMetricsManager, kReportRequestTimeNs + 200);
    EXPECT_THAT(mMetricsManager->mQueueOverflowAtomsStats,
                UnorderedElementsAre(std::make_pair(kInterestAtomId, 1),
                                     std::make_pair(kNewInterestAtomId, 1)));
    EXPECT_EQ(metricsReport.metrics_size(), 4);
    EXPECT_THAT(metricsReport.metrics(),
                Each(Property(&StatsLogReport::data_corrupted_reason_size, 0)));
}

TEST_GUARDED(DataCorruptionTest, TestStateLostPropagation, __ANDROID_API_T__) {
    // Initialize config with state and count metric
    StatsdConfig config;

    auto syncStartMatcher = CreateSyncStartAtomMatcher();
    *config.add_atom_matcher() = syncStartMatcher;

    auto state = CreateScreenState();
    *config.add_state() = state;

    // Create count metric that slices by screen state.
    auto countMetric = config.add_count_metric();
    countMetric->set_id(kNotInterestedMetricId);
    countMetric->set_what(syncStartMatcher.id());
    countMetric->set_bucket(TimeUnit::FIVE_MINUTES);
    countMetric->add_slice_by_state(state.id());

    // Initialize StatsLogProcessor.
    const uint64_t bucketSizeNs =
            TimeUnitToBucketSizeInMillis(config.count_metric(0).bucket()) * 1000000LL;
    int uid = 12345;
    int64_t cfgId = 98765;
    ConfigKey cfgKey(uid, cfgId);
    auto processor =
            CreateStatsLogProcessor(kBucketStartTimeNs, kBucketStartTimeNs, config, cfgKey);

    // Check that CountMetricProducer was initialized correctly.
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    sp<MetricsManager> metricsManager = processor->mMetricsManagers.begin()->second;
    EXPECT_TRUE(metricsManager->isConfigValid());

    // Check that StateTrackers were initialized correctly.
    EXPECT_EQ(1, StateManager::getInstance().getStateTrackersCount());
    EXPECT_EQ(1, StateManager::getInstance().getListenersCount(SCREEN_STATE_ATOM_ID));

    auto usedEventSocketLossReported =
            createSocketLossInfoLogEvent(AID_SYSTEM, SCREEN_STATE_ATOM_ID);
    processor->OnLogEvent(usedEventSocketLossReported.get());

    ConfigMetricsReport metricsReport = getMetricsReport(*metricsManager, kReportRequestTimeNs);
    ASSERT_EQ(metricsReport.metrics_size(), 1);
    const auto& statsLogReport = metricsReport.metrics(0);
    EXPECT_THAT(statsLogReport.data_corrupted_reason(), ElementsAre(DATA_CORRUPTED_SOCKET_LOSS));
}

TEST(DataCorruptionTest, TestStateLostFromQueueOverflowPropagation) {
    // Initialize config with state and count metric
    StatsdConfig config;

    auto syncStartMatcher = CreateSyncStartAtomMatcher();
    *config.add_atom_matcher() = syncStartMatcher;

    auto state = CreateScreenState();
    *config.add_state() = state;

    // Create count metric that slices by screen state.
    auto countMetric = config.add_count_metric();
    countMetric->set_id(kNotInterestedMetricId);
    countMetric->set_what(syncStartMatcher.id());
    countMetric->set_bucket(TimeUnit::FIVE_MINUTES);
    countMetric->add_slice_by_state(state.id());

    // Initialize StatsLogProcessor.
    const uint64_t bucketSizeNs =
            TimeUnitToBucketSizeInMillis(config.count_metric(0).bucket()) * 1000000LL;
    int uid = 12345;
    int64_t cfgId = 98765;
    ConfigKey cfgKey(uid, cfgId);
    auto processor =
            CreateStatsLogProcessor(kBucketStartTimeNs, kBucketStartTimeNs, config, cfgKey);

    // Check that CountMetricProducer was initialized correctly.
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    sp<MetricsManager> metricsManager = processor->mMetricsManagers.begin()->second;
    EXPECT_TRUE(metricsManager->isConfigValid());

    // Check that StateTrackers were initialized correctly.
    EXPECT_EQ(1, StateManager::getInstance().getStateTrackersCount());
    EXPECT_EQ(1, StateManager::getInstance().getListenersCount(SCREEN_STATE_ATOM_ID));

    StatsdStats::getInstance().noteEventQueueOverflow(kAtomsLogTimeNs, SCREEN_STATE_ATOM_ID,
                                                      /*isSkipped*/ false);

    vector<uint8_t> buffer;
    ConfigMetricsReportList reports;
    processor->onDumpReport(cfgKey, kBucketStartTimeNs + bucketSizeNs * 2 + 1, false, true,
                            ADB_DUMP, FAST, &buffer);
    ASSERT_GT(buffer.size(), 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    ASSERT_EQ(1, reports.reports_size());
    const ConfigMetricsReport metricsReport = reports.reports(0);
    ASSERT_EQ(metricsReport.metrics_size(), 1);
    const auto& statsLogReport = metricsReport.metrics(0);
    EXPECT_THAT(statsLogReport.data_corrupted_reason(),
                ElementsAre(DATA_CORRUPTED_EVENT_QUEUE_OVERFLOW));
}

}  // namespace statsd
}  // namespace os
}  // namespace android

#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
