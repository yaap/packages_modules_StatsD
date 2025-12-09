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

#include <gtest/gtest_prod.h>

#include <map>
#include <unordered_set>

namespace android {
namespace os {
namespace statsd {

/**
 *
 * Based on benchmarks the more fast container to be used for atom ids filtering
 * is unordered_set<int>
 * #BM_LogEventFilterUnorderedSet                       391208 ns     390086 ns         1793
 * #BM_LogEventFilterUnorderedSet2Consumers            1293527 ns    1289326 ns          543
 * #BM_LogEventFilterSet                                613362 ns     611259 ns         1146
 * #BM_LogEventFilterSet2Consumers                     1859397 ns    1854193 ns          378
 *
 */

/**
 * Stores superset of atoms ids consumed by various consumers
 */

class AtomIdSetManager {
public:
    using ConsumerId = const void*;

    using AtomIdSet = std::unordered_set<int32_t>;

    const AtomIdSet& getAtomIds() const {
        return mTagIds;
    }

    AtomIdSet& getAtomIdsMutable() {
        return mTagIds;
    }

    void setAtomIds(AtomIdSet tagIds, ConsumerId consumer) {
        // update ids list from consumer
        if (tagIds.size() == 0) {
            mTagIdsPerConsumer.erase(consumer);
        } else {
            mTagIdsPerConsumer[consumer] = std::move(tagIds);
        }
        // populate the superset incorporating list of distinct atom ids from all consumers
        mTagIds.clear();
        for (auto& [_, inputSet] : mTagIdsPerConsumer) {
            mTagIds.insert(inputSet.begin(), inputSet.end());
        }
    }

private:
    std::map<ConsumerId, AtomIdSet> mTagIdsPerConsumer;
    AtomIdSet mTagIds;
};

inline bool isAtomInSet(const std::unordered_set<int32_t>& s, int32_t atomId) {
    return s.find(atomId) != s.end();
}

}  // namespace statsd
}  // namespace os
}  // namespace android
