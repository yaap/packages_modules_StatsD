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

#include <memory>
#include <set>
#include <unordered_set>

namespace android {
namespace os {
namespace statsd {

class AtomsInUseChangeListener {
public:
    virtual ~AtomsInUseChangeListener() = default;

    typedef const void* ConsumerId;

    typedef std::unordered_set<int32_t> AtomIdSet;
    /**
     * @brief Set the Atom Ids object
     *
     * @param tagIds set of atoms ids
     * @param consumer used to differentiate the consumers to form proper superset of ids
     */
    virtual void setAtomIds(AtomIdSet tagIds, ConsumerId consumer) = 0;
};

/**
 * Propagates changes in atoms ids to its listeners
 */
class AtomsInUseChangeDispatcher : public AtomsInUseChangeListener {
public:
    void setAtomIds(AtomIdSet tagIds, ConsumerId consumer) override {
        for (auto& listener : mListeners) {
            listener->setAtomIds(tagIds, consumer);
        }
    }

    void addListener(const std::shared_ptr<AtomsInUseChangeListener>& listener) {
        mListeners.insert(listener);
    }

    void removeListener(const std::shared_ptr<AtomsInUseChangeListener>& listener) {
        mListeners.erase(listener);
    }

private:
    std::set<std::shared_ptr<AtomsInUseChangeListener>> mListeners;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
