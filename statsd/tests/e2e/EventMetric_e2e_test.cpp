// Copyright (C) 2019 The Android Open Source Project
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

#include <gtest/gtest.h>

#include <vector>

#include "flags/FlagProvider.h"
#include "src/StatsLogProcessor.h"
#include "src/state/StateTracker.h"
#include "src/stats_log_util.h"
#include "tests/statsd_test_util.h"

namespace android {
namespace os {
namespace statsd {

#ifdef __ANDROID__

// Setup for test fixture.
class EventMetricE2eTest : public ::testing::Test {
    void SetUp() override {
        FlagProvider::getInstance().overrideFuncs(&isAtLeastSFuncTrue);
    }

    void TearDown() override {
        FlagProvider::getInstance().resetOverrides();
    }

public:
    void doTestRepeatedFieldsAndEmptyArrays();
    void doTestMatchRepeatedFieldPositionFirst();
};

TEST_F(EventMetricE2eTest, TestEventMetricDataAggregated) {
    StatsdConfig config;

    AtomMatcher wakelockAcquireMatcher = CreateAcquireWakelockAtomMatcher();
    *config.add_atom_matcher() = wakelockAcquireMatcher;

    EventMetric wakelockEventMetric =
            createEventMetric("EventWakelockStateChanged", wakelockAcquireMatcher.id(), nullopt);
    *config.add_event_metric() = wakelockEventMetric;

    ConfigKey key(123, 987);
    uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Initialize log events before update.
    std::vector<std::unique_ptr<LogEvent>> events;

    int app1Uid = 123;
    vector<int> attributionUids = {app1Uid};
    std::vector<string> attributionTags = {"App1"};

    events.push_back(CreateAcquireWakelockEvent(bucketStartTimeNs + 10 * NS_PER_SEC,
                                                attributionUids, attributionTags, "wl1"));
    events.push_back(CreateAcquireWakelockEvent(bucketStartTimeNs + 20 * NS_PER_SEC,
                                                attributionUids, attributionTags, "wl1"));
    events.push_back(CreateAcquireWakelockEvent(bucketStartTimeNs + 30 * NS_PER_SEC,
                                                attributionUids, attributionTags, "wl2"));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    uint64_t dumpTimeNs = bucketStartTimeNs + 100 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    EXPECT_TRUE(report.has_estimated_data_bytes());
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport wakelockEventMetricReport = report.metrics(0);
    EXPECT_TRUE(wakelockEventMetricReport.has_estimated_data_bytes());
    EXPECT_EQ(wakelockEventMetricReport.metric_id(), wakelockEventMetric.id());
    EXPECT_TRUE(wakelockEventMetricReport.has_event_metrics());
    ASSERT_EQ(wakelockEventMetricReport.event_metrics().data_size(), 3);
    auto data = wakelockEventMetricReport.event_metrics().data(0);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 10 * NS_PER_SEC);
    EXPECT_EQ(data.atom().wakelock_state_changed().tag(), "wl1");
    data = wakelockEventMetricReport.event_metrics().data(1);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 20 * NS_PER_SEC);
    EXPECT_EQ(data.atom().wakelock_state_changed().tag(), "wl1");
    data = wakelockEventMetricReport.event_metrics().data(2);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 30 * NS_PER_SEC);
    EXPECT_EQ(data.atom().wakelock_state_changed().tag(), "wl2");
}

