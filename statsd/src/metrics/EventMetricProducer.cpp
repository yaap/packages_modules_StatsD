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

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "EventMetricProducer.h"

#include <limits.h>
#include <stdlib.h>

#include "metrics/parsing_utils/metrics_manager_util.h"
#include "stats_log_util.h"
#include "stats_util.h"

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_STRING;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::ProtoOutputStream;
using std::map;
using std::string;
using std::unordered_map;
using std::vector;
using std::shared_ptr;

namespace android {
namespace os {
namespace statsd {

// for StatsLogReport
const int FIELD_ID_ID = 1;
const int FIELD_ID_EVENT_METRICS = 4;
const int FIELD_ID_IS_ACTIVE = 14;
const int FIELD_ID_ESTIMATED_MEMORY_BYTES = 18;
const int FIELD_ID_DATA_CORRUPTED_REASON = 19;

// for EventMetricDataWrapper
const int FIELD_ID_DATA = 1;
// for EventMetricData
const int FIELD_ID_AGGREGATED_ATOM = 4;
// for AggregatedAtomInfo
const int FIELD_ID_ATOM = 1;
const int FIELD_ID_ATOM_TIMESTAMPS = 2;

EventMetricProducer::EventMetricProducer(
        const ConfigKey& key, const EventMetric& metric, const int conditionIndex,
        const vector<ConditionState>& initialConditionCache, const sp<ConditionWizard>& wizard,
        const uint64_t protoHash, const int64_t startTimeNs,
        const wp<ConfigMetadataProvider> configMetadataProvider,
        const unordered_map<int, shared_ptr<Activation>>& eventActivationMap,
        const unordered_map<int, vector<shared_ptr<Activation>>>& eventDeactivationMap,
        const vector<int>& slicedStateAtoms,
        const unordered_map<int, unordered_map<int, int64_t>>& stateGroupMap)
    : MetricProducer(metric.id(), key, startTimeNs, conditionIndex, initialConditionCache, wizard,
                     protoHash, eventActivationMap, eventDeactivationMap, slicedStateAtoms,
                     stateGroupMap, /*splitBucketForAppUpgrade=*/nullopt, configMetadataProvider),
      mSamplingPercentage(metric.sampling_percentage()) {
    if (metric.links().size() > 0) {
        for (const auto& link : metric.links()) {
            Metric2Condition mc;
            mc.conditionId = link.condition();
            translateFieldMatcher(link.fields_in_what(), &mc.metricFields);
            translateFieldMatcher(link.fields_in_condition(), &mc.conditionFields);
            mMetric2ConditionLinks.push_back(mc);
        }
        mConditionSliced = true;
    }

    VLOG("metric %lld created. bucket size %lld start_time: %lld", (long long)mMetricId,
         (long long)mBucketSizeNs, (long long)mTimeBaseNs);
}

EventMetricProducer::~EventMetricProducer() {
    VLOG("~EventMetricProducer() called");
}

optional<InvalidConfigReason> EventMetricProducer::onConfigUpdatedLocked(
        const StatsdConfig& config, const int configIndex, const int metricIndex,
        const vector<sp<AtomMatchingTracker>>& allAtomMatchingTrackers,
        const unordered_map<int64_t, int>& oldAtomMatchingTrackerMap,
        const unordered_map<int64_t, int>& newAtomMatchingTrackerMap,
        const sp<EventMatcherWizard>& matcherWizard,
        const vector<sp<ConditionTracker>>& allConditionTrackers,
        const unordered_map<int64_t, int>& conditionTrackerMap, const sp<ConditionWizard>& wizard,
        const unordered_map<int64_t, int>& metricToActivationMap,
        unordered_map<int, vector<int>>& trackerToMetricMap,
        unordered_map<int, vector<int>>& conditionToMetricMap,
        unordered_map<int, vector<int>>& activationAtomTrackerToMetricMap,
        unordered_map<int, vector<int>>& deactivationAtomTrackerToMetricMap,
        vector<int>& metricsWithActivation) {
    optional<InvalidConfigReason> invalidConfigReason = MetricProducer::onConfigUpdatedLocked(
            config, configIndex, metricIndex, allAtomMatchingTrackers, oldAtomMatchingTrackerMap,
            newAtomMatchingTrackerMap, matcherWizard, allConditionTrackers, conditionTrackerMap,
            wizard, metricToActivationMap, trackerToMetricMap, conditionToMetricMap,
            activationAtomTrackerToMetricMap, deactivationAtomTrackerToMetricMap,
            metricsWithActivation);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    const EventMetric& metric = config.event_metric(configIndex);
    int trackerIndex;
    // Update appropriate indices, specifically mConditionIndex and MetricsManager maps.
    invalidConfigReason = handleMetricWithAtomMatchingTrackers(
            metric.what(), mMetricId, metricIndex, false, allAtomMatchingTrackers,
            newAtomMatchingTrackerMap, trackerToMetricMap, trackerIndex);
    if (invalidConfigReason.has_value()) {
        return invalidConfigReason;
    }

    if (metric.has_condition()) {
        invalidConfigReason = handleMetricWithConditions(
                metric.condition(), mMetricId, metricIndex, conditionTrackerMap, metric.links(),
                allConditionTrackers, mConditionTrackerIndex, conditionToMetricMap);
        if (invalidConfigReason.has_value()) {
            return invalidConfigReason;
        }
    }
    return nullopt;
}

void EventMetricProducer::dropDataLocked(const int64_t dropTimeNs) {
    mAggregatedAtoms.clear();
    resetDataCorruptionFlagsLocked();
    mTotalDataSize = 0;
    StatsdStats::getInstance().noteBucketDropped(mMetricId);
}

void EventMetricProducer::onSlicedConditionMayChangeLocked(bool overallCondition,
                                                           const int64_t eventTime) {
}

std::unique_ptr<std::vector<uint8_t>> serializeProtoLocked(ProtoOutputStream& protoOutput) {
    size_t bufferSize = protoOutput.size();

    std::unique_ptr<std::vector<uint8_t>> buffer(new std::vector<uint8_t>(bufferSize));

    size_t pos = 0;
    sp<android::util::ProtoReader> reader = protoOutput.data();
    while (reader->readBuffer() != NULL) {
        size_t toRead = reader->currentToRead();
        std::memcpy(&((*buffer)[pos]), reader->readBuffer(), toRead);
        pos += toRead;
        reader->move(toRead);
    }

    return buffer;
}

void EventMetricProducer::clearPastBucketsLocked(const int64_t dumpTimeNs) {
    mAggregatedAtoms.clear();
    resetDataCorruptionFlagsLocked();
    mTotalDataSize = 0;
}

void EventMetricProducer::onDumpReportLocked(const int64_t dumpTimeNs,
                                             const bool include_current_partial_bucket,
                                             const bool erase_data, const DumpLatency dumpLatency,
                                             std::set<string>* str_set, std::set<int32_t>& usedUids,
                                             ProtoOutputStream* protoOutput) {
    protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_ID, (long long)mMetricId);
    protoOutput->write(FIELD_TYPE_BOOL | FIELD_ID_IS_ACTIVE, isActiveLocked());
    // Data corrupted reason
    writeDataCorruptedReasons(*protoOutput, FIELD_ID_DATA_CORRUPTED_REASON,
                              mDataCorruptedDueToQueueOverflow != DataCorruptionSeverity::kNone,
                              mDataCorruptedDueToSocketLoss != DataCorruptionSeverity::kNone);
    if (!mAggregatedAtoms.empty()) {
        protoOutput->write(FIELD_TYPE_INT64 | FIELD_ID_ESTIMATED_MEMORY_BYTES,
                           (long long)byteSizeLocked());
    }
    uint64_t protoToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_EVENT_METRICS);
    for (const auto& [atomDimensionKey, elapsedTimestampsNs] : mAggregatedAtoms) {
        uint64_t wrapperToken =
                protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_DATA);

