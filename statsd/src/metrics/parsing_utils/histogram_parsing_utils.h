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

#include <optional>
#include <variant>
#include <vector>

#include "guardrail/StatsdStats.h"
#include "src/statsd_config.pb.h"
#include "stats_util.h"

namespace android {
namespace os {
namespace statsd {

constexpr float UNDERFLOW_BIN_START = std::numeric_limits<float>::min();

using ParseHistogramBinConfigsResult =
        std::variant<std::vector<std::optional<const BinStarts>>, InvalidConfigReason>;

ParseHistogramBinConfigsResult parseHistogramBinConfigs(
        const ValueMetric& valueMetric,
        const std::vector<ValueMetric::AggregationType>& aggregationTypes);

}  // namespace statsd
}  // namespace os
}  // namespace android
