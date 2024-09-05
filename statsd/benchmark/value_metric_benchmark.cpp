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

#include <memory>

#include "benchmark/benchmark.h"
#include "src/statsd_config.pb.h"
#include "tests/statsd_test_util.h"

using namespace std;

namespace android {
namespace os {
namespace statsd {
namespace {

const ConfigKey cfgKey(0, 12345);

vector<shared_ptr<LogEvent>> createEvents() {
    vector<shared_ptr<LogEvent>> events;
    for (int i = 1; i <= 1000; i++) {
        events.push_back(CreateTwoValueLogEvent(/* atomId */ 1, /* eventTimeNs */ i,
                                                /* value1 */ i % 50, /* value2 */ i));
    }
    return events;
}

StatsdConfig createConfig() {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateSimpleAtomMatcher("matcher", /* atomId */ 1);

    *config.add_value_metric() =
            createValueMetric("Value", config.atom_matcher(0), /* valueField */ 2,
                              /* condition */ nullopt, /* states */ {});
    config.mutable_value_metric(0)->mutable_dimensions_in_what()->set_field(1);
    config.mutable_value_metric(0)->mutable_dimensions_in_what()->add_child()->set_field(1);
    config.mutable_value_metric(0)->set_use_diff(true);

    return config;
}

void BM_ValueMetricPushedDiffedViaStatsLogProcessor(benchmark::State& state) {
    StatsdConfig config = createConfig();
    sp<StatsLogProcessor> processor = CreateStatsLogProcessor(1, 1, config, cfgKey);
    vector<shared_ptr<LogEvent>> events = createEvents();

    for (auto _ : state) {
        for (const auto& event : events) {
            processor->OnLogEvent(event.get());
        }
    }
}
BENCHMARK(BM_ValueMetricPushedDiffedViaStatsLogProcessor);

void BM_ValueMetricPushedDiffedViaNumericValueMetricProducer(benchmark::State& state) {
    StatsdConfig config = createConfig();
    sp<MockStatsPullerManager> pullerManager = new StrictMock<MockStatsPullerManager>();
    sp<NumericValueMetricProducer> producer = createNumericValueMetricProducer(
            pullerManager, config.value_metric(0), /* atomId */ 1, /* isPulled */ false, cfgKey,
            /* protoHash */ 0x123456, /* timeBaseNs */ 100, /* startTimeNs */ 100,
            /* logEventMatcherIndex */ 0);

    vector<shared_ptr<LogEvent>> events = createEvents();

    for (auto _ : state) {
        for (const auto& event : events) {
            producer->onMatchedLogEvent(/* matcherIndex */ 0, *event);
        }
    }
}
BENCHMARK(BM_ValueMetricPushedDiffedViaNumericValueMetricProducer);

}  // anonymous namespace
}  // namespace statsd
}  // namespace os
}  // namespace android