TEST_F_GUARDED(EventMetricE2eTest, TestRepeatedFieldsAndEmptyArrays, __ANDROID_API_T__) {
    StatsdConfig config;

    AtomMatcher testAtomReportedAtomMatcher =
            CreateSimpleAtomMatcher("TestAtomReportedMatcher", util::TEST_ATOM_REPORTED);
    *config.add_atom_matcher() = testAtomReportedAtomMatcher;

    EventMetric testAtomReportedEventMetric =
            createEventMetric("EventTestAtomReported", testAtomReportedAtomMatcher.id(), nullopt);
    *config.add_event_metric() = testAtomReportedEventMetric;

    ConfigKey key(123, 987);
    uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Initialize log events before update.
    std::vector<std::unique_ptr<LogEvent>> events;

    vector<int> intArray = {3, 6};
    vector<int64_t> longArray = {1000L, 10002L};
    vector<float> floatArray = {0.3f, 0.09f};
    vector<string> stringArray = {"str1", "str2"};
    int boolArrayLength = 2;
    bool boolArray[boolArrayLength];
    boolArray[0] = 1;
    boolArray[1] = 0;
    vector<uint8_t> boolArrayVector = {1, 0};
    vector<int> enumArray = {TestAtomReported::ON, TestAtomReported::OFF};

    events.push_back(CreateTestAtomReportedEventVariableRepeatedFields(
            bucketStartTimeNs + 10 * NS_PER_SEC, intArray, longArray, floatArray, stringArray,
            boolArray, boolArrayLength, enumArray));
    events.push_back(CreateTestAtomReportedEventVariableRepeatedFields(
            bucketStartTimeNs + 20 * NS_PER_SEC, {}, {}, {}, {}, {}, 0, {}));
    events.push_back(CreateTestAtomReportedEventVariableRepeatedFields(
            bucketStartTimeNs + 30 * NS_PER_SEC, {}, {}, {}, {}, {}, 0, enumArray));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    uint64_t dumpTimeNs = bucketStartTimeNs + 100 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport testAtomEventMetricReport = report.metrics(0);
    EXPECT_EQ(testAtomEventMetricReport.metric_id(), testAtomReportedEventMetric.id());
    EXPECT_TRUE(testAtomEventMetricReport.has_event_metrics());
    ASSERT_EQ(testAtomEventMetricReport.event_metrics().data_size(), 3);

    EventMetricData data = testAtomEventMetricReport.event_metrics().data(0);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 10 * NS_PER_SEC);
    TestAtomReported atom = data.atom().test_atom_reported();
    EXPECT_THAT(atom.repeated_int_field(), ElementsAreArray(intArray));
    EXPECT_THAT(atom.repeated_long_field(), ElementsAreArray(longArray));
    EXPECT_THAT(atom.repeated_float_field(), ElementsAreArray(floatArray));
    EXPECT_THAT(atom.repeated_string_field(), ElementsAreArray(stringArray));
    EXPECT_THAT(atom.repeated_boolean_field(), ElementsAreArray(boolArrayVector));
    EXPECT_THAT(atom.repeated_enum_field(), ElementsAreArray(enumArray));

    data = testAtomEventMetricReport.event_metrics().data(1);
    atom = data.atom().test_atom_reported();
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 20 * NS_PER_SEC);
    EXPECT_EQ(atom.repeated_int_field_size(), 0);
    EXPECT_EQ(atom.repeated_long_field_size(), 0);
    EXPECT_EQ(atom.repeated_float_field_size(), 0);
    EXPECT_EQ(atom.repeated_string_field_size(), 0);
    EXPECT_EQ(atom.repeated_boolean_field_size(), 0);
    EXPECT_EQ(atom.repeated_enum_field_size(), 0);

    data = testAtomEventMetricReport.event_metrics().data(2);
    atom = data.atom().test_atom_reported();
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 30 * NS_PER_SEC);
    EXPECT_EQ(atom.repeated_int_field_size(), 0);
    EXPECT_EQ(atom.repeated_long_field_size(), 0);
    EXPECT_EQ(atom.repeated_float_field_size(), 0);
    EXPECT_EQ(atom.repeated_string_field_size(), 0);
    EXPECT_EQ(atom.repeated_boolean_field_size(), 0);
    EXPECT_THAT(atom.repeated_enum_field(), ElementsAreArray(enumArray));
}

TEST_F_GUARDED(EventMetricE2eTest, TestMatchRepeatedFieldPositionFirst, __ANDROID_API_T__) {
    StatsdConfig config;

    AtomMatcher testAtomReportedStateFirstOnAtomMatcher =
            CreateTestAtomRepeatedStateFirstOnAtomMatcher();
    *config.add_atom_matcher() = testAtomReportedStateFirstOnAtomMatcher;

    EventMetric testAtomReportedEventMetric = createEventMetric(
            "EventTestAtomReported", testAtomReportedStateFirstOnAtomMatcher.id(), nullopt);
    *config.add_event_metric() = testAtomReportedEventMetric;

    ConfigKey key(123, 987);
    uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Initialize log events before update.
    std::vector<std::unique_ptr<LogEvent>> events;

    vector<int> enumArrayNoMatch = {TestAtomReported::OFF, TestAtomReported::ON};
    vector<int> enumArrayMatch = {TestAtomReported::ON, TestAtomReported::OFF};

    events.push_back(CreateTestAtomReportedEventVariableRepeatedFields(
            bucketStartTimeNs + 10 * NS_PER_SEC, {}, {}, {}, {}, {}, 0, enumArrayNoMatch));
    events.push_back(CreateTestAtomReportedEventVariableRepeatedFields(
            bucketStartTimeNs + 20 * NS_PER_SEC, {}, {}, {}, {}, {}, 0, enumArrayMatch));
    // No matching is done on an empty array.
    events.push_back(CreateTestAtomReportedEventVariableRepeatedFields(
            bucketStartTimeNs + 30 * NS_PER_SEC, {}, {}, {}, {}, {}, 0, {}));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    uint64_t dumpTimeNs = bucketStartTimeNs + 100 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport testAtomEventMetricReport = report.metrics(0);
    EXPECT_EQ(testAtomEventMetricReport.metric_id(), testAtomReportedEventMetric.id());
    EXPECT_TRUE(testAtomEventMetricReport.has_event_metrics());
    ASSERT_EQ(testAtomEventMetricReport.event_metrics().data_size(), 1);

    EventMetricData data = testAtomEventMetricReport.event_metrics().data(0);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 20 * NS_PER_SEC);
    TestAtomReported atom = data.atom().test_atom_reported();
    ASSERT_EQ(atom.repeated_int_field_size(), 0);
    ASSERT_EQ(atom.repeated_long_field_size(), 0);
    ASSERT_EQ(atom.repeated_float_field_size(), 0);
    ASSERT_EQ(atom.repeated_string_field_size(), 0);
    ASSERT_EQ(atom.repeated_boolean_field_size(), 0);
    EXPECT_THAT(atom.repeated_enum_field(), ElementsAreArray(enumArrayMatch));
}