        uint64_t aggregatedToken =
                protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_AGGREGATED_ATOM);

        uint64_t atomToken = protoOutput->start(FIELD_TYPE_MESSAGE | FIELD_ID_ATOM);
        writeFieldValueTreeToStream(atomDimensionKey.getAtomTag(),
                                    atomDimensionKey.getAtomFieldValues().getValues(), mUidFields,
                                    usedUids, protoOutput);
        protoOutput->end(atomToken);
        for (int64_t timestampNs : elapsedTimestampsNs) {
            protoOutput->write(FIELD_TYPE_INT64 | FIELD_COUNT_REPEATED | FIELD_ID_ATOM_TIMESTAMPS,
                               (long long)timestampNs);
        }
        protoOutput->end(aggregatedToken);
        protoOutput->end(wrapperToken);
    }

    protoOutput->end(protoToken);
    if (erase_data) {
        mAggregatedAtoms.clear();
        resetDataCorruptionFlagsLocked();
        mTotalDataSize = 0;
    }
}

void EventMetricProducer::onConditionChangedLocked(const bool conditionMet,
                                                   const int64_t eventTime) {
    VLOG("Metric %lld onConditionChanged", (long long)mMetricId);
    mCondition = conditionMet ? ConditionState::kTrue : ConditionState::kFalse;
}

void EventMetricProducer::onMatchedLogEventInternalLocked(
        const size_t matcherIndex, const MetricDimensionKey& eventKey,
        const ConditionKey& conditionKey, bool condition, const LogEvent& event,
        const map<int, HashableDimensionKey>& statePrimaryKeys) {
    if (!condition) {
        return;
    }

    if (mSamplingPercentage < 100 && !shouldKeepRandomSample(mSamplingPercentage)) {
        return;
    }

    const int64_t elapsedTimeNs = truncateTimestampIfNecessary(event);
    AtomDimensionKey key(event.GetTagId(), HashableDimensionKey(event.getValues()));

    std::vector<int64_t>& aggregatedTimestampsNs = mAggregatedAtoms[key];
    if (aggregatedTimestampsNs.empty()) {
        sp<ConfigMetadataProvider> provider = getConfigMetadataProvider();
        if (provider != nullptr && provider->useV2SoftMemoryCalculation()) {
            mTotalDataSize += getFieldValuesSizeV2(key.getAtomFieldValues().getValues());
        } else {
            mTotalDataSize += getSize(key.getAtomFieldValues().getValues());
        }
    }
    aggregatedTimestampsNs.push_back(elapsedTimeNs);
    mTotalDataSize += sizeof(int64_t);  // Add the size of the event timestamp
}

size_t EventMetricProducer::byteSizeLocked() const {
    sp<ConfigMetadataProvider> provider = getConfigMetadataProvider();
    if (provider != nullptr && provider->useV2SoftMemoryCalculation()) {
        return mTotalDataSize +
               computeOverheadSizeLocked(/*hasPastBuckets=*/false, /*dimensionGuardrailHit=*/false);
    }
    return mTotalDataSize;
}

MetricProducer::DataCorruptionSeverity EventMetricProducer::determineCorruptionSeverity(
        int32_t /*atomId*/, DataCorruptedReason reason, LostAtomType atomType) const {
    switch (atomType) {
        case LostAtomType::kWhat:
            return DataCorruptionSeverity::kResetOnDump;
        case LostAtomType::kCondition:
            return DataCorruptionSeverity::kUnrecoverable;
        case LostAtomType::kState:
            break;
    };
    return DataCorruptionSeverity::kNone;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
