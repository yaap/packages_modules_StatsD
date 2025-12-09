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

#include <string>
#include <vector>

namespace android {
namespace os {
namespace statsd {

// Responsible for population external predefined file with atom ids in use
// and maintaining system property with the file version as a counter

class AtomsInUseListProducer {
public:
    // via constructor parameters support custom values for testing, since
    // statsd_test should not affect global state
    AtomsInUseListProducer(std::string fileName, std::string versionPropertyName);

    ~AtomsInUseListProducer();

    // updates external file with new list and bumps up the system property version
    bool setAtomsIds(const std::vector<int32_t>& atomIds);

    // removes the file & removes system property version, this will allow clients
    // to log any atom
    void reset() const;

    static constexpr int32_t kMaxAtomIdsInList = 4096;

private:
    const std::string mFileName;
    const std::string mVersionPropertyName;

    int64_t mListVersion = 0;

    bool createAtomIdsFile(const std::vector<int32_t>& atomIds);
    bool increaseVersionProperty();

    bool removeVersionProperty() const;
};

}  // namespace statsd
}  // namespace os
}  // namespace android