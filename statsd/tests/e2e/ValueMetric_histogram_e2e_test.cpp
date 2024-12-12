/*
 * Copyright (C) 2024, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <aidl/android/os/BnPullAtomCallback.h>
#include <aidl/android/os/IPullAtomCallback.h>
#include <aidl/android/os/IPullAtomResultReceiver.h>
#include <aidl/android/util/StatsEventParcel.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>

#include "src/StatsLogProcessor.h"
#include "src/logd/LogEvent.h"
#include "src/stats_log.pb.h"
#include "src/stats_log_util.h"
#include "src/statsd_config.pb.h"
#include "tests/metrics/parsing_utils/parsing_test_utils.h"
#include "tests/statsd_test_util.h"

#ifdef __ANDROID__

using aidl::android::util::StatsEventParcel;
using namespace std;
using namespace testing;

namespace android {
namespace os {
namespace statsd {
namespace {

class ValueMetricHistogramE2eTest : public Test {
protected:
    void createProcessor(const StatsdConfig& config,
                         const shared_ptr<IPullAtomCallback>& puller = nullptr,
                         int32_t pullAtomId = 0) {
        processor = CreateStatsLogProcessor(baseTimeNs, bucketStartTimeNs, config, cfgKey, puller,
                                            pullAtomId);
    }

    optional<ConfigMetricsReportList> getReports(int64_t dumpTimeNs) {
        ConfigMetricsReportList reports;
        vector<uint8_t> buffer;
        processor->onDumpReport(cfgKey, dumpTimeNs,
                                /*include_current_bucket*/ false, true, ADB_DUMP, FAST, &buffer);
        if (reports.ParseFromArray(&buffer[0], buffer.size())) {
            backfillDimensionPath(&reports);
            backfillStringInReport(&reports);
            backfillStartEndTimestamp(&reports);
            return reports;
        }
        return nullopt;
    }

    optional<ConfigMetricsReportList> getReports() {
        return getReports(bucketStartTimeNs + bucketSizeNs);
    }

    void logEvents(const vector<shared_ptr<LogEvent>>& events) {
        for (const shared_ptr<LogEvent> event : events) {
            processor->OnLogEvent(event.get());
        }
    }

    void validateHistogram(const ConfigMetricsReportList& reports, int valueIndex,
                           const vector<int>& binCounts) {
        ASSERT_EQ(reports.reports_size(), 1);
        ConfigMetricsReport report = reports.reports(0);
        ASSERT_EQ(report.metrics_size(), 1);
        StatsLogReport metricReport = report.metrics(0);
        ASSERT_TRUE(metricReport.has_value_metrics());
        ASSERT_EQ(metricReport.value_metrics().skipped_size(), 0);
        ValueMetricData data = metricReport.value_metrics().data(0);
        ASSERT_EQ(data.bucket_info_size(), 1);
        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_GE(bucket.values_size(), valueIndex + 1);
        ASSERT_THAT(bucket.values(valueIndex),
                    Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        ASSERT_THAT(bucket.values(valueIndex).histogram().count(), ElementsAreArray(binCounts));
    }

    const uint64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(TEN_MINUTES) * 1000000LL;
    const uint64_t baseTimeNs = getElapsedRealtimeNs();
    const uint64_t bucketStartTimeNs = baseTimeNs + bucketSizeNs;  // 0:10
    ConfigKey cfgKey;
    sp<StatsLogProcessor> processor;
};

class ValueMetricHistogramE2eTestPushedExplicitBins : public ValueMetricHistogramE2eTest {
protected:
    void SetUp() override {
        StatsdConfig config = createExplicitHistogramStatsdConfig(/* bins */ {1, 7, 10, 20});
        createProcessor(config);
    }
};

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestNoEvents) {
    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_TRUE(metricReport.has_value_metrics());
    ASSERT_EQ(metricReport.value_metrics().skipped_size(), 1);
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestOneEventInFirstBinAfterUnderflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 5,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {0, 1, -3});
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestOneEventInOverflowAndUnderflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 0,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* value1 */ 20,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {1, -3, 1});
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestOneEventInUnderflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ -1,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {1, -4});
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestOneEventInOverflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 100,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {-4, 1});
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestOneEventInFirstBinBeforeOverflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 15,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {-3, 1, 0});
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestOneEventInMiddleBin) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 7,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {-2, 1, -2});
}

