// Copyright (C) 2017 The Android Open Source Project
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

#include <android/binder_interface_utils.h>
#include <gtest/gtest.h>

#include <vector>

#include "src/StatsLogProcessor.h"
#include "src/stats_log_util.h"
#include "tests/statsd_test_util.h"

using ::ndk::SharedRefBase;

namespace android {
namespace os {
namespace statsd {

#ifdef __ANDROID__

namespace {

const int64_t metricId = 123456;
const int32_t ATOM_TAG = util::SUBSYSTEM_SLEEP_STATE;

StatsdConfig CreateStatsdConfig(const GaugeMetric::SamplingType sampling_type,
                                bool useCondition = true) {
    StatsdConfig config;
    config.add_default_pull_packages("AID_ROOT");  // Fake puller is registered with root.
    auto atomMatcher = CreateSimpleAtomMatcher("TestMatcher", ATOM_TAG);
    *config.add_atom_matcher() = atomMatcher;
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOffAtomMatcher();

    auto screenIsOffPredicate = CreateScreenIsOffPredicate();
    *config.add_predicate() = screenIsOffPredicate;

    auto gaugeMetric = config.add_gauge_metric();
    gaugeMetric->set_id(metricId);
    gaugeMetric->set_what(atomMatcher.id());
    if (useCondition) {
        gaugeMetric->set_condition(screenIsOffPredicate.id());
    }
    gaugeMetric->set_sampling_type(sampling_type);
    *gaugeMetric->mutable_dimensions_in_what() =
            CreateDimensions(ATOM_TAG, {1 /* subsystem name */});
    gaugeMetric->set_bucket(FIVE_MINUTES);
    gaugeMetric->set_max_pull_delay_sec(INT_MAX);
    config.set_hash_strings_in_metric_report(false);
    gaugeMetric->set_split_bucket_for_app_upgrade(true);
    gaugeMetric->set_min_bucket_size_nanos(1000);

    return config;
}

}  // namespaces

TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEvents) {
    auto config = CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE);
    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->isConfigValid());
    processor->mPullerManager->ForceClearPullerCache();

    int startBucketNum = processor->mMetricsManagers.begin()
                                 ->second->mAllMetricProducers[0]
                                 ->getCurrentBucketNum();
    EXPECT_GT(startBucketNum, (int64_t)0);

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + bucketSizeNs, nextPullTimeNs);

    auto screenOffEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 55, android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    // Pulling alarm arrives on time and reset the sequential pulling alarm.
    processor->informPullAlarmFired(nextPullTimeNs + 1);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 2 * bucketSizeNs, nextPullTimeNs);

    auto screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 10,
                                                       android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 100,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    processor->informPullAlarmFired(nextPullTimeNs + 1);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 3 * bucketSizeNs, nextPullTimeNs);

    processor->informPullAlarmFired(nextPullTimeNs + 1);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 4 * bucketSizeNs, nextPullTimeNs);

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 3 * bucketSizeNs + 2,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    processor->informPullAlarmFired(nextPullTimeNs + 3);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 5 * bucketSizeNs, nextPullTimeNs);

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 5 * bucketSizeNs + 1,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    processor->informPullAlarmFired(nextPullTimeNs + 2);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 6 * bucketSizeNs, nextPullTimeNs);

    processor->informPullAlarmFired(nextPullTimeNs + 2);

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 7 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    EXPECT_TRUE(reports.reports(0).metrics(0).has_estimated_data_bytes());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 1);

    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(6, data.bucket_info_size());

    ASSERT_EQ(1, data.bucket_info(0).atom_size());
    ASSERT_EQ(1, data.bucket_info(0).elapsed_timestamp_nanos_size());
    EXPECT_EQ(configAddedTimeNs + 55, data.bucket_info(0).elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, data.bucket_info(0).wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 2 * bucketSizeNs, data.bucket_info(0).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(0).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(0).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(0).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(1).atom_size());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs + 1, data.bucket_info(1).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs + 1, data.bucket_info(1).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(1).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, data.bucket_info(1).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(1).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(1).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(2).atom_size());
    ASSERT_EQ(1, data.bucket_info(2).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs + 1, data.bucket_info(2).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, data.bucket_info(2).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs, data.bucket_info(2).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(2).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(2).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(3).atom_size());
    ASSERT_EQ(1, data.bucket_info(3).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs + 1, data.bucket_info(3).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs, data.bucket_info(3).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 6 * bucketSizeNs, data.bucket_info(3).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(3).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(3).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(4).atom_size());
    ASSERT_EQ(1, data.bucket_info(4).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 7 * bucketSizeNs + 1, data.bucket_info(4).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 7 * bucketSizeNs, data.bucket_info(4).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 8 * bucketSizeNs, data.bucket_info(4).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(4).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(4).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(5).atom_size());
    ASSERT_EQ(1, data.bucket_info(5).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 8 * bucketSizeNs + 2, data.bucket_info(5).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 8 * bucketSizeNs, data.bucket_info(5).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 9 * bucketSizeNs, data.bucket_info(5).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(5).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(5).atom(0).subsystem_sleep_state().time_millis(), 0);
}

TEST(GaugeMetricE2ePulledTest, TestFirstNSamplesPulledNoTrigger) {
    StatsdConfig config = CreateStatsdConfig(GaugeMetric::FIRST_N_SAMPLES);
    auto gaugeMetric = config.mutable_gauge_metric(0);
    gaugeMetric->set_max_num_gauge_atoms_per_bucket(3);
    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->isConfigValid());
    processor->mPullerManager->ForceClearPullerCache();

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;

    auto screenOffEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 55, android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    auto screenOnEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 100, android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 150,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    screenOnEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 200, android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 250,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    screenOnEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 300, android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    // Not logged. max_num_gauge_atoms_per_bucket already hit.
    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 325,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    // Pulling alarm arrives on time and reset the sequential pulling alarm.
    processor->informPullAlarmFired(nextPullTimeNs + 1);

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 10,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 100,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    processor->informPullAlarmFired(nextPullTimeNs + 2);

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + (3 * bucketSizeNs) + 15,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    processor->informPullAlarmFired(nextPullTimeNs + 4);

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + (4 * bucketSizeNs) + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 1);

    auto data = gaugeMetrics.data(1);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(3, data.bucket_info_size());

    ASSERT_EQ(3, data.bucket_info(0).atom_size());
    ASSERT_EQ(3, data.bucket_info(0).elapsed_timestamp_nanos_size());
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + 55), (int64_t)(configAddedTimeNs + 150),
                              (int64_t)(configAddedTimeNs + 250)});

    ASSERT_EQ(2, data.bucket_info(1).atom_size());
    ASSERT_EQ(2, data.bucket_info(1).elapsed_timestamp_nanos_size());
    ValidateGaugeBucketTimes(data.bucket_info(1),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + (2 * bucketSizeNs),
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 1),
                              (int64_t)(configAddedTimeNs + bucketSizeNs + 100)});

    ASSERT_EQ(1, data.bucket_info(2).atom_size());
    ASSERT_EQ(1, data.bucket_info(2).elapsed_timestamp_nanos_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(2), /*startTimeNs=*/configAddedTimeNs + (2 * bucketSizeNs),
            /*endTimeNs=*/configAddedTimeNs + (3 * bucketSizeNs),
            /*eventTimesNs=*/{(int64_t)(configAddedTimeNs + (2 * bucketSizeNs) + 2)});
}

