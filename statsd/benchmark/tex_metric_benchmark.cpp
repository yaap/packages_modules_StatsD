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
const int numOfMetrics = 4;

vector<shared_ptr<LogEvent>> createExpressEventReportedEvents() {
    vector<shared_ptr<LogEvent>> events;
    for (int i = 0; i < numOfMetrics; i++) {
        events.push_back(CreateTwoValueLogEvent(/* atomId */ util::EXPRESS_EVENT_REPORTED,
                                                /* eventTimeNs */ i,
                                                /* metric_id */ i % numOfMetrics, /* value */ 1));
    }
    return events;
}

AtomMatcher CreateExpressEventReportedAtomMatcher(const string& name, int64_t metricIdHash) {
    AtomMatcher atom_matcher = CreateSimpleAtomMatcher(name, util::EXPRESS_EVENT_REPORTED);
    auto simple_atom_matcher = atom_matcher.mutable_simple_atom_matcher();
    auto field_value_matcher = simple_atom_matcher->add_field_value_matcher();
    field_value_matcher->set_field(1);  // metric id hash as int64
    field_value_matcher->set_eq_int(metricIdHash);
    return atom_matcher;
}

StatsdConfig createTexConfig() {
    StatsdConfig config;
    *config.add_atom_matcher() = CreateExpressEventReportedAtomMatcher("texMatcher1", 0);
    *config.add_atom_matcher() = CreateExpressEventReportedAtomMatcher("texMatcher2", 1);
    *config.add_atom_matcher() = CreateExpressEventReportedAtomMatcher("texMatcher3", 2);
    *config.add_atom_matcher() = CreateExpressEventReportedAtomMatcher("texMatcher4", 3);

    *config.add_value_metric() =
            createValueMetric("texValue1", config.atom_matcher(0), /* valueField */ 2,
                              /* condition */ nullopt, /* states */ {});
    *config.add_value_metric() =
            createValueMetric("texValue2", config.atom_matcher(1), /* valueField */ 2,
                              /* condition */ nullopt, /* states */ {});
    *config.add_value_metric() =
            createValueMetric("texValue3", config.atom_matcher(2), /* valueField */ 2,
                              /* condition */ nullopt, /* states */ {});
    *config.add_value_metric() =
            createValueMetric("texValue4", config.atom_matcher(3), /* valueField */ 2,
                              /* condition */ nullopt, /* states */ {});
    return config;
}

StatsdConfig createCountMetricConfig() {
    StatsdConfig config;
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("someCounterMatcher", /*atomId*/ util::EXPRESS_EVENT_REPORTED);

    CountMetric* countMetric = config.add_count_metric();
    *countMetric = createCountMetric("CountMetricAsCounter", /* what */ config.atom_matcher(0).id(),
                                     /* condition */ nullopt, /* states */ {});
    countMetric->mutable_dimensions_in_what()->set_field(util::EXPRESS_EVENT_REPORTED);
    countMetric->mutable_dimensions_in_what()->add_child()->set_field(1);
    return config;
}

StatsdConfig createValueMetricConfig() {
    StatsdConfig config;
    *config.add_atom_matcher() =
            CreateSimpleAtomMatcher("someValueMatcher", /*atomId*/ util::EXPRESS_EVENT_REPORTED);

    ValueMetric* valueMetric = config.add_value_metric();
    *valueMetric = createValueMetric("ValueMetricAsCounter", /* what */ config.atom_matcher(0),
                                     /* valueField */ 2,
                                     /* condition */ nullopt,
                                     /* states */ {});
    valueMetric->mutable_dimensions_in_what()->set_field(util::EXPRESS_EVENT_REPORTED);
    valueMetric->mutable_dimensions_in_what()->add_child()->set_field(1);
    return config;
}

void testScenario(benchmark::State& state, sp<StatsLogProcessor>& processor) {
    const int64_t elevenMinutesInNanos = NS_PER_SEC * 60 * 11;
    vector<shared_ptr<LogEvent>> events = createExpressEventReportedEvents();
    state.counters["MetricsSize"] = processor->GetMetricsSize(cfgKey);
    int64_t eventIndex = 0;
    for (auto _ : state) {
        for (int i = 0; i < 1000; i++) {
            auto event = events[eventIndex % numOfMetrics].get();
            event->setElapsedTimestampNs(eventIndex * 10);
            processor->OnLogEvent(event);
            benchmark::DoNotOptimize(processor);
            eventIndex++;
        }
    }
    vector<uint8_t> buffer;
    processor->onDumpReport(cfgKey, elevenMinutesInNanos, true, false, ADB_DUMP, FAST, &buffer);
    state.counters["ReportBufferSize"] = buffer.size();
    state.counters["MetricsSizeFinal"] = processor->GetMetricsSize(cfgKey);
}

void BM_TexCounter(benchmark::State& state) {
    // idea is to have 1 standalone value metric with dimensions to mimic 4 tex metrics
    // and compare performance - see BM_TexCounterAsValueMetric
    StatsdConfig config = createTexConfig();
    sp<StatsLogProcessor> processor = CreateStatsLogProcessor(1, 1, config, cfgKey);
    testScenario(state, processor);
}
BENCHMARK(BM_TexCounter);

void BM_TexCounterAsValueMetric(benchmark::State& state) {
    // idea is to have 1 standalone value metric with dimensions to mimic 4 tex metrics
    // and compare performance - see BM_TexCounter
    StatsdConfig config = createValueMetricConfig();
    sp<StatsLogProcessor> processor = CreateStatsLogProcessor(1, 1, config, cfgKey);
    testScenario(state, processor);
}
BENCHMARK(BM_TexCounterAsValueMetric);

void BM_TexCounterAsCountMetric(benchmark::State& state) {
    // idea is to have 1 standalone count metric with dimensions to mimic 4 tex metrics
    // and compare performance - see BM_TexCounter
    StatsdConfig config = createCountMetricConfig();
    sp<StatsLogProcessor> processor = CreateStatsLogProcessor(1, 1, config, cfgKey);
    testScenario(state, processor);
}
BENCHMARK(BM_TexCounterAsCountMetric);

}  // anonymous namespace
}  // namespace statsd
}  // namespace os
}  // namespace android