TEST_F(ValueMetricHistogramE2eTestPushedExplicitBins, TestMultipleEvents) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 8,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* value1 */ 15,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 30, /* value1 */ 19,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 40, /* value1 */ 3,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 50, /* value1 */ 9,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 60, /* value1 */ 3,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {0, 2, 2, 2, 0});
}

class ValueMetricHistogramE2eTestPushedLinearBins : public ValueMetricHistogramE2eTest {
protected:
    void SetUp() override {
        // Bin starts: [UNDERFLOW, -10, -6, -2, 2, 6, 10]
        StatsdConfig config =
                createGeneratedHistogramStatsdConfig(/* min */ -10, /* max */ 10, /* count */ 5,
                                                     HistogramBinConfig::GeneratedBins::LINEAR);
        createProcessor(config);
    }
};

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestNoEvents) {
    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_TRUE(metricReport.has_value_metrics());
    ASSERT_EQ(metricReport.value_metrics().skipped_size(), 1);
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestOneEventInFirstBinAfterUnderflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ -10,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {0, 1, -5});
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestOneEventInOverflowAndUnderflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ -11,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* value1 */ 10,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {1, -5, 1});
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestOneEventInUnderflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ -15,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {1, -6});
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestOneEventInOverflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 100,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {-6, 1});
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestOneEventInFirstBinBeforeOverflow) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 6,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {-5, 1, 0});
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestOneEventInMiddleBin) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 0,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {-3, 1, -3});
}

TEST_F(ValueMetricHistogramE2eTestPushedLinearBins, TestMultipleEvents) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 1,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* value1 */ 4,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 30, /* value1 */ -9,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 40, /* value1 */ 3,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 50, /* value1 */ 8,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 60, /* value1 */ -11,
                                      /* value2 */ 0)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {1, 1, 0, 1, 2, 1, 0});
}

class ValueMetricHistogramE2eTestMultiplePushedHistograms : public ValueMetricHistogramE2eTest {
protected:
    void SetUp() override {
        StatsdConfig config;
        *config.add_atom_matcher() = CreateSimpleAtomMatcher("matcher", /* atomId */ 1);
        *config.add_value_metric() =
                createValueMetric("ValueMetric", config.atom_matcher(0), /* valueFields */ {1, 2},
                                  {ValueMetric::HISTOGRAM, ValueMetric::HISTOGRAM},
                                  /* condition */ nullopt, /* states */ {});

        // Bin starts: [UNDERFLOW, 5, 10, 20, 40, 80, 160]
        *config.mutable_value_metric(0)->add_histogram_bin_configs() =
                createGeneratedBinConfig(/* id */ 1, /* min */ 5, /* max */ 160, /* count */ 5,
                                         HistogramBinConfig::GeneratedBins::EXPONENTIAL);

        // Bin starts: [UNDERFLOW, -10, -6, -2, 2, 6, 10]
        *config.mutable_value_metric(0)->add_histogram_bin_configs() =
                createGeneratedBinConfig(/* id */ 2, /* min */ -10, /* max */ 10, /* count */ 5,
                                         HistogramBinConfig::GeneratedBins::LINEAR);

        createProcessor(config);
    }
};

TEST_F(ValueMetricHistogramE2eTestMultiplePushedHistograms, TestNoEvents) {
    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    EXPECT_TRUE(metricReport.has_value_metrics());
    ASSERT_EQ(metricReport.value_metrics().skipped_size(), 1);
}

TEST_F(ValueMetricHistogramE2eTestMultiplePushedHistograms, TestMultipleEvents) {
    logEvents({CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 90,
                                      /* value2 */ 0),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* value1 */ 6,
                                      /* value2 */ 12),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 30, /* value1 */ 50,
                                      /* value2 */ -1),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 40, /* value1 */ 30,
                                      /* value2 */ 5),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 50, /* value1 */ 15,
                                      /* value2 */ 2),
               CreateTwoValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 60, /* value1 */ 160,
                                      /* value2 */ 9)});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 0, {0, 1, 1, 1, 1, 1, 1});
    TRACE_CALL(validateHistogram, *reports, /* valueIndex */ 1, {-3, 2, 2, 1, 1});
}

