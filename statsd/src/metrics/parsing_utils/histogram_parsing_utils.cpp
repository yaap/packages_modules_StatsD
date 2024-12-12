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

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "histogram_parsing_utils.h"

#include <google/protobuf/repeated_field.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>
#include <vector>

#include "guardrail/StatsdStats.h"
#include "src/statsd_config.pb.h"
#include "stats_util.h"

using google::protobuf::RepeatedPtrField;
using std::nullopt;
using std::optional;
using std::pow;
using std::variant;
using std::vector;

namespace android {
namespace os {
namespace statsd {
namespace {
constexpr int MIN_HISTOGRAM_BIN_COUNT = 2;
constexpr int MAX_HISTOGRAM_BIN_COUNT = 100;

BinStarts generateLinearBins(float min, float max, int count) {
    const float binWidth = (max - min) / count;

    // 2 extra bins for underflow and overflow.
    BinStarts bins(count + 2);
    bins[0] = UNDERFLOW_BIN_START;
    bins[1] = min;
    bins.back() = max;
    float curBin = min;

    // Generate values starting from 3rd element to (n-1)th element.
    std::generate(bins.begin() + 2, bins.end() - 1,
                  [&curBin, binWidth]() { return curBin += binWidth; });
    return bins;
}

BinStarts generateExponentialBins(float min, float max, int count) {
    BinStarts bins(count + 2);
    bins[0] = UNDERFLOW_BIN_START;
    bins[1] = min;
    bins.back() = max;

    // Determine the scale factor f, such that max = min * f^count.
    // So, f = (max / min)^(1 / count) ie. f is the count'th-root of max / min.
    const float factor = pow(max / min, 1.0 / count);

    // Generate values starting from 3rd element to (n-1)th element.
    float curBin = bins[1];
    std::generate(bins.begin() + 2, bins.end() - 1,
                  [&curBin, factor]() { return curBin *= factor; });

    return bins;
}

BinStarts createExplicitBins(const BinStarts& configBins) {
    BinStarts bins(configBins.size() + 1);
    bins[0] = UNDERFLOW_BIN_START;
    std::copy(configBins.begin(), configBins.end(), bins.begin() + 1);
    return bins;
}
}  // anonymous namespace

ParseHistogramBinConfigsResult parseHistogramBinConfigs(
        const ValueMetric& metric, const vector<ValueMetric::AggregationType>& aggregationTypes) {
    if (metric.histogram_bin_configs_size() == 0) {
        return {};
    }
    vector<optional<const BinStarts>> binStartsList;
    binStartsList.reserve(aggregationTypes.size());
    RepeatedPtrField<HistogramBinConfig>::const_iterator binConfigIt =
            metric.histogram_bin_configs().cbegin();
    for (const ValueMetric::AggregationType aggType : aggregationTypes) {
        if (aggType != ValueMetric::HISTOGRAM) {
            binStartsList.push_back(nullopt);
            continue;
        }
        const HistogramBinConfig& binConfig = *binConfigIt;
        if (!binConfig.has_id()) {
            ALOGE("cannot find id in HistogramBinConfig");
            return InvalidConfigReason(
                    INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_BIN_CONFIG_ID, metric.id());
        }
        switch (binConfig.binning_strategy_case()) {
            case HistogramBinConfig::kGeneratedBins: {
                const HistogramBinConfig::GeneratedBins& genBins = binConfig.generated_bins();
                if (!genBins.has_min() || !genBins.has_max() || !genBins.has_count() ||
                    !genBins.has_strategy()) {
                    ALOGE("Missing generated bin arguments");
                    return InvalidConfigReason(
                            INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                            metric.id());
                }
                if (genBins.count() < MIN_HISTOGRAM_BIN_COUNT) {
                    ALOGE("Too few generated bins");
                    return InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_FEW_BINS,
                                               metric.id());
                }
                if (genBins.count() > MAX_HISTOGRAM_BIN_COUNT) {
                    ALOGE("Too many generated bins");
                    return InvalidConfigReason(
                            INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_MANY_BINS, metric.id());
                }
                if (genBins.min() >= genBins.max()) {
                    ALOGE("Min should be lower than max for generated bins");
                    return InvalidConfigReason(
                            INVALID_CONFIG_REASON_VALUE_METRIC_HIST_GENERATED_BINS_INVALID_MIN_MAX,
                            metric.id());
                }

                switch (genBins.strategy()) {
                    case HistogramBinConfig::GeneratedBins::LINEAR: {
                        binStartsList.push_back(
                                generateLinearBins(genBins.min(), genBins.max(), genBins.count()));
                        break;
                    }
                    case HistogramBinConfig::GeneratedBins::EXPONENTIAL: {
                        // The starting point of exponential bins has to be greater than 0.
                        if (genBins.min() <= 0) {
                            ALOGE("Min should be greater than 0 for exponential bins");
                            return InvalidConfigReason(
                                    INVALID_CONFIG_REASON_VALUE_METRIC_HIST_GENERATED_BINS_INVALID_MIN_MAX,
                                    metric.id());
                        }
                        binStartsList.push_back(generateExponentialBins(
                                genBins.min(), genBins.max(), genBins.count()));
                        break;
                    }
                    default: {
                        ALOGE("Unknown GeneratedBins strategy");
                        return InvalidConfigReason(
                                INVALID_CONFIG_REASON_VALUE_METRIC_HIST_MISSING_GENERATED_BINS_ARGS,
                                metric.id());
                    }
                }

                break;
            }
            case HistogramBinConfig::kExplicitBins: {
                const HistogramBinConfig::ExplicitBins& explicitBins = binConfig.explicit_bins();
                if (explicitBins.bin_size() < MIN_HISTOGRAM_BIN_COUNT) {
                    ALOGE("Too few explicit bins");
                    return InvalidConfigReason(INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_FEW_BINS,
                                               metric.id());
                }
                if (explicitBins.bin_size() > MAX_HISTOGRAM_BIN_COUNT) {
                    ALOGE("Too many explicit bins");
                    return InvalidConfigReason(
                            INVALID_CONFIG_REASON_VALUE_METRIC_HIST_TOO_MANY_BINS, metric.id());
                }

                // Ensure explicit bins are strictly ordered in ascending order.
                // Use adjacent_find to find any 2 adjacent bin boundaries, b1 and b2, such that b1
                // >= b2. If any such adjacent bins are found, the bins are not strictly ascending
                // and the bin definition is invalid.
                if (std::adjacent_find(explicitBins.bin().begin(), explicitBins.bin().end(),
                                       std::greater_equal<float>()) != explicitBins.bin().end()) {
                    ALOGE("Explicit bins are not strictly ordered in ascending order");
                    return InvalidConfigReason(
                            INVALID_CONFIG_REASON_VALUE_METRIC_HIST_EXPLICIT_BINS_NOT_STRICTLY_ORDERED,
                            metric.id());
                }

                binStartsList.push_back(
                        createExplicitBins({explicitBins.bin().begin(), explicitBins.bin().end()}));

                break;
            }
            case HistogramBinConfig::kClientAggregatedBins: {
                binStartsList.push_back(nullopt);
                break;
            }
            default: {
                ALOGE("Either generated or explicit binning strategy must be set");
                return InvalidConfigReason(
                        INVALID_CONFIG_REASON_VALUE_METRIC_HIST_UNKNOWN_BINNING_STRATEGY,
                        metric.id());
                break;
            }
        }
        binConfigIt++;
    }
    return binStartsList;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
