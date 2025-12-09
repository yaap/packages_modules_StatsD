/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <gtest/gtest_prod.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include "LogEventFilterUtils.h"
#include "socket/AtomsInUseChangeListener.h"

namespace android {
namespace os {
namespace statsd {

/**
 * Stores superset of atoms ids consumed by various consumers in a thread safe way
 * Maintains thread-local copy for fast search operations without holding a mutex
 * on each query
 */

class LogEventFilter : public AtomsInUseChangeListener {
public:
    virtual ~LogEventFilter() = default;

    virtual void setFilteringEnabled(bool isEnabled) {
        mLogsFilteringEnabled = isEnabled;
    }

    bool getFilteringEnabled() const {
        return mLogsFilteringEnabled;
    }

    /**
     * @brief Tests atom id with list of interesting atoms
     *        If Logs filtering is disabled - assume all atoms in use
     *        Most of the time should be non-blocking call - only in case when setAtomIds() was
     *        called the call will be blocking due to atom list needs to be synced up
     * @param atomId
     * @return true if atom is used by any of consumer or filtering is disabled
     */
    bool isAtomInUse(int32_t atomId) const {
        if (!mLogsFilteringEnabled) {
            return true;
        }

        // check if there is an updated set of interesting atom ids
        if (mLocalSetUpdateCounter != mSetUpdateCounter.load(std::memory_order_relaxed)) {
            std::lock_guard guard(mTagIdsMutex);
            mLocalSetUpdateCounter = mSetUpdateCounter.load(std::memory_order_relaxed);
            // swap provides constant complexity - no copy overhead
            // the content of mAtomIdSetManager is invalid after, which is ok
            // it is not used anywhere else except for thread local cache update
            mLocalTagIds.swap(mAtomIdSetManager.getAtomIdsMutable());
        }
        return isAtomInSet(mLocalTagIds, atomId);
    }

    /**
     * @brief Set the Atom Ids object
     *
     * @param tagIds set of atoms ids
     * @param consumer used to differentiate the consumers to form proper superset of ids
     */
    void setAtomIds(AtomIdSet tagIds, ConsumerId consumer) override {
        std::lock_guard lock(mTagIdsMutex);
        mAtomIdSetManager.setAtomIds(std::move(tagIds), consumer);
        mSetUpdateCounter.fetch_add(1, std::memory_order_relaxed);
    }

private:
    std::atomic_bool mLogsFilteringEnabled = false;
    mutable std::mutex mTagIdsMutex;
    mutable AtomIdSetManager mAtomIdSetManager;
    std::atomic_int mSetUpdateCounter;

    mutable int mLocalSetUpdateCounter;
    mutable AtomIdSet mLocalTagIds;

    FRIEND_TEST(LogEventFilterTest, TestNonEmptyFilterFullOverlap);
    FRIEND_TEST(LogEventFilterTest, TestNonEmptyFilterPartialOverlap);
    FRIEND_TEST(LogEventFilterTest, TestNonEmptyFilterDisabled);
    FRIEND_TEST(LogEventFilterTest, TestNonEmptyFilterDisabledPartialOverlap);
    FRIEND_TEST(LogEventFilterTest, TestMultipleConsumerOverlapIds);
    FRIEND_TEST(LogEventFilterTest, TestMultipleConsumerOverlapIdsRemoved);
    FRIEND_TEST(LogEventFilterTest, TestMultipleConsumerEmptyFilter);
};

}  // namespace statsd
}  // namespace os
}  // namespace android
