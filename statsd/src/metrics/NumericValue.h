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

#include <string>
#include <utility>
#include <variant>

#include "HistogramValue.h"

#pragma once

namespace android {
namespace os {
namespace statsd {

// Used to store aggregations in NumericValueMetricProducer for ValueMetric.
// The aggregations are either int64 or double.
class NumericValue {
public:
    NumericValue() noexcept = default;

    // Copy constructor
    constexpr NumericValue(const NumericValue& other) = default;
    constexpr NumericValue(NumericValue& other) : NumericValue(std::as_const(other)) {
    }

    // Copy constructor for contained types
    template <typename V>
    constexpr NumericValue(const V& value) : mData(std::in_place_type<V>, value) {
    }

    // Move constructor
    constexpr NumericValue(NumericValue&& other) noexcept = default;

    // Move constructor for contained types
    template <typename V>
    constexpr NumericValue(V&& value) noexcept
        : mData(std::in_place_type<V>, std::forward<V>(value)) {
    }

    // Copy assignment
    NumericValue& operator=(const NumericValue& rhs) = default;

    // Move assignment
    NumericValue& operator=(NumericValue&& rhs) noexcept = default;

    ~NumericValue() = default;

    std::string toString() const;

    void reset();

    template <typename V>
    bool is() const;

    bool hasValue() const;

    template <typename V>
    V& getValue();

    template <typename V>
    const V& getValue() const;

    template <typename V>
    V& getValueOrDefault(V& defaultValue);

    template <typename V>
    const V& getValueOrDefault(const V& defaultValue) const;

    bool isZero() const;

    size_t getSize() const;

    NumericValue& operator+=(const NumericValue& rhs);

    friend NumericValue operator+(NumericValue lhs, const NumericValue& rhs);

    NumericValue& operator-=(const NumericValue& rhs);

    friend NumericValue operator-(NumericValue lhs, const NumericValue& rhs);

    friend bool operator==(const NumericValue& lhs, const NumericValue& rhs);

    friend bool operator!=(const NumericValue& lhs, const NumericValue& rhs);

    friend bool operator<(const NumericValue& lhs, const NumericValue& rhs);

    friend bool operator>(const NumericValue& lhs, const NumericValue& rhs);

    friend bool operator<=(const NumericValue& lhs, const NumericValue& rhs);

    friend bool operator>=(const NumericValue& lhs, const NumericValue& rhs);

private:
    // std::monostate represents "empty" or default value.
    std::variant<std::monostate, int64_t, double, HistogramValue> mData;
};

}  // namespace statsd
}  // namespace os
}  // namespace android