TEST_F(ValueMetricHistogramE2eTest, TestDimensionConditionAndMultipleAggregationTypes) {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateSimpleAtomMatcher("matcher", /* atomId */ 1);
    *config.add_atom_matcher() = CreateScreenTurnedOnAtomMatcher();
    *config.add_atom_matcher() = CreateScreenTurnedOffAtomMatcher();
    *config.add_predicate() = CreateScreenIsOnPredicate();
    config.mutable_predicate(0)->mutable_simple_predicate()->set_initial_value(
            SimplePredicate::FALSE);
    *config.add_value_metric() =
            createValueMetric("ValueMetric", config.atom_matcher(0), /* valueFields */ {1, 2, 2},
                              {ValueMetric::HISTOGRAM, ValueMetric::SUM, ValueMetric::MIN},
                              /* condition */ config.predicate(0).id(), /* states */ {});
    *config.mutable_value_metric(0)->mutable_dimensions_in_what() =
            CreateDimensions(/* atomId */ 1, {3 /* value3 */});

    // Bin starts: [UNDERFLOW, 5, 10, 20, 40, 80, 160]
    *config.mutable_value_metric(0)->add_histogram_bin_configs() =
            createGeneratedBinConfig(/* id */ 1, /* min */ 5, /* max */ 160, /* count */ 5,
                                     HistogramBinConfig::GeneratedBins::EXPONENTIAL);

    createProcessor(config);

    logEvents(
            {CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* value1 */ 90,
                                      /* value2 */ 0, /* value3 */ 1),
             CreateScreenStateChangedEvent(bucketStartTimeNs + 15, android::view::DISPLAY_STATE_ON),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* value1 */ 6,
                                      /* value2 */ 12, /* value3 */ 1),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 30, /* value1 */ 50,
                                      /* value2 */ -1, /* value3 */ 1),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 40, /* value1 */ 30,
                                      /* value2 */ 5, /* value3 */ 2),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 50, /* value1 */ 15,
                                      /* value2 */ 2, /* value3 */ 1),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 60, /* value1 */ 5,
                                      /* value2 */ 3, /* value3 */ 2),
             CreateScreenStateChangedEvent(bucketStartTimeNs + 65,
                                           android::view::DISPLAY_STATE_OFF),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 70, /* value1 */ 160,
                                      /* value2 */ 9, /* value3 */ 1),
             CreateThreeValueLogEvent(/* atomId */ 1, bucketStartTimeNs + 80, /* value1 */ 70,
                                      /* value2 */ 20, /* value3 */ 2)});

    optional<ConfigMetricsReportList> reports = getReports();

    ASSERT_NE(reports, nullopt);
    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    ASSERT_TRUE(metricReport.has_value_metrics());
    ASSERT_EQ(metricReport.value_metrics().skipped_size(), 0);
    ASSERT_EQ(metricReport.value_metrics().data_size(), 2);

    // Dimension 1
    {
        ValueMetricData data = metricReport.value_metrics().data(0);
        ASSERT_EQ(data.bucket_info_size(), 1);
        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_EQ(bucket.values_size(), 3);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(0).histogram().count(), ElementsAreArray({0, 1, 1, 0, 1, -2}));
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(1).value_long(), 13);
        ASSERT_THAT(bucket.values(2), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(2).value_long(), -1);
    }

    // Dimension 2
    {
        ValueMetricData data = metricReport.value_metrics().data(1);
        ASSERT_EQ(data.bucket_info_size(), 1);
        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_EQ(bucket.values_size(), 3);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(0).histogram().count(), ElementsAreArray({0, 1, 0, 1, -3}));
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(1).value_long(), 8);
        ASSERT_THAT(bucket.values(2), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(2).value_long(), 3);
    }
}