TEST_F(EventMetricE2eTest, TestDumpReportIncrementsReportNumber) {
    StatsdConfig config;

    AtomMatcher testAtomReportedStateFirstOnAtomMatcher =
            CreateTestAtomRepeatedStateFirstOnAtomMatcher();
    *config.add_atom_matcher() = testAtomReportedStateFirstOnAtomMatcher;

    EventMetric testAtomReportedEventMetric = createEventMetric(
            "EventTestAtomReported", testAtomReportedStateFirstOnAtomMatcher.id(), nullopt);
    *config.add_event_metric() = testAtomReportedEventMetric;

    ConfigKey key(123, 987);
    uint64_t configUpdateTime = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(configUpdateTime, configUpdateTime, config, key);

    uint64_t dumpTimeNs = configUpdateTime + 100 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    ASSERT_EQ(reports.reports_size(), 1);

    EXPECT_EQ(reports.report_number(), 1);
    EXPECT_EQ(reports.statsd_stats_id(), StatsdStats::getInstance().getStatsdStatsId());

    buffer.clear();
    processor->onDumpReport(key, dumpTimeNs + 100, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    ASSERT_EQ(reports.reports_size(), 1);

    EXPECT_EQ(reports.report_number(), 2);
    EXPECT_EQ(reports.statsd_stats_id(), StatsdStats::getInstance().getStatsdStatsId());
}

TEST_F(EventMetricE2eTest, TestEventMetricSampling) {
    // Set srand seed to make rand deterministic for testing.
    srand(0);

    StatsdConfig config;

    AtomMatcher batterySaverOnMatcher = CreateBatterySaverModeStartAtomMatcher();
    *config.add_atom_matcher() = batterySaverOnMatcher;

    EventMetric batterySaverOnEventMetric =
            createEventMetric("EventBatterySaverOn", batterySaverOnMatcher.id(), nullopt);
    batterySaverOnEventMetric.set_sampling_percentage(50);
    *config.add_event_metric() = batterySaverOnEventMetric;

    ConfigKey key(123, 987);
    uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Initialize log events before update.
    std::vector<std::unique_ptr<LogEvent>> events;

    for (int i = 0; i < 100; i++) {
        events.push_back(CreateBatterySaverOnEvent(bucketStartTimeNs + (10 + 10 * i) * NS_PER_SEC));
    }

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    uint64_t dumpTimeNs = bucketStartTimeNs + 2000 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_EQ(metricReport.metric_id(), batterySaverOnEventMetric.id());
    EXPECT_TRUE(metricReport.has_event_metrics());
    ASSERT_EQ(metricReport.event_metrics().data_size(), 46);
}

/**
 * Test an event metric that has one slice_by_state with no primary fields.
 *
 * Once the EventMetricProducer is initialized, it has one atom id in
 * mSlicedStateAtoms and no entries in mStateGroupMap.

 * One StateTracker tracks the state atom, and it has one listener which is the
 * EventMetricProducer that was initialized.
 */
TEST_F(EventMetricE2eTest, TestSlicedState) {
    // Initialize config.
    StatsdConfig config;

    auto syncStartMatcher = CreateSyncStartAtomMatcher();
    *config.add_atom_matcher() = syncStartMatcher;

    auto state = CreateScreenState();
    *config.add_state() = state;

    // Create event metric that slices by screen state.
    EventMetric syncStateEventMetric =
            createEventMetric("SyncStartReported", syncStartMatcher.id(), nullopt, {state.id()});
    *config.add_event_metric() = syncStateEventMetric;

    // Initialize StatsLogProcessor.
    const uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    ConfigKey key(123, 987);
    auto processor = CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Check that EventMetricProducer was initialized correctly.
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    sp<MetricsManager> metricsManager = processor->mMetricsManagers.begin()->second;
    EXPECT_TRUE(metricsManager->isConfigValid());
    ASSERT_EQ(metricsManager->mAllMetricProducers.size(), 1);
    sp<MetricProducer> metricProducer = metricsManager->mAllMetricProducers[0];
    ASSERT_EQ(metricProducer->mSlicedStateAtoms.size(), 1);
    EXPECT_EQ(metricProducer->mSlicedStateAtoms.at(0), SCREEN_STATE_ATOM_ID);
    ASSERT_EQ(metricProducer->mStateGroupMap.size(), 0);

    // Check that StateTrackers were initialized correctly.
    EXPECT_EQ(1, StateManager::getInstance().getStateTrackersCount());
    EXPECT_EQ(1, StateManager::getInstance().getListenersCount(SCREEN_STATE_ATOM_ID));

    // Initialize log events.
    std::vector<int> attributionUids1 = {123};
    std::vector<string> attributionTags1 = {"App1"};

    std::vector<std::unique_ptr<LogEvent>> events;
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 50 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_ON));  // 1:00
    events.push_back(CreateSyncStartEvent(bucketStartTimeNs + 75 * NS_PER_SEC, attributionUids1,
                                          attributionTags1, "sync_name"));  // 1:25
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 200 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_OFF));  // 3:30
    events.push_back(CreateSyncStartEvent(bucketStartTimeNs + 250 * NS_PER_SEC, attributionUids1,
                                          attributionTags1, "sync_name"));  // 4:20

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    // Check dump report.
    uint64_t dumpTimeNs = bucketStartTimeNs + 2000 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_EQ(metricReport.metric_id(), syncStateEventMetric.id());
    EXPECT_TRUE(metricReport.has_event_metrics());
    ASSERT_EQ(metricReport.event_metrics().data_size(), 2);

    // For each EventMetricData, check StateValue info is correct
    EventMetricData data = metricReport.event_metrics().data(0);

    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 75 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(android::view::DisplayStateEnum::DISPLAY_STATE_ON, data.slice_by_state(0).value());

    data = metricReport.event_metrics().data(1);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 250 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(android::view::DisplayStateEnum::DISPLAY_STATE_OFF, data.slice_by_state(0).value());
}

