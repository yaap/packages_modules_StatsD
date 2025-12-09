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

#include <cstdint>

namespace android {
namespace os {
namespace statsd {

constexpr const char* kAtomIdsVersionName = "statsd.config.atoms_in_use_list.version";
constexpr const char* kAtomIdsFileName = "/data/misc/stats-atoms/atoms.bin";

// Magic number to identify Atom-Ids-In-Use file format
constexpr uint32_t kMagicNumber = 0xDA1102B7;
// Initial format version
constexpr uint32_t kFormatVersion1 = 1;

/**
 * The file format is binary with schema:
 * [FileHeader][BlockHeader][Int32Array]
 */
struct FileHeader {
    uint32_t magic_number;  // Should be kMagicNumber
    uint32_t version;       // Should be kFormatVersion1
};

struct BlockHeader {
    uint32_t atomIdsCount;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
