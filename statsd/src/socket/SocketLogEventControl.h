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

#pragma once

#include <StatsdLoggingControl.h>
#include <com_android_os_statsd_flags.h>
#include <gtest/gtest_prod.h>

#include <mutex>
#include <vector>

#include "LogEventFilterUtils.h"
#include "socket/AtomsInUseChangeListener.h"
#include "socket/AtomsInUseListProducer.h"
#include "stats_util.h"

namespace android {
namespace os {
namespace statsd {

namespace flags = com::android::os::statsd::flags;

/**
 * Configures socket logging control based on list af atom ids in use
 */
class SocketLogEventControl : public AtomsInUseChangeListener {
public:
    SocketLogEventControl(std::string fileName = kAtomIdsFileName,
                          std::string versionPropertyName = kAtomIdsVersionName)
        : mAtomsInUseListProducer(fileName, versionPropertyName) {
    }

    virtual ~SocketLogEventControl() = default;

    void setControlEnabled(bool isEnabled) {
        if (!isSupported()) {
            return;
        }

        std::lock_guard lock(mTagIdsMutex);
        mIsEnabled = isEnabled;
        if (mIsEnabled) {
            setLoggingConfigLocked(mAtomIdSetManager.getAtomIds());
        } else {
            // to allow clients to log any atom
            resetConfigLocked();
        }
    }

    void setAtomIds(AtomIdSet tagIds, ConsumerId consumer) override {
        if (!isSupported()) {
            return;
        }

        std::lock_guard lock(mTagIdsMutex);
        mAtomIdSetManager.setAtomIds(tagIds, consumer);
        if (mIsEnabled) {
            setLoggingConfigLocked(mAtomIdSetManager.getAtomIds());
        }
    }

    static bool isSupported() {
        static const bool featureActive = isAtLeastB() && flags::logging_control_enabled();
        return featureActive;
    }

private:
    mutable std::mutex mTagIdsMutex;
    mutable AtomIdSetManager mAtomIdSetManager;
    bool mIsEnabled = false;
    AtomsInUseListProducer mAtomsInUseListProducer;

    void setLoggingConfigLocked(const AtomIdSet& atomsInUse) {
        // sets system property & creates atom list file
        const std::vector<int32_t> atomIds{atomsInUse.begin(), atomsInUse.end()};
        if (!mAtomsInUseListProducer.setAtomsIds(atomIds)) {
            // if for some reason up to date list was not set - disable the logging control
            mAtomsInUseListProducer.reset();
        }
    }

    void resetConfigLocked() {
        // resets system property & removes atom list file
        mAtomsInUseListProducer.reset();
    }
};

}  // namespace statsd
}  // namespace os
}  // namespace android