// Test fixture which uses a ValueMetric on a pushed atom with a statsd-aggregated histogram as well
// as a client-aggregated histogram.
class ValueMetricHistogramE2eTestClientAggregatedPushedHistogram
    : public ValueMetricHistogramE2eTest {
protected:
    void SetUp() override {
        StatsdConfig config;
        *config.add_atom_matcher() = CreateSimpleAtomMatcher("matcher", /* atomId */ 1);
        *config.add_value_metric() =
                createValueMetric("ValueMetric", config.atom_matcher(0), /* valueFields */ {2, 3},
                                  {ValueMetric::HISTOGRAM, ValueMetric::HISTOGRAM},
                                  /* condition */ nullopt, /* states */ {});
        config.mutable_value_metric(0)->mutable_value_field()->mutable_child(1)->set_position(ALL);
        *config.mutable_value_metric(0)->mutable_dimensions_in_what() =
                CreateRepeatedDimensions(/* atomId */ 1, {1 /* uid */}, {Position::FIRST});

        // Bin starts: [UNDERFLOW, -10, -6, -2, 2, 6, 10]
        *config.mutable_value_metric(0)->add_histogram_bin_configs() =
                createGeneratedBinConfig(/* id */ 1, /* min */ -10, /* max */ 10, /* count */ 5,
                                         HistogramBinConfig::GeneratedBins::LINEAR);

        config.mutable_value_metric(0)->add_histogram_bin_configs()->set_id(2);
        config.mutable_value_metric(0)
                ->mutable_histogram_bin_configs(1)
                ->mutable_client_aggregated_bins();

        createProcessor(config);

        StatsdStats::getInstance().reset();
    }

public:
    void doTestMultipleEvents() __INTRODUCED_IN(__ANDROID_API_T__);
    void doTestBadHistograms() __INTRODUCED_IN(__ANDROID_API_T__);
};

TEST_F_GUARDED(ValueMetricHistogramE2eTestClientAggregatedPushedHistogram, TestMultipleEvents,
               __ANDROID_API_T__) {
    logEvents({makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* uids */ {1},
                                       /* value1 */ 0, /* value2 */ {0, 0, 1, 1}),
               makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* uids */ {1},
                                       /* value1 */ 12, /* value2 */ {0, 2, 0, 1}),
               makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 30, /* uids */ {2},
                                       /* value1 */ -1, /* value2 */ {1, 0, 0, 0}),
               makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 40, /* uids */ {1},
                                       /* value1 */ 5, /* value2 */ {0, 0, 0, 0}),
               makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 50, /* uids */ {2},
                                       /* value1 */ 2, /* value2 */ {0, 2, 0, 1}),
               makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 60, /* uids */ {2},
                                       /* value1 */ 9, /* value2 */ {10, 5, 2, 2})});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    ASSERT_TRUE(metricReport.has_value_metrics());
    ASSERT_EQ(metricReport.value_metrics().skipped_size(), 0);
    ASSERT_EQ(metricReport.value_metrics().data_size(), 2);

    // Dimension 1
    {
        ValueMetricData data = metricReport.value_metrics().data(0);
        ASSERT_EQ(data.bucket_info_size(), 1);
        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(0).histogram().count(), ElementsAreArray({-3, 1, 1, 0, 1}));
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({0, 2, 1, 2}));
    }

    // Dimension 2
    {
        ValueMetricData data = metricReport.value_metrics().data(1);
        ASSERT_EQ(data.bucket_info_size(), 1);
        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(0).histogram().count(), ElementsAreArray({-3, 1, 1, 1, 0}));
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({11, 7, 2, 3}));
    }
}