/**
 * Test an event metric that has one slice_by_state with a mapping and no
 * primary fields.
 *
 * Once the EventMetricProducer is initialized, it has one atom id in
 * mSlicedStateAtoms and has one entry per state value in mStateGroupMap.
 *
 * One StateTracker tracks the state atom, and it has one listener which is the
 * EventMetricProducer that was initialized.
 */
TEST_F(EventMetricE2eTest, TestSlicedStateWithMap) {
    // Initialize config.
    StatsdConfig config;

    auto syncStartMatcher = CreateSyncStartAtomMatcher();
    *config.add_atom_matcher() = syncStartMatcher;

    int64_t screenOnId = 4444;
    int64_t screenOffId = 9876;
    auto state = CreateScreenStateWithOnOffMap(screenOnId, screenOffId);
    *config.add_state() = state;

    // Create event metric that slices by screen state with on/off map.
    EventMetric syncStateEventMetric =
            createEventMetric("SyncStartReported", syncStartMatcher.id(), nullopt, {state.id()});
    *config.add_event_metric() = syncStateEventMetric;

    // Initialize StatsLogProcessor.
    const uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    ConfigKey key(123, 987);
    auto processor = CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    /*
    |     1     2     3     4(minutes)
    |-----------------------|-
      x   x     x       x     (syncStartEvents)
     -------------------------SCREEN_OFF events
             |                (ScreenStateOffEvent = 1)
         |                    (ScreenStateDozeEvent = 3)
     -------------------------SCREEN_ON events
       |                      (ScreenStateOnEvent = 2)
                      |       (ScreenStateVrEvent = 5)

    Based on the diagram above, a Sync Start Event querying for Screen State would return:
    - Event 0: StateTracker::kStateUnknown
    - Event 1: Off
    - Event 2: Off
    - Event 3: On
    */
    // Initialize log events
    std::vector<int> attributionUids1 = {123};
    std::vector<string> attributionTags1 = {"App1"};

    std::vector<std::unique_ptr<LogEvent>> events;
    events.push_back(CreateSyncStartEvent(bucketStartTimeNs + 20 * NS_PER_SEC, attributionUids1,
                                          attributionTags1, "sync_name"));  // 0:30
    // Event 0 Occurred
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 30 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_ON));  // 0:40
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 50 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_DOZE));  // 1:00
    events.push_back(CreateSyncStartEvent(bucketStartTimeNs + 60 * NS_PER_SEC, attributionUids1,
                                          attributionTags1, "sync_name"));  // 1:10
    // Event 1 Occurred
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 90 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_OFF));  // 1:40
    events.push_back(CreateSyncStartEvent(bucketStartTimeNs + 120 * NS_PER_SEC, attributionUids1,
                                          attributionTags1, "sync_name"));  // 2:10
    // Event 2 Occurred
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 180 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_VR));  // 3:10
    events.push_back(CreateSyncStartEvent(bucketStartTimeNs + 200 * NS_PER_SEC, attributionUids1,
                                          attributionTags1, "sync_name"));  // 3:30
    // Event 3 Occurred

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    // Check dump report.
    uint64_t dumpTimeNs = bucketStartTimeNs + 2000 * NS_PER_SEC;
    vector<uint8_t> buffer;
    ConfigMetricsReportList reports;
    processor->onDumpReport(key, dumpTimeNs, false, true, ADB_DUMP, FAST, &buffer);
    ASSERT_GT(buffer.size(), 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_EQ(metricReport.metric_id(), syncStateEventMetric.id());
    EXPECT_TRUE(metricReport.has_event_metrics());
    ASSERT_EQ(metricReport.event_metrics().data_size(), 4);

    // For each EventMetricData, check StateValue info is correct
    EventMetricData data = metricReport.event_metrics().data(0);

    // StateTracker::kStateUnknown
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 20 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(-1 /* StateTracker::kStateUnknown */, data.slice_by_state(0).value());

    // Off
    data = metricReport.event_metrics().data(1);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 60 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_group_id());
    EXPECT_EQ(screenOffId, data.slice_by_state(0).group_id());

    // Off
    data = metricReport.event_metrics().data(2);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 120 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_group_id());
    EXPECT_EQ(screenOffId, data.slice_by_state(0).group_id());

    // On
    data = metricReport.event_metrics().data(3);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 200 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_group_id());
    EXPECT_EQ(screenOnId, data.slice_by_state(0).group_id());
}

