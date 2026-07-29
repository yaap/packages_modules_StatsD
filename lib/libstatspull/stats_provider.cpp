/*
 * Copyright (C) 2023, The Android Open Source Project
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

#include <android/binder_manager.h>
#include <stats_provider.h>

#include "utils.h"

using aidl::android::os::IStatsd;

#ifndef __ANDROID_API_T__
#define __ANDROID_API_T__ 33
#endif

struct StatsProviderState {
    android::sp<StatsProvider> statsProvider;
};

static void StateDelete(void* cookie) {
    delete reinterpret_cast<StatsProviderState*>(cookie);
}

StatsProvider::StatsProvider(StatsProviderBinderDiedCallback callback)
    : mDeathRecipient(AIBinder_DeathRecipient_new(binderDied)), mCallback(callback) {
}

StatsProvider::~StatsProvider() {
    resetStatsService();
}

std::shared_ptr<IStatsd> StatsProvider::getStatsService() {
    std::lock_guard lock(mMutex);
    if (!mStatsd) {
        // Fetch statsd
        ::ndk::SpAIBinder binder(getStatsdBinder());
        mStatsd = IStatsd::fromBinder(binder);
        if (mStatsd) {
            // it is ok to leak single pointer on past releases
            if (__builtin_available(android __ANDROID_API_T__, *)) {
                AIBinder_DeathRecipient_setOnUnlinked(mDeathRecipient.get(), StateDelete);
            }
            AIBinder_linkToDeath(binder.get(), mDeathRecipient.get(), new StatsProviderState(this));
        }
    }
    return mStatsd;
}

void StatsProvider::resetStatsService() {
    std::lock_guard lock(mMutex);
    mStatsd = nullptr;
}

void StatsProvider::binderDied(void* cookie) {
    StatsProviderState* statsProviderState = static_cast<StatsProviderState*>(cookie);
    statsProviderState->statsProvider->resetStatsService();
    statsProviderState->statsProvider->mCallback();
}
