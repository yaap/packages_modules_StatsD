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

#include <stdlib.h>

using AStatsEventAtomId = uint32_t;

// The AStatsEvent struct holds the serialized encoding of an event
// within a buf. Also includes other required fields.
struct AStatsEvent {
    uint8_t* buf;
    // Location of last field within the buf. Here, field denotes either a
    // metadata field (e.g. timestamp) or an atom field.
    size_t lastFieldPos;
    // Number of valid bytes within the buffer.
    size_t numBytesWritten;
    uint32_t numElements;
    AStatsEventAtomId atomId;
    uint32_t errors;
    bool built;
    size_t bufSize;
};