/**
* Test an event metric that has one slice_by_state with a primary field.

* Once the EventMetricProducer is initialized, it should have one
* MetricStateLink stored. State querying using a non-empty primary key
* should also work as intended.
*/
TEST_F(EventMetricE2eTest, TestSlicedStateWithPrimaryFields) {
    // Initialize config.
    StatsdConfig config;

    auto appCrashMatcher = CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);
    *config.add_atom_matcher() = appCrashMatcher;

    auto state = CreateUidProcessState();
    *config.add_state() = state;

    // Create event metric that slices by uid process state.
    EventMetric appCrashEventMetric =
            createEventMetric("AppCrashReported", appCrashMatcher.id(), nullopt, {state.id()});
    MetricStateLink* stateLink = appCrashEventMetric.add_state_link();
    stateLink->set_state_atom_id(UID_PROCESS_STATE_ATOM_ID);
    auto fieldsInWhat = stateLink->mutable_fields_in_what();
    *fieldsInWhat = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    auto fieldsInState = stateLink->mutable_fields_in_state();
    *fieldsInState = CreateDimensions(UID_PROCESS_STATE_ATOM_ID, {1 /*uid*/});
    *config.add_event_metric() = appCrashEventMetric;

    // Initialize StatsLogProcessor.
    const uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    ConfigKey key(123, 987);
    auto processor = CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    /*
    NOTE: "1" or "2" represents the uid associated with the state/app crash event
    |    1    2    3
    |--------------|-
      1   1       2 1(AppCrashEvents)
     ----------------PROCESS STATE events
            2        (TopEvent = 1002)
       1             (ImportantForegroundEvent = 1005)

    Based on the diagram above, an AppCrashEvent querying for process state value would return:
    - Event 0: StateTracker::kStateUnknown
    - Event 1: Important Foreground
    - Event 2: Top
    - Event 3: Important Foreground
    */
    // Initialize log events
    std::vector<std::unique_ptr<LogEvent>> events;
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 20 * NS_PER_SEC, 1 /*uid*/));  // 0:30
    // Event 0 Occurred
    events.push_back(CreateUidProcessStateChangedEvent(
            bucketStartTimeNs + 30 * NS_PER_SEC, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND));  // 0:40
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 60 * NS_PER_SEC, 1 /*uid*/));  // 1:10
    // Event 1 Occurred
    events.push_back(CreateUidProcessStateChangedEvent(
            bucketStartTimeNs + 90 * NS_PER_SEC, 2 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_TOP));  // 1:40
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 160 * NS_PER_SEC, 2 /*uid*/));  // 2:50
    // Event 2 Occurred
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 180 * NS_PER_SEC, 1 /*uid*/));  // 3:10
    // Event 3 Occurred

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    // Check dump report.
    uint64_t dumpTimeNs = bucketStartTimeNs + 2000 * NS_PER_SEC;
    vector<uint8_t> buffer;
    ConfigMetricsReportList reports;
    processor->onDumpReport(key, dumpTimeNs, false, true, ADB_DUMP, FAST, &buffer);
    ASSERT_GT(buffer.size(), 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_EQ(metricReport.metric_id(), appCrashEventMetric.id());
    EXPECT_TRUE(metricReport.has_event_metrics());
    ASSERT_EQ(metricReport.event_metrics().data_size(), 4);

    // For each EventMetricData, check StateValue info is correct
    EventMetricData data = metricReport.event_metrics().data(0);

    // StateTracker::kStateUnknown
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 20 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(-1 /* StateTracker::kStateUnknown */, data.slice_by_state(0).value());

    // Important Foreground
    data = metricReport.event_metrics().data(1);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 60 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_IMPORTANT_FOREGROUND, data.slice_by_state(0).value());

    // Top
    data = metricReport.event_metrics().data(2);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 160 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_TOP, data.slice_by_state(0).value());

    // Important Foreground
    data = metricReport.event_metrics().data(3);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 180 * NS_PER_SEC);
    ASSERT_EQ(1, data.slice_by_state_size());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_IMPORTANT_FOREGROUND, data.slice_by_state(0).value());
}

