/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "subscriber_util.h"

#include <algorithm>
#include <limits>

#include "external/Perfetto.h"
#include "external/Uprobestats.h"
#include "subscriber/IncidentdReporter.h"
#include "subscriber/SubscriberReporter.h"

namespace android {
namespace os {
namespace statsd {

int64_t doubleToInt64Clamped(double value) {
    double min_int64 = static_cast<double>(std::numeric_limits<int64_t>::min());
    double max_int64 = static_cast<double>(std::numeric_limits<int64_t>::max());

    double clamped_double = std::clamp(value, min_int64, max_int64);

    return static_cast<int64_t>(clamped_double);
}

void triggerSubscribers(const int64_t ruleId, const int64_t metricId,
                        const MetricDimensionKey& dimensionKey, double metricValue,
                        const ConfigKey& configKey,
                        const std::vector<Subscription>& subscriptions) {
    VLOG("informSubscribers called.");
    if (subscriptions.empty()) {
        VLOG("No Subscriptions were associated.");
        return;
    }

    for (const Subscription& subscription : subscriptions) {
        if (subscription.probability_of_informing() < 1
                && ((float)rand() / (float)RAND_MAX) >= subscription.probability_of_informing()) {
            // Note that due to float imprecision, 0.0 and 1.0 might not truly mean never/always.
            // The config writer was advised to use -0.1 and 1.1 for never/always.
            ALOGI("Fate decided that a subscriber would not be informed.");
            continue;
        }
        switch (subscription.subscriber_information_case()) {
            case Subscription::SubscriberInformationCase::kIncidentdDetails:
                // incidentd is on the deprecation path. clamp the double
                // value instead of creating new fields for double.
                if (!GenerateIncidentReport(subscription.incidentd_details(), ruleId, metricId,
                                            dimensionKey, doubleToInt64Clamped(metricValue),
                                            configKey)) {
                    ALOGW("Failed to generate incident report.");
                }
                break;
            case Subscription::SubscriberInformationCase::kPerfettoDetails:
                if (!CollectPerfettoTraceAndUploadToDropbox(subscription.perfetto_details(),
                                                            subscription.id(), ruleId, configKey)) {
                    ALOGW("Failed to generate perfetto traces.");
                }
                break;
            case Subscription::SubscriberInformationCase::kUprobestatsDetails:
                if (!StartUprobeStats(subscription.uprobestats_details())) {
                    ALOGW("Failed to start uprobestats.");
                }
                break;
            case Subscription::SubscriberInformationCase::kBroadcastSubscriberDetails:
                SubscriberReporter::getInstance().alertBroadcastSubscriber(configKey, subscription,
                                                                           dimensionKey);
                break;
            default:
                break;
        }
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android
