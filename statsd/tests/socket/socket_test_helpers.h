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

constexpr const char* kTestFileName = "/data/local/tmp/atoms_in_use_test.bin";
constexpr const char* kTestVersionProperty = "debug.statsd.config.atoms_in_use_list.version";

void cleanupSocketTestFiles(const std::string& fileName, const std::string& versionProperty);
void verifyAtomsInUseFileContent(const std::string& fileName, const std::vector<int32_t>& atoms);
void verifyAtomsInUseListAbsence(const std::string& fileName, const std::string& versionProperty);

}  // namespace statsd
}  // namespace os
}  // namespace android