TEST_F(EventMetricE2eTest, TestMultipleSlicedStates) {
    // Initialize config.
    StatsdConfig config;

    auto appCrashMatcher = CreateSimpleAtomMatcher("APP_CRASH_OCCURRED", util::APP_CRASH_OCCURRED);
    *config.add_atom_matcher() = appCrashMatcher;

    int64_t screenOnId = 4444;
    int64_t screenOffId = 9876;
    auto state1 = CreateScreenStateWithOnOffMap(screenOnId, screenOffId);
    *config.add_state() = state1;
    auto state2 = CreateUidProcessState();
    *config.add_state() = state2;

    // Create event metric that slices by screen state with on/off map and
    // slices by uid process state.
    EventMetric appCrashEventMetric = createEventMetric("AppCrashReported", appCrashMatcher.id(),
                                                        nullopt, {state1.id(), state2.id()});
    MetricStateLink* stateLink = appCrashEventMetric.add_state_link();
    stateLink->set_state_atom_id(UID_PROCESS_STATE_ATOM_ID);
    auto fieldsInWhat = stateLink->mutable_fields_in_what();
    *fieldsInWhat = CreateDimensions(util::APP_CRASH_OCCURRED, {1 /*uid*/});
    auto fieldsInState = stateLink->mutable_fields_in_state();
    *fieldsInState = CreateDimensions(UID_PROCESS_STATE_ATOM_ID, {1 /*uid*/});
    *config.add_event_metric() = appCrashEventMetric;

    // Initialize StatsLogProcessor.
    const uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    ConfigKey key(123, 987);
    auto processor = CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    /*
      |    1    2    3  (minutes)
      |-----------------
        1  1    1     1 (AppCrashEvents)
       -----------------SCREEN_OFF events
             |          (ScreenOffEvent = 1)
         |              (ScreenDozeEvent = 3)
       -----------------SCREEN_ON events
                   |    (ScreenOnEvent = 2)
       -----------------PROCESS STATE events
             1          (TopEvent = 1002)
       1          1     (ImportantForegroundEvent = 1005)

       Based on the diagram above, Screen State / Process State pairs for each
       AppCrashEvent are:
       - 0: StateTracker::kStateUnknown / Important Foreground
       - 1: Off / Important Foreground
       - 2: Off / Top
       - 3: On  / Important Foreground
      */

    // Initialize log events
    std::vector<std::unique_ptr<LogEvent>> events;
    events.push_back(CreateUidProcessStateChangedEvent(
            bucketStartTimeNs + 5 * NS_PER_SEC, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND));  // 0:15
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 20 * NS_PER_SEC, 1 /*uid*/));  // 0:30
    // Event 0 Occurred
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 30 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_DOZE));  // 0:40
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 60 * NS_PER_SEC, 1 /*uid*/));  // 1:10
    // Event 1 Occurred
    events.push_back(CreateUidProcessStateChangedEvent(
            bucketStartTimeNs + 90 * NS_PER_SEC, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_TOP));  // 1:40
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 90 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_OFF));  // 1:40
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 120 * NS_PER_SEC, 1 /*uid*/));  // 2:10
    // Event 2 Occurred
    events.push_back(CreateUidProcessStateChangedEvent(
            bucketStartTimeNs + 150 * NS_PER_SEC, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND));  // 2:40
    events.push_back(CreateScreenStateChangedEvent(
            bucketStartTimeNs + 160 * NS_PER_SEC,
            android::view::DisplayStateEnum::DISPLAY_STATE_ON));  // 2:50
    events.push_back(
            CreateAppCrashOccurredEvent(bucketStartTimeNs + 200 * NS_PER_SEC, 1 /*uid*/));  // 3:30
    // Event 3 Occurred

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    // Check dump report.
    uint64_t dumpTimeNs = bucketStartTimeNs + 2000 * NS_PER_SEC;
    vector<uint8_t> buffer;
    ConfigMetricsReportList reports;
    processor->onDumpReport(key, dumpTimeNs, false, true, ADB_DUMP, FAST, &buffer);
    ASSERT_GT(buffer.size(), 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_EQ(metricReport.metric_id(), appCrashEventMetric.id());
    EXPECT_TRUE(metricReport.has_event_metrics());
    ASSERT_EQ(metricReport.event_metrics().data_size(), 4);

    // For each EventMetricData, check StateValue info is correct
    EventMetricData data = metricReport.event_metrics().data(0);

    // Screen State: StateTracker::kStateUnknown
    // Process State: Important Foreground
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 20 * NS_PER_SEC);
    ASSERT_EQ(2, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_value());
    EXPECT_EQ(-1 /* StateTracker::kStateUnknown */, data.slice_by_state(0).value());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(1).atom_id());
    EXPECT_TRUE(data.slice_by_state(1).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_IMPORTANT_FOREGROUND, data.slice_by_state(1).value());

    // Screen State: Off
    // Process State: Important Foreground
    data = metricReport.event_metrics().data(1);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 60 * NS_PER_SEC);
    ASSERT_EQ(2, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_group_id());
    EXPECT_EQ(screenOffId, data.slice_by_state(0).group_id());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(1).atom_id());
    EXPECT_TRUE(data.slice_by_state(1).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_IMPORTANT_FOREGROUND, data.slice_by_state(1).value());

    // Screen State: Off
    // Process State: Top
    data = metricReport.event_metrics().data(2);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 120 * NS_PER_SEC);
    ASSERT_EQ(2, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_group_id());
    EXPECT_EQ(screenOffId, data.slice_by_state(0).group_id());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(1).atom_id());
    EXPECT_TRUE(data.slice_by_state(1).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_TOP, data.slice_by_state(1).value());

    // Screen State: On
    // Process State: Important Foreground
    data = metricReport.event_metrics().data(3);
    EXPECT_EQ(data.elapsed_timestamp_nanos(), bucketStartTimeNs + 200 * NS_PER_SEC);
    ASSERT_EQ(2, data.slice_by_state_size());
    EXPECT_EQ(SCREEN_STATE_ATOM_ID, data.slice_by_state(0).atom_id());
    EXPECT_TRUE(data.slice_by_state(0).has_group_id());
    EXPECT_EQ(screenOnId, data.slice_by_state(0).group_id());
    EXPECT_EQ(UID_PROCESS_STATE_ATOM_ID, data.slice_by_state(1).atom_id());
    EXPECT_TRUE(data.slice_by_state(1).has_value());
    EXPECT_EQ(android::app::PROCESS_STATE_IMPORTANT_FOREGROUND, data.slice_by_state(1).value());
}

