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

#include "StatsSocketListenerIoUring.h"

#include <IOUringSocketHandler/IOUringSocketHandler.h>
#include <android-base/scopeguard.h>
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

#include "StatsSocketListener.h"
#include "guardrail/StatsdStats.h"
#include "logd/logevent_util.h"
#include "stats_log_util.h"
#include "statslog_statsd.h"
#include "utils/api_tracing.h"

using namespace std;

namespace android {
namespace os {
namespace statsd {

StatsSocketListenerIoUring::StatsSocketListenerIoUring(
        const std::shared_ptr<LogEventQueue>& queue,
        const std::shared_ptr<LogEventFilter>& logEventFilter)
    : BaseStatsSocketListener(queue, logEventFilter) {
    VLOG("IoUring Socket Initializing");
    mIoUringSocketHandler = std::make_unique<IOUringSocketHandler>(getLogSocket());
}

int StatsSocketListenerIoUring::startListener() {
    VLOG("IoUring Socket Listener starting");
    if (this->getLogSocket() <= 0) {
        ALOGE("IoUring Socket Listener: Missing socket");
        return -1;
    }

    mThread = std::thread(&StatsSocketListenerIoUring::threadFunction, this);
    VLOG("IoUring Socket Listener started");
    return 0;
}

int StatsSocketListenerIoUring::stopListener() {
    if (mFalledBackToSyncListener) {
        if (gSocketListener == nullptr) {
            ALOGE("StatsSocketListener pointer is NULL when stopping the IoUring version, this "
                  "should not be possible");
            StatsdStats::getInstance().noteIllegalState(
                    COUNTER_TYPE_ERROR_STOP_IOURING_LISTENER_SYNC_LISTENER_NULLPTR);
            return -1;
        }
        gSocketListener->stopListener();
        return 0;
    }
    mShouldStopThreadFunction = true;
    if (mThread.joinable()) {
        mThread.join();
    }
    return 0;
}

bool StatsSocketListenerIoUring::initializeIoUring() {
    if (mIoUringSocketHandler == nullptr) {
        ALOGE("IoUring Socket Listener: LibUringUtils is null");
        return false;
    }
    if (!mIoUringSocketHandler->SetupIoUring(MAX_BUFFERS)) {
        ALOGE("IoUring Socket Listener: SetupIoUring failed");
        return false;
    }
    if (!mIoUringSocketHandler->AllocateAndRegisterBuffers(
                MAX_BUFFERS, sizeof(android_log_header_t) + LOGGER_ENTRY_MAX_PAYLOAD + 1)) {
        ALOGE("IoUring Socket Listener: RegisterBuffers failed");
        return false;
    }
    if (!mIoUringSocketHandler->EnqueueMultishotRecvmsg()) {
        ALOGE("IoUring Socket Listener: EnqueueMultishotRecvmsg failed");
        return false;
    }
    return true;
}

void StatsSocketListenerIoUring::startStatsSocketListener() {
    gSocketListener = std::make_unique<StatsSocketListener>(mQueue, mLogEventFilter);
    if (gSocketListener->startListener()) {
        VLOG("IoUring Fallback Socket Initialization failed. Terminating");
        exit(1);
    }
    mFalledBackToSyncListener = true;
}

void StatsSocketListenerIoUring::threadFunction() {
    // If initialization of the IoUring library failed, fall back to the original
    // StatsSocketListener
    VLOG("IoUring Socket Listener started");
    if (!initializeIoUring()) {
        startStatsSocketListener();
        return;
    }

    static bool name_set;
    if (!name_set) {
        prctl(PR_SET_NAME, "statsd.writer");
        name_set = true;
    }

    while (!mShouldStopThreadFunction) {
        void* receivedData = nullptr;
        size_t length = 0;
        struct ucred* credential = nullptr;
        mIoUringSocketHandler->ReceiveData(&receivedData, length, &credential);
        if (!onDataAvailable(receivedData, length, credential)) {
            VLOG("StatsSocketListenerIoUring::threadFunction, onDataAvailable failed");
        }
    }
}

bool StatsSocketListenerIoUring::onDataAvailable(void* buffer, int len, struct ucred* credential) {
    ATRACE_CALL_DEBUG();
    // Release the buffer from here onwards
    auto scope_guard = android::base::make_scope_guard(
            [this]() -> void { mIoUringSocketHandler->ReleaseBuffer(); });

    if (len <= (ssize_t)(sizeof(android_log_header_t))) {
        VLOG("IoUring onDataReceived error: len <= android_log_header_t");
        return false;
    }

    struct ucred fake_cred;
    if (credential == NULL) {
        credential = &fake_cred;
        credential->pid = 0;
        credential->uid = DEFAULT_OVERFLOWUID;
    }

    const uint32_t uid = credential->uid;
    const uint32_t pid = credential->pid;

    // TODO: b/382145446 - Revisit the handling of batch read and call noteBatchSocketRead with
    //  statistics of atoms.
    processSocketMessage(buffer, len, uid, pid);

    return true;
}
}  // namespace statsd
}  // namespace os
}  // namespace android