TEST_F_GUARDED(ValueMetricHistogramE2eTestClientAggregatedPushedHistogram, TestBadHistograms,
               __ANDROID_API_T__) {
    logEvents(
            {// Histogram has negative bin count.
             makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 10, /* uids */ {1},
                                     /* value1 */ 0, /* value2 */ {0, 0, -1, 1}),

             // Good histogram, recorded in interval.
             makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 20, /* uids */ {1},
                                     /* value1 */ 12, /* value2 */ {0, 2, 0, 1}),

             // Histogram has more bins than what's already aggregated. Aggregation is not updated.
             makeRepeatedUidLogEvent(/* atomId */ 1, bucketStartTimeNs + 30, /* uids */ {1},
                                     /* value1 */ -1, /* value2 */ {1, 0, 0, 0, 0})});

    optional<ConfigMetricsReportList> reports = getReports();
    ASSERT_NE(reports, nullopt);

    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);
    StatsLogReport metricReport = report.metrics(0);
    ASSERT_TRUE(metricReport.has_value_metrics());
    EXPECT_EQ(metricReport.value_metrics().skipped_size(), 0);
    ASSERT_EQ(metricReport.value_metrics().data_size(), 1);
    ValueMetricData data = metricReport.value_metrics().data(0);
    ASSERT_EQ(data.bucket_info_size(), 1);
    ValueBucketInfo bucket = data.bucket_info(0);
    ASSERT_EQ(bucket.values_size(), 2);
    ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
    EXPECT_THAT(bucket.values(0).histogram().count(), ElementsAreArray({-3, 2, -2, 1}));
    ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
    EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({0, 2, 0, 1}));

    StatsdStatsReport statsdStatsReport = getStatsdStatsReport();
    ASSERT_EQ(statsdStatsReport.atom_metric_stats_size(), 1);
    EXPECT_EQ(statsdStatsReport.atom_metric_stats(0).bad_value_type(), 2);
}

class Puller : public BnPullAtomCallback {
public:
    int curPullNum = 0;

    // Mapping of uid to histograms for each pull
    const map<int, vector<vector<int>>> histMap;

    Puller(const map<int, vector<vector<int>>>& histMap) : histMap(histMap) {
    }

    Status onPullAtom(int atomId,
                      const shared_ptr<IPullAtomResultReceiver>& resultReceiver) override {
        if (__builtin_available(android __ANDROID_API_T__, *)) {
            vector<StatsEventParcel> parcels;
            for (auto const& [uid, histograms] : histMap) {
                const vector<int>& histogram = histograms[curPullNum];
                AStatsEvent* statsEvent = AStatsEvent_obtain();
                AStatsEvent_setAtomId(statsEvent, atomId);
                AStatsEvent_writeInt32(statsEvent, uid);
                AStatsEvent_writeInt32(statsEvent, curPullNum);
                AStatsEvent_writeInt32Array(statsEvent, histogram.data(), histogram.size());
                AStatsEvent_build(statsEvent);
                size_t size;
                uint8_t* buffer = AStatsEvent_getBuffer(statsEvent, &size);

                StatsEventParcel p;
                // vector.assign() creates a copy, but this is inevitable unless
                // stats_event.h/c uses a vector as opposed to a buffer.
                p.buffer.assign(buffer, buffer + size);
                parcels.push_back(std::move(p));
                AStatsEvent_release(statsEvent);
            }
            curPullNum++;
            resultReceiver->pullFinished(atomId, /*success=*/true, parcels);
        }
        return Status::ok();
    }
};

}  // anonymous namespace

// Test fixture which uses a ValueMetric on a pulled atom with a client-aggregated histogram.
class ValueMetricHistogramE2eTestClientAggregatedPulledHistogram
    : public ValueMetricHistogramE2eTest {
protected:
    const int atomId = 10'000;
    StatsdConfig config;

    void SetUp() override {
        *config.add_atom_matcher() = CreateSimpleAtomMatcher("matcher", atomId);

        *config.add_value_metric() =
                createValueMetric("ValueMetric", config.atom_matcher(0), /* valueFields */ {2, 3},
                                  {ValueMetric::SUM, ValueMetric::HISTOGRAM},
                                  nullopt /* condition */, /* states */ {});

        config.mutable_value_metric(0)->mutable_value_field()->mutable_child(1)->set_position(ALL);
        *config.mutable_value_metric(0)->mutable_dimensions_in_what() =
                CreateDimensions(atomId, {1 /* uid */});

        config.mutable_value_metric(0)->add_histogram_bin_configs()->set_id(1);
        config.mutable_value_metric(0)
                ->mutable_histogram_bin_configs(0)
                ->mutable_client_aggregated_bins();

        config.mutable_value_metric(0)->set_skip_zero_diff_output(false);

        config.add_default_pull_packages("AID_ROOT");

        StatsdStats::getInstance().reset();
    }

    void createProcessorWithHistData(const map<int, vector<vector<int>>>& histData) {
        createProcessor(config, SharedRefBase::make<Puller>(histData), atomId);
    }

public:
    void doTestPulledAtom() __INTRODUCED_IN(__ANDROID_API_T__);
    void doTestBadHistograms() __INTRODUCED_IN(__ANDROID_API_T__);
    void doTestZeroDefaultBase() __INTRODUCED_IN(__ANDROID_API_T__);
};