TEST_F(EventMetricE2eTest, TestEventMetricFieldsFilter) {
    StatsdConfig config;

    AtomMatcher testAtomReportedAtomMatcher =
            CreateSimpleAtomMatcher("TestAtomReportedMatcher", util::TEST_ATOM_REPORTED);
    *config.add_atom_matcher() = testAtomReportedAtomMatcher;

    EventMetric metric =
            createEventMetric("EventTestAtomReported", testAtomReportedAtomMatcher.id(), nullopt);
    metric.mutable_fields_filter()->mutable_fields()->set_field(util::TEST_ATOM_REPORTED);
    metric.mutable_fields_filter()->mutable_fields()->add_child()->set_field(2);  // int_field
    *config.add_event_metric() = metric;

    ConfigKey key(123, 987);
    uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Initialize log events before update.
    std::vector<std::unique_ptr<LogEvent>> events;

    events.push_back(CreateTestAtomReportedEventWithPrimitives(
            bucketStartTimeNs + 10 * NS_PER_SEC, 1 /* intField */, 1l /* longField */,
            1.0f /* floatField */, "string_field1", false /* boolField */,
            TestAtomReported::OFF /* enumField */));
    events.push_back(CreateTestAtomReportedEventWithPrimitives(
            bucketStartTimeNs + 20 * NS_PER_SEC, 2 /* intField */, 2l /* longField */,
            2.0f /* floatField */, "string_field2", true /* boolField */,
            TestAtomReported::ON /* enumField */));
    events.push_back(CreateTestAtomReportedEventWithPrimitives(
            bucketStartTimeNs + 30 * NS_PER_SEC, 3 /* intField */, 3l /* longField */,
            3.0f /* floatField */, "string_field3", false /* boolField */,
            TestAtomReported::ON /* enumField */));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    uint64_t dumpTimeNs = bucketStartTimeNs + 100 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport testAtomEventMetricReport = report.metrics(0);
    EXPECT_EQ(testAtomEventMetricReport.metric_id(), metric.id());
    EXPECT_TRUE(testAtomEventMetricReport.has_event_metrics());
    ASSERT_EQ(testAtomEventMetricReport.event_metrics().data_size(), 3);

    TestAtomReported atom =
            testAtomEventMetricReport.event_metrics().data(0).atom().test_atom_reported();
    EXPECT_EQ(atom.int_field(), 1);
    EXPECT_FALSE(atom.has_long_field());
    EXPECT_FALSE(atom.has_float_field());
    EXPECT_FALSE(atom.has_string_field());
    EXPECT_FALSE(atom.has_boolean_field());
    EXPECT_FALSE(atom.has_state());

    atom = testAtomEventMetricReport.event_metrics().data(1).atom().test_atom_reported();
    EXPECT_EQ(atom.int_field(), 2);
    EXPECT_FALSE(atom.has_long_field());
    EXPECT_FALSE(atom.has_float_field());
    EXPECT_FALSE(atom.has_string_field());
    EXPECT_FALSE(atom.has_boolean_field());
    EXPECT_FALSE(atom.has_state());

    atom = testAtomEventMetricReport.event_metrics().data(2).atom().test_atom_reported();
    EXPECT_EQ(atom.int_field(), 3);
    EXPECT_FALSE(atom.has_long_field());
    EXPECT_FALSE(atom.has_float_field());
    EXPECT_FALSE(atom.has_string_field());
    EXPECT_FALSE(atom.has_boolean_field());
    EXPECT_FALSE(atom.has_state());
}

