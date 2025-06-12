/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "BaseStatsSocketListener.h"

#include <ctype.h>
#include <cutils/sockets.h>
#include <limits.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "guardrail/StatsdStats.h"
#include "android-base/scopeguard.h"
#include "logd/logevent_util.h"
#include "stats_log_util.h"
#include "statslog_statsd.h"
#include "utils/api_tracing.h"

using namespace std;

namespace android {
namespace os {
namespace statsd {
BaseStatsSocketListener::BaseStatsSocketListener(const std::shared_ptr<LogEventQueue>& queue,
                        const std::shared_ptr<LogEventFilter>& logEventFilter)
                        : mQueue(queue),
                          mLogEventFilter(logEventFilter){};

tuple<int32_t, int64_t> BaseStatsSocketListener::processSocketMessage(void* buffer,
                                                                  const uint32_t len, uint32_t uid,
                                                                  uint32_t pid) {
    ATRACE_CALL_DEBUG();
    static const uint32_t kStatsEventTag = 1937006964;

    if (len <= (ssize_t)(sizeof(android_log_header_t)) + sizeof(uint32_t)) {
        return {-1, 0};
    }

    const uint8_t* ptr = ((uint8_t*)buffer) + sizeof(android_log_header_t);
    uint32_t bufferLen = len - sizeof(android_log_header_t);

    // When a log failed to write to statsd socket (e.g., due ot EBUSY), a special message would
    // be sent to statsd when the socket communication becomes available again.
    // The format is android_log_event_int_t with a single integer in the payload indicating the
    // number of logs that failed. (*FORMAT MUST BE IN SYNC WITH system/core/libstats*)
    // Note that all normal stats logs are in the format of event_list, so there won't be confusion.
    //
    // TODO(b/80538532): In addition to log it in StatsdStats, we should properly reset the config.
    if (bufferLen == sizeof(android_log_event_long_t)) {
        const android_log_event_long_t* long_event =
                reinterpret_cast<const android_log_event_long_t*>(ptr);
        if (long_event->payload.type == EVENT_TYPE_LONG) {
            int64_t composed_long = long_event->payload.data;

            // format:
            // |last_tag|dropped_count|
            int32_t dropped_count = (int32_t)(0xffffffff & composed_long);
            int32_t last_atom_tag = (int32_t)((0xffffffff00000000 & (uint64_t)composed_long) >> 32);

            ALOGE("Found dropped events: %d error %d last atom tag %d from uid %d", dropped_count,
                  long_event->header.tag, last_atom_tag, uid);
            StatsdStats::getInstance().noteLogLost((int32_t)getWallClockSec(), dropped_count,
                                                   long_event->header.tag, last_atom_tag, uid, pid);
            return {-1, 0};
        }
    }

    // test that received valid StatsEvent buffer
    const uint32_t statsEventTag = *reinterpret_cast<const uint32_t*>(ptr);
    if (statsEventTag != kStatsEventTag) {
        return {-1, 0};
    }

    // move past the 4-byte StatsEventTag
    const uint8_t* msg = ptr + sizeof(uint32_t);
    bufferLen -= sizeof(uint32_t);

    return processStatsEventBuffer(msg, bufferLen, uid, pid, *mQueue, *mLogEventFilter);
}

tuple<int32_t, int64_t> BaseStatsSocketListener::processStatsEventBuffer(const uint8_t* msg,
                                                                     const uint32_t len,
                                                                     uint32_t uid, uint32_t pid,
                                                                     LogEventQueue& queue,
                                                                     const LogEventFilter& filter) {
    ATRACE_CALL_DEBUG();
    std::unique_ptr<LogEvent> logEvent = std::make_unique<LogEvent>(uid, pid);

    if (filter.getFilteringEnabled()) {
        const LogEvent::BodyBufferInfo bodyInfo = logEvent->parseHeader(msg, len);
        if (filter.isAtomInUse(logEvent->GetTagId())) {
            logEvent->parseBody(bodyInfo);
        }
    } else {
        logEvent->parseBuffer(msg, len);
    }

    const int32_t atomId = logEvent->GetTagId();
    const bool isAtomSkipped = logEvent->isParsedHeaderOnly();
    const int64_t atomTimestamp = logEvent->GetElapsedTimestampNs();

    // Tell StatsdStats about new event
    StatsdStats::getInstance().noteAtomLogged(atomId, atomTimestamp, isAtomSkipped);

    if (atomId == util::STATS_SOCKET_LOSS_REPORTED) {
        if (isAtomSkipped) {
            ALOGW("Atom STATS_SOCKET_LOSS_REPORTED should not be skipped");
        }

        // handling socket loss info reported atom
        // processing it here to not lose info due to queue overflow
        const std::optional<SocketLossInfo>& lossInfo = toSocketLossInfo(*logEvent);
        if (lossInfo) {
            StatsdStats::getInstance().noteAtomSocketLoss(*lossInfo);
        } else {
            ALOGW("Atom STATS_SOCKET_LOSS_REPORTED content is invalid");
        }
    }

    const auto [success, oldestTimestamp, queueSize] = queue.push(std::move(logEvent));
    if (success) {
        StatsdStats::getInstance().noteEventQueueSize(queueSize, atomTimestamp);
    } else {
        StatsdStats::getInstance().noteEventQueueOverflow(oldestTimestamp, atomId);
    }
    return {atomId, atomTimestamp};
}

int BaseStatsSocketListener::getLogSocket() {
    static const char socketName[] = "statsdw";
    int sock = android_get_control_socket(socketName);

    if (sock < 0) {  // statsd started up in init.sh
        sock = socket_local_server(socketName, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_DGRAM);

        int on = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on))) {
            return -1;
        }
    }
    return sock;
}

void BaseStatsSocketListener::noteBatchSocketRead(int32_t size, int64_t lastReadTimeNs,
                                          int64_t currReadTimeNs, int64_t minAtomReadTimeNs,
                                          int64_t maxAtomReadTimeNs,
                                          const std::unordered_map <int32_t, int32_t> &atomCounts) {
    StatsdStats::getInstance().noteBatchSocketRead(size, lastReadTimeNs, currReadTimeNs,
                                               minAtomReadTimeNs, maxAtomReadTimeNs, atomCounts);
    mLastSocketReadTimeNs = currReadTimeNs;
    mAtomCounts.clear();
}

}  // namespace statsd
}  // namespace os
}  // namespace android