TEST_F_GUARDED(ValueMetricHistogramE2eTestClientAggregatedPulledHistogram, TestPulledAtom,
               __ANDROID_API_T__) {
    map<int, vector<vector<int>>> histData;
    histData[1].push_back({0, 0, 0, 0});
    histData[1].push_back({1, 0, 2, 0});
    histData[1].push_back({1, 1, 3, 5});
    histData[1].push_back({1, 1, 3, 5});
    histData[1].push_back({3, 1, 3, 5});
    histData[2].push_back({0, 1, 0, 0});
    histData[2].push_back({1, 3, 0, 2});
    histData[2].push_back({1, 3, 0, 2});
    histData[2].push_back({2, 9, 3, 5});
    histData[2].push_back({3, 9, 3, 5});
    createProcessorWithHistData(histData);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 2 + 1);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 3 + 2);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 4 + 3);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 5 + 4);

    optional<ConfigMetricsReportList> reports = getReports(baseTimeNs + bucketSizeNs * 6 + 100);
    ASSERT_NE(reports, nullopt);

    ASSERT_NE(reports, nullopt);
    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);

    StatsLogReport metricReport = report.metrics(0);
    ASSERT_TRUE(metricReport.has_value_metrics());
    EXPECT_EQ(metricReport.value_metrics().skipped_size(), 0);
    ASSERT_EQ(metricReport.value_metrics().data_size(), 2);
    StatsLogReport::ValueMetricDataWrapper valueMetrics;
    sortMetricDataByDimensionsValue(metricReport.value_metrics(), &valueMetrics);

    // Dimension uid = 1
    {
        ValueMetricData data = valueMetrics.data(0);
        ASSERT_EQ(data.bucket_info_size(), 4);

        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, 0, 2, 0}));

        bucket = data.bucket_info(1);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({0, 1, 1, 5}));

        bucket = data.bucket_info(2);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({-4}));

        bucket = data.bucket_info(3);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({2, -3}));
    }

    // Dimension uid = 2
    {
        ValueMetricData data = valueMetrics.data(1);
        ASSERT_EQ(data.bucket_info_size(), 4);

        ValueBucketInfo bucket = data.bucket_info(0);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, 2, 0, 2}));

        bucket = data.bucket_info(1);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({-4}));

        bucket = data.bucket_info(2);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, 6, 3, 3}));

        bucket = data.bucket_info(3);
        ASSERT_EQ(bucket.values_size(), 2);
        ASSERT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
        EXPECT_EQ(bucket.values(0).value_long(), 1);
        ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
        EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, -3}));
    }
}

