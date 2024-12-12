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

#include "src/metrics/parsing_utils/histogram_parsing_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <variant>
#include <vector>

#include "src/guardrail/StatsdStats.h"
#include "src/stats_util.h"
#include "src/statsd_config.pb.h"
#include "tests/metrics/parsing_utils/parsing_test_utils.h"
#include "tests/statsd_test_util.h"

#ifdef __ANDROID__

using namespace std;
using namespace testing;

namespace android {
namespace os {
namespace statsd {
namespace {

using HistogramParsingUtilsTest = InitConfigTest;

constexpr auto LINEAR = HistogramBinConfig::GeneratedBins::LINEAR;
constexpr auto EXPONENTIAL = HistogramBinConfig::GeneratedBins::EXPONENTIAL;

TEST_F(HistogramParsingUtilsTest, TestMissingHistogramBinConfigId) {
    StatsdConfig config = createExplicitHistogramStatsdConfig(/* bins */ {5});
    config.mutable_value_metric(0)->mutable_histogram_bin_configs()->Mutable(0)->clear_id();

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_BIN_CONFIG_ID,
                                  config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestMissingHistogramBinConfigBinningStrategy) {
    StatsdConfig config = createHistogramStatsdConfig();
    config.mutable_value_metric(0)->add_histogram_bin_configs()->set_id(1);

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_UNKNOWN_BINNING_STRATEGY,
                                  config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestGeneratedBinsMissingMin) {
    StatsdConfig config =
            createGeneratedHistogramStatsdConfig(/* min */ 1, /* max */ 10, /* count */ 5, LINEAR);
    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_generated_bins()
            ->clear_min();

    EXPECT_EQ(
            initConfig(config),
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                                config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestGeneratedBinsMissingMax) {
    StatsdConfig config =
            createGeneratedHistogramStatsdConfig(/* min */ 1, /* max */ 10, /* count */ 5, LINEAR);
    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_generated_bins()
            ->clear_max();

    EXPECT_EQ(
            initConfig(config),
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                                config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestGeneratedBinsMissingCount) {
    StatsdConfig config =
            createGeneratedHistogramStatsdConfig(/* min */ 1, /* max */ 10, /* count */ 5, LINEAR);
    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_generated_bins()
            ->clear_count();

    EXPECT_EQ(
            initConfig(config),
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                                config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestGeneratedBinsMissingStrategy) {
    StatsdConfig config = createHistogramStatsdConfig();
    *config.mutable_value_metric(0)->add_histogram_bin_configs() =
            createGeneratedBinConfig(/* id */ 1, /* min */ 1, /* max */ 10, /* count */ 5,
                                     HistogramBinConfig::GeneratedBins::UNKNOWN);

    EXPECT_EQ(
            initConfig(config),
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                                config.value_metric(0).id()));

    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_generated_bins()
            ->clear_strategy();

    clearData();
    EXPECT_EQ(
            initConfig(config),
            InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                                config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestGeneratedBinsMinNotLessThanMax) {
    StatsdConfig config =
            createGeneratedHistogramStatsdConfig(/* min */ 10, /* max */ 10, /* count */ 5, LINEAR);

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_GENERATED_BINS_INVALID_MIN_MAX,
                      config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestExponentialBinsMinNotLessThanMax) {
    StatsdConfig config = createGeneratedHistogramStatsdConfig(/* min */ 10, /* max */ 10,
                                                               /* count */ 5, EXPONENTIAL);

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_GENERATED_BINS_INVALID_MIN_MAX,
                      config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestExponentialBinsZeroMin) {
    StatsdConfig config = createGeneratedHistogramStatsdConfig(/* min */ 0, /* max */ 10,
                                                               /* count */ 5, EXPONENTIAL);

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_GENERATED_BINS_INVALID_MIN_MAX,
                      config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestTooFewGeneratedBins) {
    StatsdConfig config =
            createGeneratedHistogramStatsdConfig(/* min */ 10, /* max */ 50, /* count */ 2, LINEAR);

    EXPECT_EQ(initConfig(config), nullopt);

    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_generated_bins()
            ->set_count(1);

    clearData();
    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_FEW_BINS,
                                  config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestTooManyGeneratedBins) {
    StatsdConfig config = createGeneratedHistogramStatsdConfig(/* min */ 10, /* max */ 50,
                                                               /* count */ 100, LINEAR);

    EXPECT_EQ(initConfig(config), nullopt);

    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_generated_bins()
            ->set_count(101);

    clearData();
    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_MANY_BINS,
                                  config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestTooFewExplicitBins) {
    StatsdConfig config = createExplicitHistogramStatsdConfig(/* bins */ {1});

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_FEW_BINS,
                                  config.value_metric(0).id()));

    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_explicit_bins()
            ->add_bin(2);

    clearData();
    EXPECT_EQ(initConfig(config), nullopt);
}

TEST_F(HistogramParsingUtilsTest, TestTooManyExplicitBins) {
    BinStarts bins(100);
    // Fill bins with values 1, 2, ..., 100.
    std::iota(std::begin(bins), std::end(bins), 1);
    StatsdConfig config = createExplicitHistogramStatsdConfig(bins);

    EXPECT_EQ(initConfig(config), nullopt);

    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_explicit_bins()
            ->add_bin(101);

    clearData();
    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_MANY_BINS,
                                  config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestExplicitBinsDuplicateValues) {
    BinStarts bins(50);
    // Fill bins with values 1, 2, ..., 50.
    std::iota(std::begin(bins), std::end(bins), 1);
    StatsdConfig config = createExplicitHistogramStatsdConfig(bins);

    config.mutable_value_metric(0)
            ->mutable_histogram_bin_configs(0)
            ->mutable_explicit_bins()
            ->add_bin(50);

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_EXPLICIT_BINS_NOT_STRICTLY_ORDERED,
                      config.value_metric(0).id()));
}

TEST_F(HistogramParsingUtilsTest, TestExplicitBinsUnsortedValues) {
    BinStarts bins(50);
    // Fill bins with values 1, 2, ..., 50.
    std::iota(std::begin(bins), std::end(bins), 1);

    // Swap values at indices 10 and 40.
    std::swap(bins[10], bins[40]);

    StatsdConfig config = createExplicitHistogramStatsdConfig(bins);

    EXPECT_EQ(initConfig(config),
              InvalidConfigReason(
                      INVALID_CONFIG_REASON_VALUE_METRIC_HIST_EXPLICIT_BINS_NOT_STRICTLY_ORDERED,
                      config.value_metric(0).id()));
}

const BinStarts getParsedBins(const ValueMetric& metric) {
    ParseHistogramBinConfigsResult result =
            parseHistogramBinConfigs(metric, /* aggregationTypes */ {ValueMetric::HISTOGRAM});
    return holds_alternative<vector<optional<const BinStarts>>>(result)
                   ? *get<vector<optional<const BinStarts>>>(result).front()
                   : BinStarts();
}

const BinStarts getParsedGeneratedBins(float min, float max, int count,
                                       HistogramBinConfig::GeneratedBins::Strategy strategy) {
    StatsdConfig config = createGeneratedHistogramStatsdConfig(min, max, count, strategy);

    return getParsedBins(config.value_metric(0));
}

const BinStarts getParsedLinearBins(float min, float max, int count) {
    return getParsedGeneratedBins(min, max, count, LINEAR);
}

TEST_F(HistogramParsingUtilsTest, TestValidLinearBins) {
    EXPECT_THAT(getParsedLinearBins(-10, 10, 5),
                ElementsAre(UNDERFLOW_BIN_START, -10, -6, -2, 2, 6, 10));
    EXPECT_THAT(getParsedLinearBins(-10, 10, 2), ElementsAre(UNDERFLOW_BIN_START, -10, 0, 10));
    EXPECT_THAT(getParsedLinearBins(-100, -50, 3),
                ElementsAre(UNDERFLOW_BIN_START, -100, FloatNear(-83.33, 0.01),
                            FloatNear(-66.67, 0.01), -50));
    EXPECT_THAT(getParsedLinearBins(2.5, 11.3, 7),
                ElementsAre(UNDERFLOW_BIN_START, 2.5, FloatNear(3.76, 0.01), FloatNear(5.01, 0.01),
                            FloatNear(6.27, 0.01), FloatNear(7.53, 0.01), FloatNear(8.79, 0.01),
                            FloatNear(10.04, 0.01), 11.3));
}

BinStarts getParsedExponentialBins(float min, float max, int count) {
    return getParsedGeneratedBins(min, max, count, EXPONENTIAL);
}

TEST_F(HistogramParsingUtilsTest, TestValidExponentialBins) {
    EXPECT_THAT(getParsedExponentialBins(5, 160, 5),
                ElementsAre(UNDERFLOW_BIN_START, 5, 10, 20, 40, 80, 160));
    EXPECT_THAT(getParsedExponentialBins(3, 1875, 4),
                ElementsAre(UNDERFLOW_BIN_START, 3, FloatEq(15), FloatEq(75), FloatEq(375), 1875));
    EXPECT_THAT(getParsedExponentialBins(1, 1000, 3),
                ElementsAre(UNDERFLOW_BIN_START, 1, 10, 100, 1000));
}

BinStarts getParsedExplicitBins(BinStarts bins) {
    StatsdConfig config = createExplicitHistogramStatsdConfig(bins);

    return getParsedBins(config.value_metric(0));
}

TEST_F(HistogramParsingUtilsTest, TestValidExplicitBins) {
    EXPECT_THAT(getParsedExplicitBins({0, 1, 2}), ElementsAre(UNDERFLOW_BIN_START, 0, 1, 2));
    EXPECT_THAT(getParsedExplicitBins({-1, 5, 200}), ElementsAre(UNDERFLOW_BIN_START, -1, 5, 200));
}

TEST_F(HistogramParsingUtilsTest, TestMultipleHistogramBinConfigs) {
    StatsdConfig config = createGeneratedHistogramStatsdConfig(/* min */ -100, /* max */ 0,
                                                               /* count */ 5, LINEAR);
    config.mutable_value_metric(0)->clear_aggregation_type();
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::HISTOGRAM);
    config.mutable_value_metric(0)->add_aggregation_types(ValueMetric::HISTOGRAM);
    config.mutable_value_metric(0)->mutable_value_field()->add_child()->set_field(2);
    *config.mutable_value_metric(0)->add_histogram_bin_configs() =
            createExplicitBinConfig(/* id */ 2, {1, 9, 30});

    ParseHistogramBinConfigsResult result = parseHistogramBinConfigs(
            config.value_metric(0),
            /* aggregationTypes */ {ValueMetric::HISTOGRAM, ValueMetric::HISTOGRAM});
    ASSERT_TRUE(holds_alternative<vector<optional<const BinStarts>>>(result));
    const vector<optional<const BinStarts>>& histograms =
            get<vector<optional<const BinStarts>>>(result);
    ASSERT_EQ(histograms.size(), 2);

    EXPECT_THAT(*(histograms[0]), ElementsAre(UNDERFLOW_BIN_START, -100, -80, -60, -40, -20, 0));
    EXPECT_THAT(*(histograms[1]), ElementsAre(UNDERFLOW_BIN_START, 1, 9, 30));
}

}  // anonymous namespace

}  // namespace statsd
}  // namespace os
}  // namespace android
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif
