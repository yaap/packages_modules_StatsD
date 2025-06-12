/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef EVENT_METRIC_PRODUCER_H
#define EVENT_METRIC_PRODUCER_H

#include <unordered_map>

#include <android/util/ProtoOutputStream.h>

#include "../condition/ConditionTracker.h"
#include "../matchers/matcher_util.h"
#include "HashableDimensionKey.h"
#include "MetricProducer.h"
#include "src/statsd_config.pb.h"
#include "stats_util.h"

namespace android {
namespace os {
namespace statsd {

class EventMetricProducer : public MetricProducer {
public:
    EventMetricProducer(
            const ConfigKey& key, const EventMetric& eventMetric, int conditionIndex,
            const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
            const uint64_t protoHash, int64_t startTimeNs,
            const wp<ConfigMetadataProvider> configMetadataProvider,
            const std::unordered_map<int, std::shared_ptr<Activation>>& eventActivationMap = {},
            const std::unordered_map<int, std::vector<std::shared_ptr<Activation>>>&
                    eventDeactivationMap = {},
            const vector<int>& slicedStateAtoms = {},
            const unordered_map<int, unordered_map<int, int64_t>>& stateGroupMap = {});

    virtual ~EventMetricProducer();

    MetricType getMetricType() const override {
        return METRIC_TYPE_EVENT;
    }

    void onStateChanged(const int64_t eventTimeNs, const int32_t atomId,
                        const HashableDimensionKey& primaryKey, const FieldValue& oldState,
                        const FieldValue& newState) override;

private:
    void onMatchedLogEventInternalLocked(
            const size_t matcherIndex, const MetricDimensionKey& eventKey,
            const ConditionKey& conditionKey, bool condition, const LogEvent& event,
            const std::map<int, HashableDimensionKey>& statePrimaryKeys) override;

    void onDumpReportLocked(const int64_t dumpTimeNs, const bool include_current_partial_bucket,
                            const bool erase_data, const DumpLatency dumpLatency,
                            std::set<string>* str_set, std::set<int32_t>& usedUids,
                            android::util::ProtoOutputStream* protoOutput) override;
    void clearPastBucketsLocked(const int64_t dumpTimeNs) override;

    // Internal interface to handle condition change.
    void onConditionChangedLocked(const bool conditionMet, int64_t eventTime) override;

    // Internal interface to handle sliced condition change.
    void onSlicedConditionMayChangeLocked(bool overallCondition, int64_t eventTime) override;

    optional<InvalidConfigReason> onConfigUpdatedLocked(
            const StatsdConfig& config, int configIndex, int metricIndex,
            const std::vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
            const std::unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
            const std::unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
            const sp<EventMatcherWizard>& matcherWizard,
            const std::vector<sp<ConditionTracker>>& allConditionTrackers,
            const std::unordered_map<int64_t, int>& conditionTrackerMap,
            const sp<ConditionWizard>& wizard,
            const std::unordered_map<int64_t, int>& metricToActivationMap,
            std::unordered_map<int, std::vector<int>>& trackerToMetricMap,
            std::unordered_map<int, std::vector<int>>& conditionToMetricMap,
            std::unordered_map<int, std::vector<int>>& activationAtomTrackerToMetricMap,
            std::unordered_map<int, std::vector<int>>& deactivationAtomTrackerToMetricMap,
            std::vector<int>& metricsWithActivation) override;

    void dropDataLocked(const int64_t dropTimeNs) override;

    // Internal function to calculate the current used bytes.
    size_t byteSizeLocked() const override;

    void dumpStatesLocked(int out, bool verbose) const override{};

    DataCorruptionSeverity determineCorruptionSeverity(int32_t atomId, DataCorruptedReason reason,
                                                       LostAtomType atomType) const override;

    // Maps the field/value pairs of an atom to a list of timestamps used to deduplicate atoms.
    // Used when event metric DOES NOT use slice_by_state. Empty otherwise.
    std::unordered_map<AtomDimensionKey, std::vector<int64_t>> mAggregatedAtoms;

    // Maps the field/value pairs of an atom to the field/value pairs of a state to a list of
    // timestamps used to deduplicate atoms and states.
    // Used when event metric DOES use slice_by_state. Empty otherwise.
    std::unordered_map<AtomDimensionKey,
                       std::unordered_map<HashableDimensionKey, std::vector<int64_t>>>
            mAggAtomsAndStates;

    const int mSamplingPercentage;

    // Allowlist/denylist of fields to report. Empty means all are reported.
    // If mOmitFields == true, this is a denylist, otherwise it's an allowlist.
    const std::vector<Matcher> mFieldMatchers;
    const bool mOmitFields;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
#endif  // EVENT_METRIC_PRODUCER_H
