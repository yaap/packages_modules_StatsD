/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <fuzzer/FuzzedDataProvider.h>

#include "include/stats_event.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzedDataProvider fdp(data, size);

    // This only tests the libstatssocket APIs.
    // Since fuzzing is not supported across processes, it does not fuzz statsd.
    // See statsd_fuzzer.

    AStatsEvent* event = AStatsEvent_obtain();

    AStatsEvent_setAtomId(event, fdp.ConsumeIntegral<int32_t>());
    // atom-level annotation
    AStatsEvent_addBoolAnnotation(event, fdp.ConsumeIntegral<int32_t>(), fdp.ConsumeBool());

    while (fdp.remaining_bytes() > 0) {
        AStatsEvent_writeInt32(event, fdp.ConsumeIntegral<int32_t>());
        AStatsEvent_addBoolAnnotation(event, fdp.ConsumeIntegral<int32_t>(), fdp.ConsumeBool());
        AStatsEvent_addInt32Annotation(event, fdp.ConsumeIntegral<int32_t>(),
                                       fdp.ConsumeIntegral<int32_t>());
        AStatsEvent_writeBool(event, fdp.ConsumeBool());
        AStatsEvent_addBoolAnnotation(event, fdp.ConsumeIntegral<int32_t>(), fdp.ConsumeBool());
        AStatsEvent_addInt32Annotation(event, fdp.ConsumeIntegral<int32_t>(),
                                       fdp.ConsumeIntegral<int32_t>());
        AStatsEvent_writeFloat(event, fdp.ConsumeFloatingPoint<float>());
        AStatsEvent_addBoolAnnotation(event, fdp.ConsumeIntegral<int32_t>(), fdp.ConsumeBool());
        AStatsEvent_addInt32Annotation(event, fdp.ConsumeIntegral<int32_t>(),
                                       fdp.ConsumeIntegral<int32_t>());
        AStatsEvent_writeInt64(event, fdp.ConsumeIntegral<int64_t>());
        AStatsEvent_addBoolAnnotation(event, fdp.ConsumeIntegral<int32_t>(), fdp.ConsumeBool());
        AStatsEvent_addInt32Annotation(event, fdp.ConsumeIntegral<int32_t>(),
                                       fdp.ConsumeIntegral<int32_t>());
        AStatsEvent_writeString(event, fdp.ConsumeRandomLengthString().c_str());
        AStatsEvent_addBoolAnnotation(event, fdp.ConsumeIntegral<int32_t>(), fdp.ConsumeBool());
        AStatsEvent_addInt32Annotation(event, fdp.ConsumeIntegral<int32_t>(),
                                       fdp.ConsumeIntegral<int32_t>());
    }

    AStatsEvent_write(event);
    AStatsEvent_release(event);
    return 0;
}