TEST(GaugeMetricE2ePulledTest, TestConditionChangeToTrueSamplePulledEvents) {
    auto config = CreateStatsdConfig(GaugeMetric::CONDITION_CHANGE_TO_TRUE);
    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->isConfigValid());
    processor->mPullerManager->ForceClearPullerCache();

    int startBucketNum = processor->mMetricsManagers.begin()
                                 ->second->mAllMetricProducers[0]
                                 ->getCurrentBucketNum();
    EXPECT_GT(startBucketNum, (int64_t)0);

    auto screenOffEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 55, android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    auto screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 10,
                                                       android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 100,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 3 * bucketSizeNs + 2,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 5 * bucketSizeNs + 1,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());
    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 5 * bucketSizeNs + 3,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());
    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 5 * bucketSizeNs + 10,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 8 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 1);

    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(3, data.bucket_info_size());

    ASSERT_EQ(1, data.bucket_info(0).atom_size());
    ASSERT_EQ(1, data.bucket_info(0).elapsed_timestamp_nanos_size());
    EXPECT_EQ(configAddedTimeNs + 55, data.bucket_info(0).elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, data.bucket_info(0).wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 2 * bucketSizeNs, data.bucket_info(0).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(0).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(0).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(0).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(1).atom_size());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs + 100, data.bucket_info(1).elapsed_timestamp_nanos(0));
    EXPECT_EQ(configAddedTimeNs + 55, data.bucket_info(0).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(1).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, data.bucket_info(1).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(1).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(1).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(2, data.bucket_info(2).atom_size());
    ASSERT_EQ(2, data.bucket_info(2).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 7 * bucketSizeNs + 1, data.bucket_info(2).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 7 * bucketSizeNs + 10, data.bucket_info(2).elapsed_timestamp_nanos(1));
    EXPECT_EQ(baseTimeNs + 7 * bucketSizeNs, data.bucket_info(2).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 8 * bucketSizeNs, data.bucket_info(2).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(2).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(2).atom(0).subsystem_sleep_state().time_millis(), 0);
    EXPECT_TRUE(data.bucket_info(2).atom(1).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(2).atom(1).subsystem_sleep_state().time_millis(), 0);
}

TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEvent_LateAlarm) {
    auto config = CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE);
    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->isConfigValid());
    processor->mPullerManager->ForceClearPullerCache();

    int startBucketNum = processor->mMetricsManagers.begin()
                                 ->second->mAllMetricProducers[0]
                                 ->getCurrentBucketNum();
    EXPECT_GT(startBucketNum, (int64_t)0);

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + bucketSizeNs, nextPullTimeNs);

    auto screenOffEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 55, android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    auto screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 10,
                                                       android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    // Pulling alarm arrives one bucket size late.
    processor->informPullAlarmFired(nextPullTimeNs + bucketSizeNs);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 3 * bucketSizeNs, nextPullTimeNs);

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 3 * bucketSizeNs + 11,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    // Pulling alarm arrives more than one bucket size late.
    processor->informPullAlarmFired(nextPullTimeNs + bucketSizeNs + 12);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 5 * bucketSizeNs, nextPullTimeNs);

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 7 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 1);

    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(3, data.bucket_info_size());

    ASSERT_EQ(1, data.bucket_info(0).atom_size());
    ASSERT_EQ(1, data.bucket_info(0).elapsed_timestamp_nanos_size());
    EXPECT_EQ(configAddedTimeNs + 55, data.bucket_info(0).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 2 * bucketSizeNs, data.bucket_info(0).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(0).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(0).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(0).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(1).atom_size());
    EXPECT_EQ(configAddedTimeNs + 3 * bucketSizeNs + 11,
              data.bucket_info(1).elapsed_timestamp_nanos(0));
    EXPECT_EQ(configAddedTimeNs + 55, data.bucket_info(0).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs, data.bucket_info(1).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 6 * bucketSizeNs, data.bucket_info(1).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(1).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(1).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(2).atom_size());
    ASSERT_EQ(1, data.bucket_info(2).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 6 * bucketSizeNs + 12, data.bucket_info(2).elapsed_timestamp_nanos(0));
    EXPECT_EQ(baseTimeNs + 6 * bucketSizeNs, data.bucket_info(2).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 7 * bucketSizeNs, data.bucket_info(2).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(2).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(2).atom(0).subsystem_sleep_state().time_millis(), 0);
}

TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEventsWithActivation) {
    auto config = CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE, /*useCondition=*/false);

    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    auto batterySaverStartMatcher = CreateBatterySaverModeStartAtomMatcher();
    *config.add_atom_matcher() = batterySaverStartMatcher;
    const int64_t ttlNs = 2 * bucketSizeNs;  // Two buckets.
    auto metric_activation = config.add_metric_activation();
    metric_activation->set_metric_id(metricId);
    metric_activation->set_activation_type(ACTIVATE_IMMEDIATELY);
    auto event_activation = metric_activation->add_event_activation();
    event_activation->set_atom_matcher_id(batterySaverStartMatcher.id());
    event_activation->set_ttl_seconds(ttlNs / 1000000000);

    StatsdStats::getInstance().reset();

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->isConfigValid());
    processor->mPullerManager->ForceClearPullerCache();

    const int startBucketNum = processor->mMetricsManagers.begin()
                                       ->second->mAllMetricProducers[0]
                                       ->getCurrentBucketNum();
    EXPECT_EQ(startBucketNum, 2);
    EXPECT_FALSE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + bucketSizeNs, nextPullTimeNs);

    // Check no pull occurred on metric initialization when it's not active.
    const int64_t metricInitTimeNs = configAddedTimeNs + 1;  // 10 mins + 1 ns.
    processor->onStatsdInitCompleted(metricInitTimeNs);
    StatsdStatsReport_PulledAtomStats pulledAtomStats =
            getPulledAtomStats(util::SUBSYSTEM_SLEEP_STATE);
    EXPECT_EQ(pulledAtomStats.atom_id(), ATOM_TAG);
    EXPECT_EQ(pulledAtomStats.total_pull(), 0);

    // Check no pull occurred on app upgrade when metric is not active.
    const int64_t appUpgradeTimeNs = metricInitTimeNs + 1;  // 10 mins + 2 ns.
    processor->notifyAppUpgrade(appUpgradeTimeNs, "appName", 1000 /* uid */, 2 /* version */);
    pulledAtomStats = getPulledAtomStats(util::SUBSYSTEM_SLEEP_STATE);
    EXPECT_EQ(pulledAtomStats.atom_id(), ATOM_TAG);
    EXPECT_EQ(pulledAtomStats.total_pull(), 0);

    // Check skipped bucket is not added when metric is not active.
    int64_t dumpReportTimeNs = appUpgradeTimeNs + 1;  // 10 mins + 3 ns.
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, dumpReportTimeNs, true /* include_current_partial_bucket */,
                            true /* erase_data */, ADB_DUMP, NO_TIME_CONSTRAINTS, &buffer);
    ConfigMetricsReportList reports;
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics =
            reports.reports(0).metrics(0).gauge_metrics();
    EXPECT_EQ(gaugeMetrics.skipped_size(), 0);

    // Pulling alarm arrives on time and reset the sequential pulling alarm.
    // Event should not be kept.
    processor->informPullAlarmFired(nextPullTimeNs + 1);  // 15 mins + 1 ns.
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 2 * bucketSizeNs, nextPullTimeNs);
    EXPECT_FALSE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // Activate the metric. A pull occurs upon activation. The event is kept. 1 total
    // 15 mins + 2 ms
    const int64_t activationNs = configAddedTimeNs + bucketSizeNs + (2 * 1000 * 1000);  // 2 millis.
    auto batterySaverOnEvent = CreateBatterySaverOnEvent(activationNs);
    processor->OnLogEvent(batterySaverOnEvent.get());  // 15 mins + 2 ms.
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // This event should be kept. 2 total.
    processor->informPullAlarmFired(nextPullTimeNs + 1);  // 20 mins + 1 ns.
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 3 * bucketSizeNs, nextPullTimeNs);

    // This event should be kept. 3 total.
    processor->informPullAlarmFired(nextPullTimeNs + 2);  // 25 mins + 2 ns.
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 4 * bucketSizeNs, nextPullTimeNs);

    // Create random event to deactivate metric.
    // A pull should not occur here. 3 total.
    // 25 mins + 2 ms + 1 ns.
    const int64_t deactivationNs = activationNs + ttlNs + 1;
    auto deactivationEvent = CreateScreenBrightnessChangedEvent(deactivationNs, 50);
    processor->OnLogEvent(deactivationEvent.get());
    EXPECT_FALSE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // Event should not be kept. 3 total.
    // 30 mins + 3 ns.
    processor->informPullAlarmFired(nextPullTimeNs + 3);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 5 * bucketSizeNs, nextPullTimeNs);

    // Event should not be kept. 3 total.
    // 35 mins + 2 ns.
    processor->informPullAlarmFired(nextPullTimeNs + 2);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 6 * bucketSizeNs, nextPullTimeNs);

    buffer.clear();
    // 40 mins + 10 ns.
    processor->onDumpReport(cfgKey, configAddedTimeNs + 6 * bucketSizeNs + 10,
                            false /* include_current_partial_bucket */, true /* erase_data */,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    gaugeMetrics = StatsLogReport::GaugeMetricDataWrapper();
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 0);

    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(3, data.bucket_info_size());

    auto bucketInfo = data.bucket_info(0);
    ASSERT_EQ(1, bucketInfo.atom_size());
    ASSERT_EQ(1, bucketInfo.elapsed_timestamp_nanos_size());
    EXPECT_EQ(activationNs, bucketInfo.elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, bucketInfo.wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, bucketInfo.start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, bucketInfo.end_bucket_elapsed_nanos());
    EXPECT_TRUE(bucketInfo.atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(bucketInfo.atom(0).subsystem_sleep_state().time_millis(), 0);

    bucketInfo = data.bucket_info(1);
    ASSERT_EQ(1, bucketInfo.atom_size());
    ASSERT_EQ(1, bucketInfo.elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs + 1, bucketInfo.elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, bucketInfo.wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, bucketInfo.start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs, bucketInfo.end_bucket_elapsed_nanos());
    EXPECT_TRUE(bucketInfo.atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(bucketInfo.atom(0).subsystem_sleep_state().time_millis(), 0);

    bucketInfo = data.bucket_info(2);
    ASSERT_EQ(1, bucketInfo.atom_size());
    ASSERT_EQ(1, bucketInfo.elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs + 2, bucketInfo.elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, bucketInfo.wall_clock_timestamp_nanos_size());
    EXPECT_EQ(MillisToNano(NanoToMillis(baseTimeNs + 5 * bucketSizeNs)),
              bucketInfo.start_bucket_elapsed_nanos());
    EXPECT_EQ(MillisToNano(NanoToMillis(deactivationNs)), bucketInfo.end_bucket_elapsed_nanos());
    EXPECT_TRUE(bucketInfo.atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(bucketInfo.atom(0).subsystem_sleep_state().time_millis(), 0);

    // Check skipped bucket is not added after deactivation.
    dumpReportTimeNs = configAddedTimeNs + 8 * bucketSizeNs + 10;
    buffer.clear();
    processor->onDumpReport(cfgKey, dumpReportTimeNs, true /* include_current_partial_bucket */,
                            true /* erase_data */, ADB_DUMP, NO_TIME_CONSTRAINTS, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    gaugeMetrics = reports.reports(0).metrics(0).gauge_metrics();
    EXPECT_EQ(gaugeMetrics.skipped_size(), 0);
}

TEST(GaugeMetricE2ePulledTest, TestFirstNSamplesPulledNoTriggerWithActivation) {
    StatsdConfig config = CreateStatsdConfig(GaugeMetric::FIRST_N_SAMPLES);
    auto gaugeMetric = config.mutable_gauge_metric(0);
    gaugeMetric->set_max_num_gauge_atoms_per_bucket(2);
    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    auto batterySaverStartMatcher = CreateBatterySaverModeStartAtomMatcher();
    *config.add_atom_matcher() = batterySaverStartMatcher;
    const int64_t ttlNs = 2 * bucketSizeNs;  // Two buckets.
    auto metric_activation = config.add_metric_activation();
    metric_activation->set_metric_id(metricId);
    metric_activation->set_activation_type(ACTIVATE_IMMEDIATELY);
    auto event_activation = metric_activation->add_event_activation();
    event_activation->set_atom_matcher_id(batterySaverStartMatcher.id());
    event_activation->set_ttl_seconds(ttlNs / NS_PER_SEC);

    StatsdStats::getInstance().reset();

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    processor->mPullerManager->ForceClearPullerCache();

    EXPECT_FALSE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;

    // Condition true but Active false
    auto screenOffEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 55, android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    auto screenOnEvent =
            CreateScreenStateChangedEvent(configAddedTimeNs + 100, android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    // Pulling alarm arrives on time and reset the sequential pulling alarm.
    // Event should not be kept.
    processor->informPullAlarmFired(nextPullTimeNs + 1);  // 15 mins + 1 ns.
    EXPECT_FALSE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // Activate the metric. A pull occurs upon activation. The event is not kept. 0 total
    // 15 mins + 1000 ns.
    const int64_t activationNs = configAddedTimeNs + bucketSizeNs + 1000;
    auto batterySaverOnEvent = CreateBatterySaverOnEvent(activationNs);
    processor->OnLogEvent(batterySaverOnEvent.get());  // 15 mins + 1000 ns.
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());

    // A pull occurs upon condition change. The event is kept. 1 total. 1 in bucket
    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 150,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 200,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    // A pull occurs upon condition change. The event is kept. 1 total. 2 in bucket
    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 250,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 300,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    // A pull occurs upon condition change. The event is not kept due to
    // max_num_gauge_atoms_per_bucket. 1 total. 2 total in bucket
    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 325,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 375,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());
    // Condition false but Active true

    // This event should not be kept. 1 total.
    processor->informPullAlarmFired(nextPullTimeNs + 1);  // 20 mins + 1 ns.

    // This event should not be kept. 1 total.
    processor->informPullAlarmFired(nextPullTimeNs + 2);  // 25 mins + 2 ns.

    // A pull occurs upon condition change. The event is kept. 2 total. 1 in bucket
    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 3 * bucketSizeNs + 50,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());
    // Condition true but Active true

    // Create random event to deactivate metric.
    // A pull should not occur here. 2 total. 1 in bucket.
    // 25 mins + 1000 ns + 1 ns.
    const int64_t deactivationNs = activationNs + ttlNs + 1;
    auto deactivationEvent = CreateScreenBrightnessChangedEvent(deactivationNs, 50);
    processor->OnLogEvent(deactivationEvent.get());
    EXPECT_FALSE(processor->mMetricsManagers.begin()->second->mAllMetricProducers[0]->isActive());
    // Condition true but Active false

    screenOnEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 3 * bucketSizeNs + 50,
                                                  android::view::DISPLAY_STATE_ON);
    processor->OnLogEvent(screenOnEvent.get());

    screenOffEvent = CreateScreenStateChangedEvent(configAddedTimeNs + 3 * bucketSizeNs + 100,
                                                   android::view::DISPLAY_STATE_OFF);
    processor->OnLogEvent(screenOffEvent.get());

    vector<uint8_t> buffer;
    // 30 mins + 10 ns.
    processor->onDumpReport(cfgKey, configAddedTimeNs + 4 * bucketSizeNs + 10,
                            false /* include_current_partial_bucket */, true /* erase_data */,
                            ADB_DUMP, FAST, &buffer);
    ConfigMetricsReportList reports;
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics = StatsLogReport::GaugeMetricDataWrapper();
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 0);

    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(2, data.bucket_info_size());

    ASSERT_EQ(2, data.bucket_info(0).atom_size());
    ASSERT_EQ(2, data.bucket_info(0).elapsed_timestamp_nanos_size());
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + (2 * bucketSizeNs),
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 150),
                              (int64_t)(configAddedTimeNs + bucketSizeNs + 250)});

    ASSERT_EQ(1, data.bucket_info(1).atom_size());
    ASSERT_EQ(1, data.bucket_info(1).elapsed_timestamp_nanos_size());
    ValidateGaugeBucketTimes(data.bucket_info(1),
                             /*startTimeNs=*/
                             MillisToNano(NanoToMillis(configAddedTimeNs + (3 * bucketSizeNs))),
                             /*endTimeNs=*/MillisToNano(NanoToMillis(deactivationNs)),
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + (3 * bucketSizeNs) + 50)});
}

