/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <benchmark/benchmark.h>

#include "state/StateManager.h"
#include "tests/statsd_test_util.h"

namespace android {
namespace os {
namespace statsd {

namespace {

class DummyStateListener : public StateListener {
public:
    void onStateChanged(const int64_t /*eventTimeNs*/, const int32_t /*atomId*/,
                        const HashableDimensionKey& /*primaryKey*/, const FieldValue& /*oldState*/,
                        const FieldValue& /*newState*/) override {
    }
    void onStateEventLost(int32_t /*atomId*/, DataCorruptedReason /*reason*/) override {
    }
};

}  // anonymous namespace

static void BM_StateManagerGetStateValue_Global(benchmark::State& state) {
    sp<DummyStateListener> listener = new DummyStateListener();
    int32_t atomId = util::SCREEN_STATE_CHANGED;
    StateManager::getInstance().registerListener(atomId, listener);

    auto event = CreateScreenStateChangedEvent(123, android::view::DISPLAY_STATE_ON);
    StateManager::getInstance().onLogEvent(*event);

    for (auto _ : state) {
        FieldValue output =
                StateManager::getInstance().getStateValue(atomId, DEFAULT_DIMENSION_KEY);
        benchmark::DoNotOptimize(output);
    }
}
BENCHMARK(BM_StateManagerGetStateValue_Global);

static void BM_StateManagerGetStateValue_Primary(benchmark::State& state) {
    sp<DummyStateListener> listener = new DummyStateListener();
    int32_t atomId = util::WAKELOCK_STATE_CHANGED;
    StateManager::getInstance().registerListener(atomId, listener);

    std::vector<int> uids = {1000};
    std::vector<std::string> tags = {"tag"};
    auto event = CreateAcquireWakelockEvent(123, uids, tags, "wl");
    StateManager::getInstance().onLogEvent(*event);

    HashableDimensionKey primaryKey;
    filterPrimaryKey(event->getValues(), &primaryKey);

    for (auto _ : state) {
        FieldValue output = StateManager::getInstance().getStateValue(atomId, primaryKey);
        benchmark::DoNotOptimize(output);
    }
}
BENCHMARK(BM_StateManagerGetStateValue_Primary);

}  // namespace statsd
}  // namespace os
}  // namespace android
