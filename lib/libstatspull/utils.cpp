/*
 * Copyright (C) 2024, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "utils.h"

#include <android/binder_manager.h>
#include <com_android_os_statsd_flags.h>

namespace flags = com::android::os::statsd::flags;

ndk::SpAIBinder getStatsdBinder() {
    ndk::SpAIBinder binder;
    // below ifs cannot be combined into single statement due to the way how
    // macro __builtin_available is handler by compiler:
    // - it should be used explicitly & independently to guard the corresponding API call
    // once use_wait_for_service_api flag will be finalized, external if/else pair will be
    // removed
#ifdef __ANDROID__
    if (flags::use_wait_for_service_api()) {
        if (__builtin_available(android __ANDROID_API_S__, *)) {
            binder.set(AServiceManager_waitForService("stats"));
        } else {
            binder.set(AServiceManager_getService("stats"));
        }
    } else {
        binder.set(AServiceManager_getService("stats"));
    }
#endif  //  __ANDROID__
    return binder;
}