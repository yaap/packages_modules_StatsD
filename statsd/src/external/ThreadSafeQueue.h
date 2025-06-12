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

#include <mutex>
#include <queue>

namespace android {
namespace os {
namespace statsd {

template <typename T>
class ThreadSafeQueue {
public:
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mQueue.empty()) {
            return std::nullopt;
        }
        T value = std::move(mQueue.front());
        mQueue.pop();
        return value;
    }

    void push(const T value) {
        std::unique_lock<std::mutex> lock(mMutex);
        mQueue.push(value);
    }

    bool empty() const {
        std::unique_lock<std::mutex> lock(mMutex);
        return mQueue.empty();
    }

private:
    mutable std::mutex mMutex;
    std::queue<T> mQueue;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