TEST(GaugeMetricE2ePulledTest, TestRandomSamplePulledEventsNoCondition) {
    auto config = CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE, /*useCondition=*/false);

    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs =
        TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor = CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                             SharedRefBase::make<FakeSubsystemSleepCallback>(),
                                             ATOM_TAG);
    ASSERT_EQ(processor->mMetricsManagers.size(), 1u);
    EXPECT_TRUE(processor->mMetricsManagers.begin()->second->isConfigValid());
    processor->mPullerManager->ForceClearPullerCache();

    int startBucketNum = processor->mMetricsManagers.begin()->second->
            mAllMetricProducers[0]->getCurrentBucketNum();
    EXPECT_GT(startBucketNum, (int64_t)0);

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + bucketSizeNs, nextPullTimeNs);

    // Pulling alarm arrives on time and reset the sequential pulling alarm.
    processor->informPullAlarmFired(nextPullTimeNs + 1);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 2 * bucketSizeNs, nextPullTimeNs);

    processor->informPullAlarmFired(nextPullTimeNs + 4);
    EXPECT_EQ(baseTimeNs + startBucketNum * bucketSizeNs + 3 * bucketSizeNs,
              nextPullTimeNs);

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 7 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(
            reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 0);

    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_FALSE(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str().empty());
    ASSERT_EQ(3, data.bucket_info_size());

    ASSERT_EQ(1, data.bucket_info(0).atom_size());
    ASSERT_EQ(1, data.bucket_info(0).elapsed_timestamp_nanos_size());
    EXPECT_EQ(configAddedTimeNs, data.bucket_info(0).elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, data.bucket_info(0).wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 2 * bucketSizeNs, data.bucket_info(0).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(0).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(0).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(0).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(1).atom_size());
    ASSERT_EQ(1, data.bucket_info(1).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs + 1, data.bucket_info(1).elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, data.bucket_info(1).wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 3 * bucketSizeNs, data.bucket_info(1).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, data.bucket_info(1).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(1).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(1).atom(0).subsystem_sleep_state().time_millis(), 0);

    ASSERT_EQ(1, data.bucket_info(2).atom_size());
    ASSERT_EQ(1, data.bucket_info(2).elapsed_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs + 4, data.bucket_info(2).elapsed_timestamp_nanos(0));
    ASSERT_EQ(0, data.bucket_info(2).wall_clock_timestamp_nanos_size());
    EXPECT_EQ(baseTimeNs + 4 * bucketSizeNs, data.bucket_info(2).start_bucket_elapsed_nanos());
    EXPECT_EQ(baseTimeNs + 5 * bucketSizeNs, data.bucket_info(2).end_bucket_elapsed_nanos());
    EXPECT_TRUE(data.bucket_info(2).atom(0).subsystem_sleep_state().subsystem_name().empty());
    EXPECT_GT(data.bucket_info(2).atom(0).subsystem_sleep_state().time_millis(), 0);
}

TEST(GaugeMetricE2ePulledTest, TestGaugeMetricPullProbabilityWithTriggerEvent) {
    // Initiating StatsdStats at the start of this test, so it doesn't call rand() during the test.
    StatsdStats::getInstance();
    // Set srand seed to make rand deterministic for testing.
    srand(0);

    auto config = CreateStatsdConfig(GaugeMetric::FIRST_N_SAMPLES, /*useCondition=*/false);
    auto gaugeMetric = config.mutable_gauge_metric(0);
    gaugeMetric->set_pull_probability(50);
    auto triggerEventMatcher = CreateScreenTurnedOnAtomMatcher();
    gaugeMetric->set_trigger_event(triggerEventMatcher.id());
    gaugeMetric->set_max_num_gauge_atoms_per_bucket(200);
    gaugeMetric->set_bucket(ONE_HOUR);

    int64_t configAddedTimeNs = 60 * NS_PER_SEC;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(configAddedTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);

    std::vector<std::unique_ptr<LogEvent>> events;
    // First bucket events.
    for (int i = 0; i < 30; i++) {
        events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + (i * 10 * NS_PER_SEC),
                                                       android::view::DISPLAY_STATE_ON));
    }
    // Second bucket events.
    for (int i = 0; i < 30; i++) {
        events.push_back(CreateScreenStateChangedEvent(
                configAddedTimeNs + bucketSizeNs + (i * 10 * NS_PER_SEC),
                android::view::DISPLAY_STATE_ON));
    }

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 7 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);

    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_EQ((int)gaugeMetrics.data_size(), 2);  // 2 sets of data for each pull.

    // Data 1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_EQ("subsystem_name_1",
              data.dimensions_in_what().value_tuple().dimensions_value(0).value_str());
    ASSERT_EQ(2, data.bucket_info_size());

    // Data 1, Bucket 1
    ASSERT_EQ(13, data.bucket_info(0).atom_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(0), configAddedTimeNs, configAddedTimeNs + bucketSizeNs,
            {(int64_t)60 * NS_PER_SEC, (int64_t)80 * NS_PER_SEC, (int64_t)90 * NS_PER_SEC,
             (int64_t)130 * NS_PER_SEC, (int64_t)150 * NS_PER_SEC, (int64_t)170 * NS_PER_SEC,
             (int64_t)190 * NS_PER_SEC, (int64_t)200 * NS_PER_SEC, (int64_t)240 * NS_PER_SEC,
             (int64_t)250 * NS_PER_SEC, (int64_t)300 * NS_PER_SEC, (int64_t)330 * NS_PER_SEC,
             (int64_t)340 * NS_PER_SEC});

    // Data 1, Bucket 2
    ASSERT_EQ(18, data.bucket_info(1).atom_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(1), configAddedTimeNs + bucketSizeNs,
            configAddedTimeNs + 2 * bucketSizeNs,
            {(int64_t)3660 * NS_PER_SEC, (int64_t)3680 * NS_PER_SEC, (int64_t)3700 * NS_PER_SEC,
             (int64_t)3710 * NS_PER_SEC, (int64_t)3720 * NS_PER_SEC, (int64_t)3740 * NS_PER_SEC,
             (int64_t)3780 * NS_PER_SEC, (int64_t)3790 * NS_PER_SEC, (int64_t)3820 * NS_PER_SEC,
             (int64_t)3850 * NS_PER_SEC, (int64_t)3860 * NS_PER_SEC, (int64_t)3870 * NS_PER_SEC,
             (int64_t)3880 * NS_PER_SEC, (int64_t)3900 * NS_PER_SEC, (int64_t)3910 * NS_PER_SEC,
             (int64_t)3920 * NS_PER_SEC, (int64_t)3930 * NS_PER_SEC, (int64_t)3940 * NS_PER_SEC});

    // Data 2
    data = gaugeMetrics.data(1);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_EQ("subsystem_name_2",
              data.dimensions_in_what().value_tuple().dimensions_value(0).value_str());
    ASSERT_EQ(2, data.bucket_info_size());

    // Data 2, Bucket 1
    ASSERT_EQ(13, data.bucket_info(0).atom_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(0), configAddedTimeNs, configAddedTimeNs + bucketSizeNs,
            {(int64_t)60 * NS_PER_SEC, (int64_t)80 * NS_PER_SEC, (int64_t)90 * NS_PER_SEC,
             (int64_t)130 * NS_PER_SEC, (int64_t)150 * NS_PER_SEC, (int64_t)170 * NS_PER_SEC,
             (int64_t)190 * NS_PER_SEC, (int64_t)200 * NS_PER_SEC, (int64_t)240 * NS_PER_SEC,
             (int64_t)250 * NS_PER_SEC, (int64_t)300 * NS_PER_SEC, (int64_t)330 * NS_PER_SEC,
             (int64_t)340 * NS_PER_SEC});

    // Data 2, Bucket 2
    ASSERT_EQ(18, data.bucket_info(1).atom_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(1), configAddedTimeNs + bucketSizeNs,
            configAddedTimeNs + 2 * bucketSizeNs,
            {(int64_t)3660 * NS_PER_SEC, (int64_t)3680 * NS_PER_SEC, (int64_t)3700 * NS_PER_SEC,
             (int64_t)3710 * NS_PER_SEC, (int64_t)3720 * NS_PER_SEC, (int64_t)3740 * NS_PER_SEC,
             (int64_t)3780 * NS_PER_SEC, (int64_t)3790 * NS_PER_SEC, (int64_t)3820 * NS_PER_SEC,
             (int64_t)3850 * NS_PER_SEC, (int64_t)3860 * NS_PER_SEC, (int64_t)3870 * NS_PER_SEC,
             (int64_t)3880 * NS_PER_SEC, (int64_t)3900 * NS_PER_SEC, (int64_t)3910 * NS_PER_SEC,
             (int64_t)3920 * NS_PER_SEC, (int64_t)3930 * NS_PER_SEC, (int64_t)3940 * NS_PER_SEC});
}

