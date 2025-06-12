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
    : BaseStatsSocketListener(queue, logEventFilter),
      SocketListener(getLogSocket(), false /*start listen*/){
}

bool StatsSocketListener::onDataAvailable(SocketClient* cli) {
    ATRACE_CALL_DEBUG();
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
                processSocketMessage(buffer, n, uid, pid);
        mAtomCounts[atomId]++;
        minAtomReadTime = min(minAtomReadTime, atomTimeNs);
        maxAtomReadTime = max(maxAtomReadTime, atomTimeNs);
    }

    noteBatchSocketRead(i, mLastSocketReadTimeNs, elapsedTimeNs, minAtomReadTime, maxAtomReadTime,
                        mAtomCounts);
    return true;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
