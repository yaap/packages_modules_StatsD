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

#include "StatsSocketListener.h"

#include <ctype.h>
#include <cutils/sockets.h>
#include <limits.h>
#include <stdio.h>
#include <sys/cdefs.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "guardrail/StatsdStats.h"
#include "logd/logevent_util.h"
#include "stats_log_util.h"
#include "statslog_statsd.h"
#include "utils/api_tracing.h"

using namespace std;

namespace android {
namespace os {
namespace statsd {

StatsSocketListener::StatsSocketListener(const std::shared_ptr<LogEventQueue>& queue,
                                         const std::shared_ptr<LogEventFilter>& logEventFilter)
    : SocketListener(getLogSocket(), false /*start listen*/),
      mQueue(queue),
      mLogEventFilter(logEventFilter),
      mLastSocketReadTimeNs(0) {
}

bool StatsSocketListener::onDataAvailable(SocketClient* cli) {
    ATRACE_CALL();
    static bool name_set;
    if (!name_set) {
        prctl(PR_SET_NAME, "statsd.writer");
        name_set = true;
    }

    int64_t elapsedTimeNs = getElapsedRealtimeNs();
    // + 1 to ensure null terminator if MAX_PAYLOAD buffer is received
    char buffer[sizeof(android_log_header_t) + LOGGER_ENTRY_MAX_PAYLOAD + 1];
    struct iovec iov = {buffer, sizeof(buffer) - 1};

    alignas(4) char control[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr hdr = {
            NULL, 0, &iov, 1, control, sizeof(control), 0,
    };

    const int socket = cli->getSocket();
    int i = 0;
    int64_t minAtomReadTime = INT64_MAX;
    int64_t maxAtomReadTime = -1;
    mAtomCounts.clear();
    ssize_t n = 0;
    while (n = recvmsg(socket, &hdr, MSG_DONTWAIT), n > 0) {
        // To clear the entire buffer is secure/safe, but this contributes to 1.68%
        // overhead under logging load. We are safe because we check counts, but
        // still need to clear null terminator.
        // Note that the memset, if needed, should happen before each read in the while loop.
        // memset(buffer, 0, sizeof(buffer));
        if (n <= (ssize_t)(sizeof(android_log_header_t))) {
            return false;
        }
        buffer[n] = 0;
        i++;

        struct ucred* cred = NULL;

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
        while (cmsg != NULL) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_CREDENTIALS) {
                cred = (struct ucred*)CMSG_DATA(cmsg);
                break;
            }
            cmsg = CMSG_NXTHDR(&hdr, cmsg);
        }

        struct ucred fake_cred;
        if (cred == NULL) {
            cred = &fake_cred;
            cred->pid = 0;
            cred->uid = DEFAULT_OVERFLOWUID;
        }

        const uint32_t uid = cred->uid;
        const uint32_t pid = cred->pid;

        auto [atomId, atomTimeNs] =
                processSocketMessage(buffer, n, uid, pid, *mQueue, *mLogEventFilter);
        mAtomCounts[atomId]++;
        minAtomReadTime = min(minAtomReadTime, atomTimeNs);
        maxAtomReadTime = max(maxAtomReadTime, atomTimeNs);
    }

    StatsdStats::getInstance().noteBatchSocketRead(i, mLastSocketReadTimeNs, elapsedTimeNs,
                                                   minAtomReadTime, maxAtomReadTime, mAtomCounts);
    mLastSocketReadTimeNs = elapsedTimeNs;
    mAtomCounts.clear();
    return true;
}

tuple<int32_t, int64_t> StatsSocketListener::processSocketMessage(const char* buffer,
                                                                  const uint32_t len, uint32_t uid,
                                                                  uint32_t pid,
                                                                  LogEventQueue& queue,
                                                                  const LogEventFilter& filter) {
    ATRACE_CALL();
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

    return processStatsEventBuffer(msg, bufferLen, uid, pid, queue, filter);
}

tuple<int32_t, int64_t> StatsSocketListener::processStatsEventBuffer(const uint8_t* msg,
                                                                     const uint32_t len,
                                                                     uint32_t uid, uint32_t pid,
                                                                     LogEventQueue& queue,
                                                                     const LogEventFilter& filter) {
    ATRACE_CALL();
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
        StatsdStats::getInstance().noteEventQueueOverflow(oldestTimestamp, atomId, isAtomSkipped);
    }
    return {atomId, atomTimestamp};
}

int StatsSocketListener::getLogSocket() {
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

}  // namespace statsd
}  // namespace os
}  // namespace android
