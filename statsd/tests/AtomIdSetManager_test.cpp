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

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <unordered_set>
#include <vector>

#include "socket/LogEventFilterUtils.h"

namespace android {
namespace os {
namespace statsd {

constexpr int kAtomIdsCount = 100;

// Helper to generate a set of atom IDs
AtomIdSetManager::AtomIdSet generateAtomIds(int start, int end) {
    AtomIdSetManager::AtomIdSet ids;
    for (int i = start; i <= end; ++i) {
        ids.insert(i);
    }
    return ids;
}

TEST(AtomIdSetManagerTest, TestEmpty) {
    AtomIdSetManager manager;
    const auto sampleIds = generateAtomIds(1, kAtomIdsCount);
    for (const auto& atomId : sampleIds) {
        EXPECT_FALSE(isAtomInSet(manager.getAtomIds(), atomId));
    }
}

TEST(AtomIdSetManagerTest, TestRemoveNonExistingConsumer) {
    AtomIdSetManager manager;
    EXPECT_FALSE(isAtomInSet(manager.getAtomIds(), 1));
    AtomIdSetManager::AtomIdSet emptyAtomIdsSet;
    manager.setAtomIds(emptyAtomIdsSet, reinterpret_cast<AtomIdSetManager::ConsumerId>(0));
    EXPECT_FALSE(isAtomInSet(manager.getAtomIds(), 1));
}

TEST(AtomIdSetManagerTest, TestSingleConsumer) {
    AtomIdSetManager manager;
    auto filterIds = generateAtomIds(1, kAtomIdsCount);
    manager.setAtomIds(filterIds, reinterpret_cast<AtomIdSetManager::ConsumerId>(0));

    for (int i = 1; i <= kAtomIdsCount; ++i) {
        EXPECT_TRUE(isAtomInSet(manager.getAtomIds(), i));
    }
    for (int i = kAtomIdsCount + 1; i <= kAtomIdsCount * 2; ++i) {
        EXPECT_FALSE(isAtomInSet(manager.getAtomIds(), i));
    }
}

TEST(AtomIdSetManagerTest, TestMultipleConsumersWithOverlap) {
    AtomIdSetManager manager;
    auto filterIds1 = generateAtomIds(1, kAtomIdsCount);
    auto filterIds2 = generateAtomIds(kAtomIdsCount / 2, kAtomIdsCount * 3 / 2);
    manager.setAtomIds(filterIds1, reinterpret_cast<AtomIdSetManager::ConsumerId>(0));
    manager.setAtomIds(filterIds2, reinterpret_cast<AtomIdSetManager::ConsumerId>(1));

    for (int i = 1; i <= kAtomIdsCount * 3 / 2; ++i) {
        EXPECT_TRUE(isAtomInSet(manager.getAtomIds(), i));
    }
    EXPECT_FALSE(isAtomInSet(manager.getAtomIds(), kAtomIdsCount * 3 / 2 + 1));
}

TEST(AtomIdSetManagerTest, TestMultipleConsumersRemoveOne) {
    AtomIdSetManager manager;
    auto filterIds1 = generateAtomIds(1, kAtomIdsCount);
    auto filterIds2 = generateAtomIds(kAtomIdsCount + 1, kAtomIdsCount * 2);
    manager.setAtomIds(filterIds1, reinterpret_cast<AtomIdSetManager::ConsumerId>(0));
    manager.setAtomIds(filterIds2, reinterpret_cast<AtomIdSetManager::ConsumerId>(1));

    AtomIdSetManager::AtomIdSet emptySet;
    manager.setAtomIds(emptySet, reinterpret_cast<AtomIdSetManager::ConsumerId>(1));

    for (int i = 1; i <= kAtomIdsCount * 2; ++i) {
        bool expectedInUse = (i <= kAtomIdsCount);
        EXPECT_EQ(expectedInUse, isAtomInSet(manager.getAtomIds(), i));
    }
}

}  // namespace statsd
}  // namespace os
}  // namespace android