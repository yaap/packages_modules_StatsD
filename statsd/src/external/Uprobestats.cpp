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

#include "Log.h"

#include <com_android_os_statsd_flags.h>
#include <dlfcn.h>

#include <string>

#include "src/statsd_config.pb.h"  // Alert

namespace android {
namespace os {
namespace statsd {

namespace {
typedef int (*AUprobestatsClient_startUprobestatsFn)(const uint8_t* config, int64_t size);

const char kLibuprobestatsClientPath[] = "libuprobestats_client.so";

AUprobestatsClient_startUprobestatsFn libInit() {
    if (__builtin_available(android __ANDROID_API_V__, *)) {
        void* handle = dlopen(kLibuprobestatsClientPath, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            ALOGE("dlopen error: %s %s", __func__, dlerror());
            return nullptr;
        }
        auto f = reinterpret_cast<AUprobestatsClient_startUprobestatsFn>(
                dlsym(handle, "AUprobestatsClient_startUprobestats"));
        if (!f) {
            ALOGE("dlsym error: %s %s", __func__, dlerror());
            return nullptr;
        }
        return f;
    }
    return nullptr;
}

namespace flags = com::android::os::statsd::flags;

}  // namespace

bool StartUprobeStats(const UprobestatsDetails& config) {
    if (!flags::trigger_uprobestats()) {
        return false;
    }
    static AUprobestatsClient_startUprobestatsFn AUprobestatsClient_startUprobestats = libInit();
    if (AUprobestatsClient_startUprobestats == nullptr) {
        return false;
    }
    if (!config.has_config()) {
        ALOGE("The uprobestats trace config is empty, aborting");
        return false;
    }
    const std::string& cfgProto = config.config();
    AUprobestatsClient_startUprobestats(reinterpret_cast<const uint8_t*>(cfgProto.c_str()),
                                        cfgProto.length());
    return true;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
