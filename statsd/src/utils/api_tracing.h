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

#pragma once

#define ATRACE_TAG ATRACE_TAG_APP

#include <utils/Trace.h>

#include <string>

#include "stats_log_util.h"

// Use the local value to turn on/off atrace logs.
// The advantage is that in production compiler can remove the logging code if the local
// STATSD_DEBUG/VERBOSE is false.
#define ATRACE_CALL_DEBUG(...) \
    if (STATSD_DEBUG) ATRACE_CALL(__VA_ARGS__);

#define TIME_CALL_NAME(name) ScopedTimingTrace PASTE(___timer, __LINE__)(name)

// TIME_CALL is a TIME_CALL_NAME that uses the current function name.
#define TIME_CALL() TIME_CALL_NAME(__FUNCTION__)

#define TIME_CALL_DEBUG(...) \
    if (STATSD_DEBUG) TIME_CALL(__VA_ARGS__);

class ScopedTimingTrace {
    int64_t mStartTimeNs;
    std::string mTag;

public:
    inline ScopedTimingTrace(const char* name) : mTag(name) {
        mStartTimeNs = ::android::os::statsd::getElapsedRealtimeNs();
    }

    ~ScopedTimingTrace() {
        ALOGI("%s duration %d ns", mTag.c_str(),
              (int)(::android::os::statsd::getElapsedRealtimeNs() - mStartTimeNs));
    }
};