TEST_F(EventMetricE2eTest, TestEventMetricFieldsFilterOmit) {
    StatsdConfig config;

    AtomMatcher testAtomReportedAtomMatcher =
            CreateSimpleAtomMatcher("TestAtomReportedMatcher", util::TEST_ATOM_REPORTED);
    *config.add_atom_matcher() = testAtomReportedAtomMatcher;

    EventMetric metric =
            createEventMetric("EventTestAtomReported", testAtomReportedAtomMatcher.id(), nullopt);
    metric.mutable_fields_filter()->mutable_omit_fields()->set_field(util::TEST_ATOM_REPORTED);
    metric.mutable_fields_filter()->mutable_omit_fields()->add_child()->set_field(2);  // int_field
    *config.add_event_metric() = metric;

    ConfigKey key(123, 987);
    uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    sp<StatsLogProcessor> processor =
            CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, key);

    // Initialize log events before update.
    std::vector<std::unique_ptr<LogEvent>> events;

    events.push_back(CreateTestAtomReportedEventWithPrimitives(
            bucketStartTimeNs + 10 * NS_PER_SEC, 1 /* intField */, 1l /* longField */,
            1.0f /* floatField */, "string_field1", false /* boolField */,
            TestAtomReported::OFF /* enumField */));
    events.push_back(CreateTestAtomReportedEventWithPrimitives(
            bucketStartTimeNs + 20 * NS_PER_SEC, 2 /* intField */, 2l /* longField */,
            2.0f /* floatField */, "string_field2", true /* boolField */,
            TestAtomReported::ON /* enumField */));
    events.push_back(CreateTestAtomReportedEventWithPrimitives(
            bucketStartTimeNs + 30 * NS_PER_SEC, 3 /* intField */, 3l /* longField */,
            3.0f /* floatField */, "string_field3", false /* boolField */,
            TestAtomReported::ON /* enumField */));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    uint64_t dumpTimeNs = bucketStartTimeNs + 100 * NS_PER_SEC;
    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);

    ConfigMetricsReport report = reports.reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport testAtomEventMetricReport = report.metrics(0);
    EXPECT_EQ(testAtomEventMetricReport.metric_id(), metric.id());
    EXPECT_TRUE(testAtomEventMetricReport.has_event_metrics());
    ASSERT_EQ(testAtomEventMetricReport.event_metrics().data_size(), 3);

    TestAtomReported atom =
            testAtomEventMetricReport.event_metrics().data(0).atom().test_atom_reported();
    EXPECT_FALSE(atom.has_int_field());
    EXPECT_EQ(atom.long_field(), 1l);
    EXPECT_EQ(atom.float_field(), 1.0f);
    EXPECT_EQ(atom.string_field(), "string_field1");
    EXPECT_FALSE(atom.boolean_field());
    EXPECT_EQ(atom.state(), TestAtomReported::OFF);

    atom = testAtomEventMetricReport.event_metrics().data(1).atom().test_atom_reported();
    EXPECT_FALSE(atom.has_int_field());
    EXPECT_EQ(atom.long_field(), 2l);
    EXPECT_EQ(atom.float_field(), 2.0f);
    EXPECT_EQ(atom.string_field(), "string_field2");
    EXPECT_TRUE(atom.boolean_field());
    EXPECT_EQ(atom.state(), TestAtomReported::ON);

    atom = testAtomEventMetricReport.event_metrics().data(2).atom().test_atom_reported();
    EXPECT_FALSE(atom.has_int_field());
    EXPECT_EQ(atom.long_field(), 3l);
    EXPECT_EQ(atom.float_field(), 3.0f);
    EXPECT_EQ(atom.string_field(), "string_field3");
    EXPECT_FALSE(atom.boolean_field());
    EXPECT_EQ(atom.state(), TestAtomReported::ON);
}
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif

}  // namespace statsd
}  // namespace os
}  // namespace android