TEST(GaugeMetricE2ePulledTest, TestGaugeMetricPullProbabilityWithBucketBoundaryAlarm) {
    // Initiating StatsdStats at the start of this test, so it doesn't call rand() during the test.
    StatsdStats::getInstance();
    // Set srand seed to make rand deterministic for testing.
    srand(0);

    auto config = CreateStatsdConfig(GaugeMetric::FIRST_N_SAMPLES, /*useCondition=*/false);
    auto gaugeMetric = config.mutable_gauge_metric(0);
    gaugeMetric->set_pull_probability(50);
    gaugeMetric->set_max_num_gauge_atoms_per_bucket(200);

    int64_t baseTimeNs = 5 * 60 * NS_PER_SEC;
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(configAddedTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);

    // Pulling alarm arrives on time and resets the sequential pulling alarm.
    for (int i = 1; i < 31; i++) {
        processor->informPullAlarmFired(configAddedTimeNs + i * bucketSizeNs);
    }

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 32 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_EQ((int)gaugeMetrics.data_size(), 2);

    // Data 1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_EQ("subsystem_name_1",
              data.dimensions_in_what().value_tuple().dimensions_value(0).value_str());
    ASSERT_EQ(14, data.bucket_info_size());

    EXPECT_EQ(1, data.bucket_info(0).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(0), configAddedTimeNs,
                             configAddedTimeNs + bucketSizeNs, {configAddedTimeNs});

    EXPECT_EQ(1, data.bucket_info(1).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(1), configAddedTimeNs + 2 * bucketSizeNs,
                             configAddedTimeNs + 3 * bucketSizeNs,
                             {configAddedTimeNs + 2 * bucketSizeNs});  // 1200000000000ns

    EXPECT_EQ(1, data.bucket_info(2).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(2), configAddedTimeNs + 3 * bucketSizeNs,
                             configAddedTimeNs + 4 * bucketSizeNs,
                             {(int64_t)configAddedTimeNs + 3 * bucketSizeNs});  // 1500000000000ns

    EXPECT_EQ(1, data.bucket_info(3).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(3), configAddedTimeNs + 7 * bucketSizeNs,
                             configAddedTimeNs + 8 * bucketSizeNs,
                             {configAddedTimeNs + 7 * bucketSizeNs});  // 2700000000000ns

    EXPECT_EQ(1, data.bucket_info(4).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(4), configAddedTimeNs + 9 * bucketSizeNs,
                             configAddedTimeNs + 10 * bucketSizeNs,
                             {configAddedTimeNs + 9 * bucketSizeNs});  // 3300000000000ns

    EXPECT_EQ(1, data.bucket_info(5).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(5), configAddedTimeNs + 11 * bucketSizeNs,
                             configAddedTimeNs + 12 * bucketSizeNs,
                             {configAddedTimeNs + 11 * bucketSizeNs});  // 3900000000000ns

    EXPECT_EQ(1, data.bucket_info(6).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(6), configAddedTimeNs + 13 * bucketSizeNs,
                             configAddedTimeNs + 14 * bucketSizeNs,
                             {configAddedTimeNs + 13 * bucketSizeNs});  // 4500000000000ns

    EXPECT_EQ(1, data.bucket_info(7).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(7), configAddedTimeNs + 14 * bucketSizeNs,
                             configAddedTimeNs + 15 * bucketSizeNs,
                             {configAddedTimeNs + 14 * bucketSizeNs});  // 4800000000000ns

    EXPECT_EQ(1, data.bucket_info(8).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(8), configAddedTimeNs + 18 * bucketSizeNs,
                             configAddedTimeNs + 19 * bucketSizeNs,
                             {configAddedTimeNs + 18 * bucketSizeNs});  // 6000000000000ns

    EXPECT_EQ(1, data.bucket_info(9).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(9), configAddedTimeNs + 19 * bucketSizeNs,
                             configAddedTimeNs + 20 * bucketSizeNs,
                             {configAddedTimeNs + 19 * bucketSizeNs});  // 6300000000000ns

    EXPECT_EQ(1, data.bucket_info(10).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(10), configAddedTimeNs + 24 * bucketSizeNs,
                             configAddedTimeNs + 25 * bucketSizeNs,
                             {configAddedTimeNs + 24 * bucketSizeNs});  // 7800000000000ns

    EXPECT_EQ(1, data.bucket_info(11).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(11), configAddedTimeNs + 27 * bucketSizeNs,
                             configAddedTimeNs + 28 * bucketSizeNs,
                             {configAddedTimeNs + 27 * bucketSizeNs});  // 8700000000000ns

    EXPECT_EQ(1, data.bucket_info(12).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(12), configAddedTimeNs + 28 * bucketSizeNs,
                             configAddedTimeNs + 29 * bucketSizeNs,
                             {configAddedTimeNs + 28 * bucketSizeNs});  // 9000000000000ns

    EXPECT_EQ(1, data.bucket_info(13).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(13), configAddedTimeNs + 30 * bucketSizeNs,
                             configAddedTimeNs + 31 * bucketSizeNs,
                             {configAddedTimeNs + 30 * bucketSizeNs});  // 9600000000000ns

    // Data 2
    data = gaugeMetrics.data(1);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_EQ("subsystem_name_2",
              data.dimensions_in_what().value_tuple().dimensions_value(0).value_str());
    ASSERT_EQ(14, data.bucket_info_size());

    EXPECT_EQ(1, data.bucket_info(0).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(0), configAddedTimeNs,
                             configAddedTimeNs + bucketSizeNs, {configAddedTimeNs});

    EXPECT_EQ(1, data.bucket_info(1).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(1), configAddedTimeNs + 2 * bucketSizeNs,
                             configAddedTimeNs + 3 * bucketSizeNs,
                             {configAddedTimeNs + 2 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(2).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(2), configAddedTimeNs + 3 * bucketSizeNs,
                             configAddedTimeNs + 4 * bucketSizeNs,
                             {(int64_t)configAddedTimeNs + 3 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(3).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(3), configAddedTimeNs + 7 * bucketSizeNs,
                             configAddedTimeNs + 8 * bucketSizeNs,
                             {configAddedTimeNs + 7 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(4).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(4), configAddedTimeNs + 9 * bucketSizeNs,
                             configAddedTimeNs + 10 * bucketSizeNs,
                             {configAddedTimeNs + 9 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(5).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(5), configAddedTimeNs + 11 * bucketSizeNs,
                             configAddedTimeNs + 12 * bucketSizeNs,
                             {configAddedTimeNs + 11 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(6).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(6), configAddedTimeNs + 13 * bucketSizeNs,
                             configAddedTimeNs + 14 * bucketSizeNs,
                             {configAddedTimeNs + 13 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(7).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(7), configAddedTimeNs + 14 * bucketSizeNs,
                             configAddedTimeNs + 15 * bucketSizeNs,
                             {configAddedTimeNs + 14 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(8).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(8), configAddedTimeNs + 18 * bucketSizeNs,
                             configAddedTimeNs + 19 * bucketSizeNs,
                             {configAddedTimeNs + 18 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(9).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(9), configAddedTimeNs + 19 * bucketSizeNs,
                             configAddedTimeNs + 20 * bucketSizeNs,
                             {configAddedTimeNs + 19 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(10).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(10), configAddedTimeNs + 24 * bucketSizeNs,
                             configAddedTimeNs + 25 * bucketSizeNs,
                             {configAddedTimeNs + 24 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(11).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(11), configAddedTimeNs + 27 * bucketSizeNs,
                             configAddedTimeNs + 28 * bucketSizeNs,
                             {configAddedTimeNs + 27 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(12).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(12), configAddedTimeNs + 28 * bucketSizeNs,
                             configAddedTimeNs + 29 * bucketSizeNs,
                             {configAddedTimeNs + 28 * bucketSizeNs});

    EXPECT_EQ(1, data.bucket_info(13).atom_size());
    ValidateGaugeBucketTimes(data.bucket_info(13), configAddedTimeNs + 30 * bucketSizeNs,
                             configAddedTimeNs + 31 * bucketSizeNs,
                             {configAddedTimeNs + 30 * bucketSizeNs});
}

TEST(GaugeMetricE2ePulledTest, TestGaugeMetricPullProbabilityWithCondition) {
    // Initiating StatsdStats at the start of this test, so it doesn't call rand() during the test.
    StatsdStats::getInstance();
    // Set srand seed to make rand deterministic for testing.
    srand(0);

    auto config = CreateStatsdConfig(GaugeMetric::CONDITION_CHANGE_TO_TRUE, /*useCondition=*/true);
    auto gaugeMetric = config.mutable_gauge_metric(0);
    gaugeMetric->set_pull_probability(50);
    gaugeMetric->set_max_num_gauge_atoms_per_bucket(200);
    gaugeMetric->set_bucket(ONE_HOUR);

    int64_t configAddedTimeNs = 60 * NS_PER_SEC;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(configAddedTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);

    std::vector<std::unique_ptr<LogEvent>> events;
    // First bucket events.
    for (int i = 0; i < 30; i++) {
        events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + (i * 10 * NS_PER_SEC),
                                                       android::view::DISPLAY_STATE_OFF));
        events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + (i * 11 * NS_PER_SEC),
                                                       android::view::DISPLAY_STATE_ON));
    }

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 2 * bucketSizeNs, false, true, ADB_DUMP,
                            FAST, &buffer);

    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_EQ((int)gaugeMetrics.data_size(), 2);  // 2 sets of data for each pull.

    // Data 1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_EQ("subsystem_name_1",
              data.dimensions_in_what().value_tuple().dimensions_value(0).value_str());
    ASSERT_EQ(1, data.bucket_info_size());

    // Data 1, Bucket 1
    ASSERT_EQ(13, data.bucket_info(0).atom_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(0), configAddedTimeNs, configAddedTimeNs + bucketSizeNs,
            {(int64_t)60 * NS_PER_SEC, (int64_t)80 * NS_PER_SEC, (int64_t)90 * NS_PER_SEC,
             (int64_t)130 * NS_PER_SEC, (int64_t)150 * NS_PER_SEC, (int64_t)170 * NS_PER_SEC,
             (int64_t)190 * NS_PER_SEC, (int64_t)200 * NS_PER_SEC, (int64_t)240 * NS_PER_SEC,
             (int64_t)250 * NS_PER_SEC, (int64_t)300 * NS_PER_SEC, (int64_t)330 * NS_PER_SEC,
             (int64_t)340 * NS_PER_SEC});

    // Data 2
    data = gaugeMetrics.data(1);
    EXPECT_EQ(ATOM_TAG, data.dimensions_in_what().field());
    ASSERT_EQ(1, data.dimensions_in_what().value_tuple().dimensions_value_size());
    EXPECT_EQ(1 /* subsystem name field */,
              data.dimensions_in_what().value_tuple().dimensions_value(0).field());
    EXPECT_EQ("subsystem_name_2",
              data.dimensions_in_what().value_tuple().dimensions_value(0).value_str());
    ASSERT_EQ(1, data.bucket_info_size());

    // Data 2, Bucket 1
    ASSERT_EQ(13, data.bucket_info(0).atom_size());
    ValidateGaugeBucketTimes(
            data.bucket_info(0), configAddedTimeNs, configAddedTimeNs + bucketSizeNs,
            {(int64_t)60 * NS_PER_SEC, (int64_t)80 * NS_PER_SEC, (int64_t)90 * NS_PER_SEC,
             (int64_t)130 * NS_PER_SEC, (int64_t)150 * NS_PER_SEC, (int64_t)170 * NS_PER_SEC,
             (int64_t)190 * NS_PER_SEC, (int64_t)200 * NS_PER_SEC, (int64_t)240 * NS_PER_SEC,
             (int64_t)250 * NS_PER_SEC, (int64_t)300 * NS_PER_SEC, (int64_t)330 * NS_PER_SEC,
             (int64_t)340 * NS_PER_SEC});
}

TEST(GaugeMetricE2ePulledTest, TestSliceByStates) {
    StatsdConfig config =
            CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE, /*useCondition=*/false);
    auto gaugeMetric = config.mutable_gauge_metric(0);

    auto state = CreateScreenState();
    *config.add_state() = state;
    gaugeMetric->add_slice_by_state(state.id());

    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    processor->mPullerManager->ForceClearPullerCache();

    // When creating the config, the gauge metric producer should register the alarm at the
    // end of the current bucket.
    ASSERT_EQ((size_t)1, processor->mPullerManager->mReceivers.size());
    EXPECT_EQ(bucketSizeNs,
              processor->mPullerManager->mReceivers.begin()->second.front().intervalNs);
    int64_t& nextPullTimeNs =
            processor->mPullerManager->mReceivers.begin()->second.front().nextPullTimeNs;

    std::vector<std::unique_ptr<LogEvent>> events;
    // First Bucket
    events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + 55,
                                                   android::view::DISPLAY_STATE_OFF));
    events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + 100,
                                                   android::view::DISPLAY_STATE_ON));
    events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + 150,
                                                   android::view::DISPLAY_STATE_OFF));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    // Pulling alarm arrives on time and reset the sequential pulling alarm.
    processor->informPullAlarmFired(nextPullTimeNs + 1);

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + (2 * bucketSizeNs) + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);
    ASSERT_EQ(reports.reports(0).metrics_size(), 1);
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    EXPECT_EQ((int)gaugeMetrics.data_size(), 4);

    // Data 0, StateTracker::kStateUnknown, subsystem_name_1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(data.dimensions_in_what().field(), ATOM_TAG);
    ASSERT_EQ(data.dimensions_in_what().value_tuple().dimensions_value_size(), 1);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).field(),
              1 /* subsystem name field */);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 1);
    ASSERT_EQ(data.slice_by_state_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).atom_id(), SCREEN_STATE_ATOM_ID);
    EXPECT_EQ(data.slice_by_state(0).value(), -1 /* StateTracker::kStateUnknown */);
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs)});

    // Data 1, DISPLAY_STATE_OFF, subsystem_name_1
    data = gaugeMetrics.data(1);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).value(), android::view::DisplayStateEnum::DISPLAY_STATE_OFF);
    // Second Bucket
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 1)});

    // Data 2, StateTracker::kStateUnknown, subsystem_name_2
    data = gaugeMetrics.data(2);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_2");
    EXPECT_EQ(data.slice_by_state(0).value(), -1 /* StateTracker::kStateUnknown */);
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs)});

    // Data 3, DISPLAY_STATE_OFF, subsystem_name_2
    data = gaugeMetrics.data(3);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_2");
    EXPECT_EQ(data.slice_by_state(0).value(), android::view::DisplayStateEnum::DISPLAY_STATE_OFF);
    // Second Bucket
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 1)});
}

