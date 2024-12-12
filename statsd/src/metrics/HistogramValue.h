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

#include <android/util/ProtoOutputStream.h>

#include <string>
#include <vector>

#include "stats_util.h"

#pragma once

namespace android {
namespace os {
namespace statsd {

size_t getCompactedBinCountsSize(const std::vector<int>& binCounts);

// Encapsulates histogram bin counts. This class is not thread-safe.
class HistogramValue {
public:
    // Default constructor
    constexpr HistogramValue() noexcept = default;

    // Copy constructor for pre-aggregated bin counts.
    constexpr HistogramValue(const std::vector<int>& binCounts) : mBinCounts(binCounts) {
    }

    // Move constructor for pre-aggregated bin counts.
    constexpr HistogramValue(std::vector<int>&& binCounts) : mBinCounts(std::move(binCounts)) {
    }

    std::string toString() const;

    bool isEmpty() const;

    size_t getSize() const;

    // Should only be called on HistogramValue instances returned by getCompactedHistogramValue().
    // Also, this should be called to dump histogram data to proto.
    void toProto(android::util::ProtoOutputStream& protoOutput) const;

    void addValue(float value, const BinStarts& binStarts);

    // Returns a new HistogramValue where mBinCounts is compressed as follows:
    // For each entry in mBinCounts, n:
    //  * n >= 0 represents the actual count
    //  * n == -1 does not appear
    //  * n <= -2 represents -n consecutive bins with count of 0
    // Called on bucket flushes from NumericValueMetricProducer
    HistogramValue getCompactedHistogramValue() const;

    bool isValid() const;

    HistogramValue& operator+=(const HistogramValue& rhs);

    // Returns a HistogramValue where each bin is the sum of the corresponding bins in lhs and rhs.
    // If rhs has fewer bins, the remaining lhs bins are copied to the returned HistogramValue.
    friend HistogramValue operator+(HistogramValue lhs, const HistogramValue& rhs);

    HistogramValue& operator-=(const HistogramValue& rhs);

    // Returns a HistogramValue where each bin in rhs is subtracted from the corresponding bin in
    // lhs. If rhs has fewer bins, the remaining lhs bins are copied to the returned HistogramValue.
    // For bins where the returned value would be less than 0, their values are set to 0 instead.
    friend HistogramValue operator-(HistogramValue lhs, const HistogramValue& rhs);

    friend bool operator==(const HistogramValue& lhs, const HistogramValue& rhs);

    friend bool operator!=(const HistogramValue& lhs, const HistogramValue& rhs);

    friend bool operator<(const HistogramValue& lhs, const HistogramValue& rhs);

    friend bool operator>(const HistogramValue& lhs, const HistogramValue& rhs);

    friend bool operator<=(const HistogramValue& lhs, const HistogramValue& rhs);

    friend bool operator>=(const HistogramValue& lhs, const HistogramValue& rhs);

    // Error states encountered during binary operations.
    static const HistogramValue ERROR_BINS_MISMATCH;
    static const HistogramValue ERROR_BIN_COUNT_TOO_HIGH;

private:
    std::vector<int> mBinCounts;
    bool mCompacted = false;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
