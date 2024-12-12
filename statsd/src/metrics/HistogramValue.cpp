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

#define STATSD_DEBUG false  // STOPSHIP if true
#include "Log.h"

#include "HistogramValue.h"

#include <android/util/ProtoOutputStream.h>

#include <algorithm>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_SINT32;
using android::util::ProtoOutputStream;
using std::string;
using std::vector;

namespace android {
namespace os {
namespace statsd {
namespace {
constexpr int FIELD_ID_COUNT = 1;
}  // anonymous namespace

const HistogramValue HistogramValue::ERROR_BINS_MISMATCH = HistogramValue({-1});
const HistogramValue HistogramValue::ERROR_BIN_COUNT_TOO_HIGH = HistogramValue({-2});

string HistogramValue::toString() const {
    std::stringstream result("{");
    std::copy(mBinCounts.begin(), mBinCounts.end(), std::ostream_iterator<int>(result, ", "));
    return result.str() + "}";
}

bool HistogramValue::isEmpty() const {
    return mBinCounts.empty() ||
           std::all_of(mBinCounts.begin(), mBinCounts.end(), [](int count) { return count <= 0; });
}

size_t HistogramValue::getSize() const {
    return sizeof(int) * mBinCounts.size();
}

void HistogramValue::toProto(ProtoOutputStream& protoOutput) const {
    for (int binCount : mBinCounts) {
        protoOutput.write(FIELD_TYPE_SINT32 | FIELD_COUNT_REPEATED | FIELD_ID_COUNT, binCount);
    }
}

void HistogramValue::addValue(float value, const BinStarts& binStarts) {
    if (mBinCounts.empty()) {
        mBinCounts.resize(binStarts.size(), 0);
    }
    size_t index = 0;
    for (; index < binStarts.size() - 1; index++) {
        if (value < binStarts[index + 1]) {
            break;
        }
    }
    mBinCounts[index]++;
}

HistogramValue HistogramValue::getCompactedHistogramValue() const {
    size_t compactSize = getCompactedBinCountsSize(mBinCounts);
    HistogramValue result;
    result.mBinCounts.reserve(compactSize);
    int zeroCount = 0;
    for (int binCount : mBinCounts) {
        if (binCount <= 0) {
            zeroCount++;
        } else {
            if (zeroCount > 1) {
                result.mBinCounts.push_back(-zeroCount);
            } else if (zeroCount == 1) {
                result.mBinCounts.push_back(0);
            }
            result.mBinCounts.push_back(binCount);
            zeroCount = 0;
        }
    }
    if (zeroCount > 1) {
        result.mBinCounts.push_back(-zeroCount);
    } else if (zeroCount == 1) {
        result.mBinCounts.push_back(0);
    }

    result.mCompacted = true;
    return result;
}

bool HistogramValue::isValid() const {
    return mCompacted ||
           std::all_of(mBinCounts.begin(), mBinCounts.end(), [](int count) { return count >= 0; });
}

HistogramValue& HistogramValue::operator+=(const HistogramValue& rhs) {
    if (mBinCounts.size() < rhs.mBinCounts.size()) {
        ALOGE("HistogramValue::operator+=() arg has too many bins");
        *this = ERROR_BINS_MISMATCH;
        return *this;
    }
    for (size_t i = 0; i < rhs.mBinCounts.size(); i++) {
        mBinCounts[i] += rhs.mBinCounts[i];
    }
    return *this;
}

HistogramValue operator+(HistogramValue lhs, const HistogramValue& rhs) {
    lhs += rhs;
    return lhs;
}

HistogramValue& HistogramValue::operator-=(const HistogramValue& rhs) {
    if (mBinCounts.size() < rhs.mBinCounts.size()) {
        ALOGE("HistogramValue::operator-=() arg has too many bins");
        *this = ERROR_BINS_MISMATCH;
        return *this;
    }
    for (size_t i = 0; i < rhs.mBinCounts.size(); i++) {
        if (mBinCounts[i] < rhs.mBinCounts[i]) {
            ALOGE("HistogramValue::operator-=() arg has a bin count that is too high");
            *this = ERROR_BIN_COUNT_TOO_HIGH;
            return *this;
        }
        mBinCounts[i] -= rhs.mBinCounts[i];
    }
    return *this;
}

HistogramValue operator-(HistogramValue lhs, const HistogramValue& rhs) {
    lhs -= rhs;
    return lhs;
}

bool operator==(const HistogramValue& lhs, const HistogramValue& rhs) {
    return lhs.mBinCounts == rhs.mBinCounts;
}

bool operator!=(const HistogramValue& lhs, const HistogramValue& rhs) {
    return lhs.mBinCounts != rhs.mBinCounts;
}

bool operator<(const HistogramValue& lhs, const HistogramValue& rhs) {
    ALOGE("HistogramValue::operator<() should not be called");
    return false;
}

bool operator>(const HistogramValue& lhs, const HistogramValue& rhs) {
    ALOGE("HistogramValue::operator>() should not be called");
    return false;
}

bool operator<=(const HistogramValue& lhs, const HistogramValue& rhs) {
    ALOGE("HistogramValue::operator<=() should not be called");
    return false;
}

bool operator>=(const HistogramValue& lhs, const HistogramValue& rhs) {
    ALOGE("HistogramValue::operator>=() should not be called");
    return false;
}

size_t getCompactedBinCountsSize(const std::vector<int>& binCounts) {
    if (binCounts.empty()) {
        return 0;
    }
    size_t compactSize = 1;
    for (size_t i = 1; i < binCounts.size(); i++) {
        // If current index i and the previous index i-1 hold 0, ie. this is a consecutive bin with
        // 0, then this bin will be compressed after compaction and not be counted towards the
        // compacted size. Hence, only increment compactSize if at least one of current index or
        // the previous index have non-zero bin counts.
        if (binCounts[i] != 0 || binCounts[i - 1] != 0) {
            compactSize++;
        }
    }
    return compactSize;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