TEST(GaugeMetricE2ePulledTest, TestSliceByStatesWithTriggerAndCondition) {
    StatsdConfig config = CreateStatsdConfig(GaugeMetric::FIRST_N_SAMPLES, /*useCondition=*/false);
    auto gaugeMetric = config.mutable_gauge_metric(0);

    *config.add_atom_matcher() = CreateBatteryStateNoneMatcher();
    *config.add_atom_matcher() = CreateBatteryStateUsbMatcher();
    auto deviceUnpluggedPredicate = CreateDeviceUnpluggedPredicate();
    *config.add_predicate() = deviceUnpluggedPredicate;
    gaugeMetric->set_condition(deviceUnpluggedPredicate.id());

    auto triggerEventMatcher = CreateBatterySaverModeStartAtomMatcher();
    *config.add_atom_matcher() = triggerEventMatcher;
    gaugeMetric->set_trigger_event(triggerEventMatcher.id());

    auto state = CreateScreenState();
    *config.add_state() = state;
    gaugeMetric->add_slice_by_state(state.id());

    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    processor->mPullerManager->ForceClearPullerCache();

    std::vector<std::unique_ptr<LogEvent>> events;
    // First Bucket
    // Condition True
    events.push_back(CreateBatteryStateChangedEvent(configAddedTimeNs + 10,
                                                    BatteryPluggedStateEnum::BATTERY_PLUGGED_NONE));
    // State Changed - No Pull
    events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + 50,
                                                   android::view::DISPLAY_STATE_OFF));
    // Trigger Event - Pull
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 100));
    // Condition False
    events.push_back(CreateBatteryStateChangedEvent(configAddedTimeNs + 150,
                                                    BatteryPluggedStateEnum::BATTERY_PLUGGED_USB));
    // State Changed - No Pull
    events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + 200,
                                                   android::view::DISPLAY_STATE_ON));
    // Trigger Event - No Pull
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 250));
    // Condition True
    events.push_back(CreateBatteryStateChangedEvent(configAddedTimeNs + 300,
                                                    BatteryPluggedStateEnum::BATTERY_PLUGGED_NONE));

    // Second Bucket
    // Trigger Event - Pull
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + bucketSizeNs + 50));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + (2 * bucketSizeNs) + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);
    ASSERT_EQ(reports.reports(0).metrics_size(), 1);
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    EXPECT_EQ((int)gaugeMetrics.data_size(), 4);
    // Data Size is 4: 2 states (DISPLAY_STATE_ON, DISPLAY_STATE_OFF) and 2 dim_in_what
    // (subsystem_name_1, subsystem_name_2). The latter 2 entries are the same as the first 2 but
    // for subsystem_name_2

    // Data 0, DISPLAY_STATE_OFF, subsystem_name_1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(data.dimensions_in_what().field(), ATOM_TAG);
    ASSERT_EQ(data.dimensions_in_what().value_tuple().dimensions_value_size(), 1);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).field(),
              1 /* subsystem name field */);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 1);
    ASSERT_EQ(data.slice_by_state_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).atom_id(), SCREEN_STATE_ATOM_ID);
    EXPECT_EQ(data.slice_by_state(0).value(), android::view::DisplayStateEnum::DISPLAY_STATE_OFF);
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + 100)});

    // Data 1, DISPLAY_STATE_ON, subsystem_name_1
    data = gaugeMetrics.data(1);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).value(), android::view::DisplayStateEnum::DISPLAY_STATE_ON);
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 50)});
}

