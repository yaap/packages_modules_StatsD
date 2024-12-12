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

#include "NumericValue.h"

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <variant>

#include "HistogramValue.h"

namespace android {
namespace os {
namespace statsd {
namespace {

// std::variant uses the visitor pattern to interact with stored types via std::visit, which applies
// a Callable (a function object) that accepts all combination of types from the variant. Here, the
// Callables are implemented as structs with operator() overloads for each combination of types from
// the variant in NumericValue.

// Templated visitor for binary operations involving two NumericValues
// Used for implementing operator+= and operator-= for NumericValue.
template <typename BinaryOp>
class BinaryOperationVisitor {
public:
    constexpr explicit BinaryOperationVisitor(BinaryOp&& op) : mOp(std::forward<BinaryOp>(op)) {
    }

    void operator()(std::monostate, std::monostate) const {
    }

    template <typename V>
    void operator()(V& lhs, const V& rhs) const {
        lhs = mOp(lhs, rhs);
    }

    void operator()(auto, auto) const {
    }

private:
    const BinaryOp mOp;
};
constexpr BinaryOperationVisitor subtract(std::minus{});
constexpr BinaryOperationVisitor add(std::plus{});

// Visitor for printing type information currently stored in the NumericValue variant.
struct ToStringVisitor {
    std::string operator()(int64_t value) const {
        return std::to_string(value) + "[L]";
    }

    std::string operator()(double value) const {
        return std::to_string(value) + "[D]";
    }

    std::string operator()(const HistogramValue& value) const {
        return value.toString();
    }

    std::string operator()(auto) const {
        return "[UNKNOWN]";
    }
};

// Visitor for determining whether the NumericValue variant stores a 0.
struct IsZeroVisitor {
    bool operator()(int64_t value) const {
        return value == 0;
    }

    bool operator()(double value) const {
        return fabs(value) <= std::numeric_limits<double>::epsilon();
    }

    bool operator()(const HistogramValue& value) const {
        return value.isEmpty();
    }

    // "Empty" variant does not store 0.
    bool operator()(std::monostate) const {
        return false;
    }
};

struct GetSizeVisitor {
    size_t operator()(const HistogramValue& value) const {
        return value.getSize();
    }

    size_t operator()(const auto& value) const {
        return sizeof(value);
    }
};

}  // anonymous namespace

std::string NumericValue::toString() const {
    return std::visit(ToStringVisitor{}, mData);
}

void NumericValue::reset() {
    mData.emplace<std::monostate>(std::monostate{});
}

template <typename V>
bool NumericValue::is() const {
    return std::holds_alternative<V>(mData);
}
template bool NumericValue::is<int64_t>() const;
template bool NumericValue::is<double>() const;
template bool NumericValue::is<HistogramValue>() const;

bool NumericValue::hasValue() const {
    return !is<std::monostate>();
}

template <typename V>
V& NumericValue::getValue() {
    return std::get<V>(mData);
}
template int64_t& NumericValue::getValue<int64_t>();
template double& NumericValue::getValue<double>();
template HistogramValue& NumericValue::getValue<HistogramValue>();

template <typename V>
const V& NumericValue::getValue() const {
    return std::get<V>(mData);
}
template const int64_t& NumericValue::getValue<int64_t>() const;
template const double& NumericValue::getValue<double>() const;
template const HistogramValue& NumericValue::getValue<HistogramValue>() const;

template <typename V>
V& NumericValue::getValueOrDefault(V& defaultValue) {
    return is<V>() ? getValue<V>() : defaultValue;
}
template int64_t& NumericValue::getValueOrDefault<int64_t>(int64_t& defaultValue);
template double& NumericValue::getValueOrDefault<double>(double& defaultValue);
template HistogramValue& NumericValue::getValueOrDefault<HistogramValue>(
        HistogramValue& defaultValue);

template <typename V>
const V& NumericValue::getValueOrDefault(const V& defaultValue) const {
    return is<V>() ? getValue<V>() : defaultValue;
}
template const int64_t& NumericValue::getValueOrDefault<int64_t>(const int64_t& defaultValue) const;
template const double& NumericValue::getValueOrDefault<double>(const double& defaultValue) const;
template const HistogramValue& NumericValue::getValueOrDefault<HistogramValue>(
        const HistogramValue& defaultValue) const;

bool NumericValue::isZero() const {
    return std::visit(IsZeroVisitor{}, mData);
}

size_t NumericValue::getSize() const {
    return std::visit(GetSizeVisitor{}, mData);
}

NumericValue& NumericValue::operator+=(const NumericValue& rhs) {
    std::visit(add, mData, rhs.mData);
    return *this;
}

NumericValue operator+(NumericValue lhs, const NumericValue& rhs) {
    lhs += rhs;
    return lhs;
}

NumericValue& NumericValue::operator-=(const NumericValue& rhs) {
    std::visit(subtract, mData, rhs.mData);
    return *this;
}

NumericValue operator-(NumericValue lhs, const NumericValue& rhs) {
    lhs -= rhs;
    return lhs;
}

bool operator==(const NumericValue& lhs, const NumericValue& rhs) {
    return lhs.mData == rhs.mData;
}

bool operator!=(const NumericValue& lhs, const NumericValue& rhs) {
    return !(lhs == rhs);
}

bool operator<(const NumericValue& lhs, const NumericValue& rhs) {
    return lhs.mData < rhs.mData;
}

bool operator>(const NumericValue& lhs, const NumericValue& rhs) {
    return rhs < lhs;
}

bool operator<=(const NumericValue& lhs, const NumericValue& rhs) {
    return !(lhs > rhs);
}

bool operator>=(const NumericValue& lhs, const NumericValue& rhs) {
    return !(lhs < rhs);
}

}  // namespace statsd
}  // namespace os
}  // namespace android