TEST_F_GUARDED(ValueMetricHistogramE2eTestClientAggregatedPulledHistogram, TestBadHistograms,
               __ANDROID_API_T__) {
    map<int, vector<vector<int>>> histData;
    histData[1].push_back({0, 0, 0, 0});  // base updated.

    histData[1].push_back({1, 0, 2});  // base updated, no aggregate recorded due to
                                       // ERROR_BINS_MISMATCH

    histData[1].push_back({1, -1, 3});  // base is reset, no aggregate recorded due to
                                        // negative bin count

    histData[1].push_back({1, 2, 3});  // base updated, no aggregate recorded

    histData[1].push_back({2, 6, 3});  // base updated, aggregate updated

    histData[1].push_back({3, 9, 4});  // base updated, aggregate updated

    histData[1].push_back({4, 8, 5});  // base updated, no aggregate recorded because 2nd bin
                                       // decreased

    createProcessorWithHistData(histData);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 2 + 1);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 3 + 2);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 4 + 3);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 5 + 4);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 6 + 5);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 7 + 6);

    optional<ConfigMetricsReportList> reports = getReports(baseTimeNs + bucketSizeNs * 8 + 100);

    ASSERT_NE(reports, nullopt);
    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);

    StatsLogReport metricReport = report.metrics(0);
    ASSERT_TRUE(metricReport.has_value_metrics());
    EXPECT_EQ(metricReport.value_metrics().skipped_size(), 0);

    EXPECT_EQ(metricReport.value_metrics().data_size(), 1);
    ValueMetricData data = metricReport.value_metrics().data(0);
    EXPECT_EQ(data.bucket_info_size(), 6);

    ValueBucketInfo bucket = data.bucket_info(0);
    ASSERT_EQ(bucket.values_size(), 1);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));

    bucket = data.bucket_info(1);
    ASSERT_EQ(bucket.values_size(), 1);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));

    bucket = data.bucket_info(2);
    ASSERT_EQ(bucket.values_size(), 1);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));

    bucket = data.bucket_info(3);
    ASSERT_EQ(bucket.values_size(), 2);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
    ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
    EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, 4, 0}));

    bucket = data.bucket_info(4);
    ASSERT_EQ(bucket.values_size(), 2);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
    ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
    EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, 3, 1}));

    bucket = data.bucket_info(5);
    ASSERT_EQ(bucket.values_size(), 1);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));

    StatsdStatsReport statsdStatsReport = getStatsdStatsReport();
    ASSERT_EQ(statsdStatsReport.atom_metric_stats_size(), 1);
    EXPECT_EQ(statsdStatsReport.atom_metric_stats(0).bad_value_type(), 3);
}

TEST_F_GUARDED(ValueMetricHistogramE2eTestClientAggregatedPulledHistogram, TestZeroDefaultBase,
               __ANDROID_API_T__) {
    config.mutable_value_metric(0)->set_use_zero_default_base(true);

    map<int, vector<vector<int>>> histData;
    histData[1].push_back({-1, 0, 2});  // base not updated
    histData[1].push_back({1, 0, 2});   // base updated, aggregate also recorded.
    histData[1].push_back({2, 0, 2});   // base updated, aggregate also recorded.

    createProcessorWithHistData(histData);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 2 + 1);

    processor->mPullerManager->ForceClearPullerCache();
    processor->informPullAlarmFired(baseTimeNs + bucketSizeNs * 3 + 1);

    optional<ConfigMetricsReportList> reports = getReports(baseTimeNs + bucketSizeNs * 4 + 100);

    ASSERT_NE(reports, nullopt);
    ASSERT_EQ(reports->reports_size(), 1);
    ConfigMetricsReport report = reports->reports(0);
    ASSERT_EQ(report.metrics_size(), 1);

    StatsLogReport metricReport = report.metrics(0);
    ASSERT_TRUE(metricReport.has_value_metrics());
    EXPECT_EQ(metricReport.value_metrics().skipped_size(), 0);

    EXPECT_EQ(metricReport.value_metrics().data_size(), 1);
    ValueMetricData data = metricReport.value_metrics().data(0);
    EXPECT_EQ(data.bucket_info_size(), 2);

    ValueBucketInfo bucket = data.bucket_info(0);
    ASSERT_EQ(bucket.values_size(), 2);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
    EXPECT_EQ(bucket.values(0).value_long(), 1);
    ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
    EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, 0, 2}));

    bucket = data.bucket_info(1);
    ASSERT_EQ(bucket.values_size(), 2);
    EXPECT_THAT(bucket.values(0), Property(&ValueBucketInfo::Value::has_value_long, IsTrue()));
    EXPECT_EQ(bucket.values(0).value_long(), 1);
    ASSERT_THAT(bucket.values(1), Property(&ValueBucketInfo::Value::has_histogram, IsTrue()));
    EXPECT_THAT(bucket.values(1).histogram().count(), ElementsAreArray({1, -2}));
}

}  // namespace statsd
}  // namespace os
}  // namespace android
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