TEST(GaugeMetricE2ePulledTest, TestSliceByStatesWithMapAndTrigger) {
    StatsdConfig config = CreateStatsdConfig(GaugeMetric::FIRST_N_SAMPLES, /*useCondition=*/false);
    auto gaugeMetric = config.mutable_gauge_metric(0);

    auto triggerEventMatcher = CreateBatterySaverModeStartAtomMatcher();
    *config.add_atom_matcher() = triggerEventMatcher;
    gaugeMetric->set_trigger_event(triggerEventMatcher.id());

    int64_t screenOnId = 4444;
    int64_t screenOffId = 9876;
    auto state = CreateScreenStateWithOnOffMap(screenOnId, screenOffId);
    *config.add_state() = state;
    gaugeMetric->add_slice_by_state(state.id());

    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);
    processor->mPullerManager->ForceClearPullerCache();

    std::vector<std::unique_ptr<LogEvent>> events;
    // First Bucket
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 50));

    events.push_back(CreateScreenStateChangedEvent(configAddedTimeNs + 100,
                                                   android::view::DISPLAY_STATE_ON));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 110));

    events.push_back(CreateScreenStateChangedEvent(
            configAddedTimeNs + 150, android::view::DisplayStateEnum::DISPLAY_STATE_DOZE));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 160));

    events.push_back(CreateScreenStateChangedEvent(
            configAddedTimeNs + 200, android::view::DisplayStateEnum::DISPLAY_STATE_OFF));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 210));

    // Second Bucket
    events.push_back(
            CreateScreenStateChangedEvent(configAddedTimeNs + bucketSizeNs + 10,
                                          android::view::DisplayStateEnum::DISPLAY_STATE_VR));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + bucketSizeNs + 50));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + (2 * bucketSizeNs) + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);
    ASSERT_EQ(reports.reports(0).metrics_size(), 1);
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    EXPECT_EQ((int)gaugeMetrics.data_size(), 6);
    // Data Size is 6: 3 states (kStateUnknown, screenOn, screenOff) and 2 dim_in_what
    // (subsystem_name_1, subsystem_name_2). The latter 3 are same as the first 3 but for
    // subsystem_name_2

    // Data 0, StateTracker::kStateUnknown, subsystem_name_1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).value(), -1 /* StateTracker::kStateUnknown */);
    // First Bucket
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + 50)});

    // Data 1, State Group Screen On, subsystem_name_1
    data = gaugeMetrics.data(1);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 2);
    EXPECT_EQ(data.slice_by_state(0).group_id(), screenOnId);
    // First Bucket
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + 110)});
    // Second Bucket
    ValidateGaugeBucketTimes(data.bucket_info(1),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 50)});

    // Data 2, State Group Screen Off, subsystem_name_1
    data = gaugeMetrics.data(2);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_str(),
              "subsystem_name_1");
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).group_id(), screenOffId);
    // First Bucket
    ValidateGaugeBucketTimes(
            data.bucket_info(0),
            /*startTimeNs=*/configAddedTimeNs,
            /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
            /*eventTimesNs=*/
            {(int64_t)(configAddedTimeNs + 160), (int64_t)(configAddedTimeNs + 210)});
}

