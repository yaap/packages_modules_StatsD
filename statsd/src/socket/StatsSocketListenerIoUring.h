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

#include <IOUringSocketHandler/IOUringSocketHandler.h>
#include <gtest/gtest_prod.h>
#include <utils/RefBase.h>

#include <thread>

#include "BaseStatsSocketListener.h"
#include "LogEventFilter.h"
#include "logd/LogEventQueue.h"

namespace android {
namespace os {
namespace statsd {

class StatsSocketListenerIoUring : public virtual BaseStatsSocketListener, public virtual RefBase {
public:
    explicit StatsSocketListenerIoUring(const std::shared_ptr<LogEventQueue>& queue,
                                        const std::shared_ptr<LogEventFilter>& logEventFilter);

    virtual ~StatsSocketListenerIoUring() = default;

    virtual int startListener() override;
    virtual int stopListener() override;

protected:
    bool onDataAvailable(void* buffer, int len, struct ucred* credential);

private:
    void threadFunction();

    bool initializeIoUring();

    void startStatsSocketListener();

    std::unique_ptr<IOUringSocketHandler> mIoUringSocketHandler;

    static const int MAX_BUFFERS = 64;

    std::unique_ptr<BaseStatsSocketListener> gSocketListener = nullptr;

    bool mFalledBackToSyncListener = false;

    std::atomic_bool mShouldStopThreadFunction = false;

    std::thread mThread;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