TEST(GaugeMetricE2ePulledTest, TestSliceByStatesWithPrimaryFieldsAndTrigger) {
    StatsdConfig config;
    config.add_default_pull_packages("AID_ROOT");  // Fake puller is registered with root.
    auto cpuTimePerUidMatcher =
            CreateSimpleAtomMatcher("CpuTimePerUidMatcher", util::CPU_TIME_PER_UID);
    *config.add_atom_matcher() = cpuTimePerUidMatcher;

    auto gaugeMetric = config.add_gauge_metric();
    gaugeMetric->set_id(metricId);
    gaugeMetric->set_what(cpuTimePerUidMatcher.id());
    gaugeMetric->set_sampling_type(GaugeMetric::FIRST_N_SAMPLES);
    *gaugeMetric->mutable_dimensions_in_what() =
            CreateDimensions(util::CPU_TIME_PER_UID, {1 /* uid */});
    gaugeMetric->set_bucket(FIVE_MINUTES);
    gaugeMetric->set_max_pull_delay_sec(INT_MAX);
    config.set_hash_strings_in_metric_report(false);
    gaugeMetric->set_split_bucket_for_app_upgrade(true);
    gaugeMetric->set_min_bucket_size_nanos(1000);

    auto triggerEventMatcher = CreateBatterySaverModeStartAtomMatcher();
    *config.add_atom_matcher() = triggerEventMatcher;
    gaugeMetric->set_trigger_event(triggerEventMatcher.id());

    auto state = CreateUidProcessState();
    *config.add_state() = state;
    gaugeMetric->add_slice_by_state(state.id());

    MetricStateLink* stateLink = gaugeMetric->add_state_link();
    stateLink->set_state_atom_id(UID_PROCESS_STATE_ATOM_ID);
    auto fieldsInWhat = stateLink->mutable_fields_in_what();
    *fieldsInWhat = CreateDimensions(util::CPU_TIME_PER_UID, {1 /* uid */});
    auto fieldsInState = stateLink->mutable_fields_in_state();
    *fieldsInState = CreateDimensions(UID_PROCESS_STATE_ATOM_ID, {1 /* uid */});

    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor = CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                             SharedRefBase::make<FakeCpuTimeCallback>(),
                                             util::CPU_TIME_PER_UID);
    processor->mPullerManager->ForceClearPullerCache();

    std::vector<std::unique_ptr<LogEvent>> events;
    // First Bucket
    events.push_back(CreateUidProcessStateChangedEvent(
            configAddedTimeNs + 55, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 80));

    events.push_back(CreateUidProcessStateChangedEvent(
            configAddedTimeNs + 100, 2 /*uid*/, android::app::ProcessStateEnum::PROCESS_STATE_TOP));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 150));

    events.push_back(CreateUidProcessStateChangedEvent(
            configAddedTimeNs + 200, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_BACKGROUND));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + 250));

    // Second Bucket
    events.push_back(CreateUidProcessStateChangedEvent(
            configAddedTimeNs + bucketSizeNs + 50, 1 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + bucketSizeNs + 150));

    events.push_back(CreateUidProcessStateChangedEvent(
            configAddedTimeNs + bucketSizeNs + 200, 2 /*uid*/,
            android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + bucketSizeNs + 220));

    events.push_back(
            CreateUidProcessStateChangedEvent(configAddedTimeNs + bucketSizeNs + 250, 2 /*uid*/,
                                              android::app::ProcessStateEnum::PROCESS_STATE_TOP));
    events.push_back(CreateBatterySaverOnEvent(configAddedTimeNs + bucketSizeNs + 300));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        processor->OnLogEvent(event.get());
    }

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + (2 * bucketSizeNs) + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(reports.reports_size(), 1);
    ASSERT_EQ(reports.reports(0).metrics_size(), 1);
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    EXPECT_EQ((int)gaugeMetrics.data_size(), 5);

    // Data 0, PROCESS_STATE_IMPORTANT_FOREGROUND, UID 1
    auto data = gaugeMetrics.data(0);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_int(),
              1 /* uid value */);
    ASSERT_EQ(data.bucket_info_size(), 2);
    EXPECT_EQ(data.slice_by_state(0).value(),
              android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND);
    // First Bucket
    ValidateGaugeBucketTimes(
            data.bucket_info(0),
            /*startTimeNs=*/configAddedTimeNs,
            /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
            /*eventTimesNs=*/
            {(int64_t)(configAddedTimeNs + 80), (int64_t)(configAddedTimeNs + 150)});
    // Second Bucket
    ValidateGaugeBucketTimes(data.bucket_info(1),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 150),
                              (int64_t)(configAddedTimeNs + bucketSizeNs + 220),
                              (int64_t)(configAddedTimeNs + bucketSizeNs + 300)});

    // Data 1, PROCESS_STATE_IMPORTANT_BACKGROUND, UID 1
    data = gaugeMetrics.data(1);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_int(),
              1 /* uid value */);
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).value(),
              android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_BACKGROUND);
    // First Bucket
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + 250)});

    // Data 2, StateTracker::kStateUnknown, UID 2
    data = gaugeMetrics.data(2);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_int(),
              2 /* uid value */);
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).value(), -1 /* StateTracker::kStateUnknown */);
    // First Bucket
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs,
                             /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + 80)});

    // Data 3, PROCESS_STATE_TOP, UID 2
    data = gaugeMetrics.data(3);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_int(),
              2 /* uid value */);
    ASSERT_EQ(data.bucket_info_size(), 2);
    EXPECT_EQ(data.slice_by_state(0).value(), android::app::ProcessStateEnum::PROCESS_STATE_TOP);
    // First Bucket
    ValidateGaugeBucketTimes(
            data.bucket_info(0),
            /*startTimeNs=*/configAddedTimeNs,
            /*endTimeNs=*/configAddedTimeNs + bucketSizeNs,
            /*eventTimesNs=*/
            {(int64_t)(configAddedTimeNs + 150), (int64_t)(configAddedTimeNs + 250)});
    // Second Bucket
    ValidateGaugeBucketTimes(data.bucket_info(1),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 150),
                              (int64_t)(configAddedTimeNs + bucketSizeNs + 300)});

    // Data 4, PROCESS_STATE_IMPORTANT_FOREGROUND, UID 2
    data = gaugeMetrics.data(4);
    EXPECT_EQ(data.dimensions_in_what().value_tuple().dimensions_value(0).value_int(),
              2 /* uid value */);
    ASSERT_EQ(data.bucket_info_size(), 1);
    EXPECT_EQ(data.slice_by_state(0).value(),
              android::app::ProcessStateEnum::PROCESS_STATE_IMPORTANT_FOREGROUND);
    // Second Bucket Only
    ValidateGaugeBucketTimes(data.bucket_info(0),
                             /*startTimeNs=*/configAddedTimeNs + bucketSizeNs,
                             /*endTimeNs=*/configAddedTimeNs + 2 * bucketSizeNs,
                             /*eventTimesNs=*/
                             {(int64_t)(configAddedTimeNs + bucketSizeNs + 220)});
}

TEST(GaugeMetricE2ePulledTest, TestFieldFilterOmit) {
    auto config = CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE, /* useCondition */ false);
    config.mutable_gauge_metric(0)->mutable_gauge_fields_filter()->mutable_omit_fields()->set_field(
            ATOM_TAG);
    config.mutable_gauge_metric(0)
            ->mutable_gauge_fields_filter()
            ->mutable_omit_fields()
            ->add_child()
            ->set_field(2);  // subsystem_subname
    int64_t baseTimeNs = getElapsedRealtimeNs();
    int64_t configAddedTimeNs = 10 * 60 * NS_PER_SEC + baseTimeNs;
    int64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;

    ConfigKey cfgKey;
    auto processor =
            CreateStatsLogProcessor(baseTimeNs, configAddedTimeNs, config, cfgKey,
                                    SharedRefBase::make<FakeSubsystemSleepCallback>(), ATOM_TAG);

    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 2 + 1);

    ConfigMetricsReportList reports;
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, configAddedTimeNs + 3 * bucketSizeNs + 10, false, true,
                            ADB_DUMP, FAST, &buffer);
    EXPECT_TRUE(buffer.size() > 0);
    EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
    backfillDimensionPath(&reports);
    backfillStringInReport(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    ASSERT_EQ(1, reports.reports_size());
    ASSERT_EQ(1, reports.reports(0).metrics_size());
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics;
    sortMetricDataByDimensionsValue(reports.reports(0).metrics(0).gauge_metrics(), &gaugeMetrics);
    ASSERT_GT((int)gaugeMetrics.data_size(), 1);

    auto data = gaugeMetrics.data(0);
    ASSERT_EQ(data.bucket_info_size(), 1);
    ASSERT_EQ(data.bucket_info(0).atom_size(), 1);
    EXPECT_FALSE(data.bucket_info(0).atom(0).subsystem_sleep_state().has_subname());
    EXPECT_EQ(data.bucket_info(0).atom(0).subsystem_sleep_state().count(), 1);
    EXPECT_GT(data.bucket_info(0).atom(0).subsystem_sleep_state().time_millis(), 0);
}
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif

}  // namespace statsd
}  // namespace os
}  // namespace android
