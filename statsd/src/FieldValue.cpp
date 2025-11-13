/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define STATSD_DEBUG false
#include "Log.h"

#include "FieldValue.h"

#include "HashableDimensionKey.h"
#include "hash.h"
#include "math.h"

using std::string;
using std::vector;

namespace android {
namespace os {
namespace statsd {

int32_t getEncodedField(int32_t pos[], int32_t depth, bool includeDepth) {
    int32_t field = 0;
    for (int32_t i = 0; i <= depth; i++) {
        int32_t shiftBits = 8 * (kMaxLogDepth - i);
        field |= (pos[i] << shiftBits);
    }

    if (includeDepth) {
        field |= (depth << 24);
    }
    return field;
}

int32_t encodeMatcherMask(int32_t mask[], int32_t depth) {
    return getEncodedField(mask, depth, false) | 0xff000000;
}

bool Field::matches(const Matcher& matcher) const {
    if (mTag != matcher.mMatcher.getTag()) {
        return false;
    }
    if ((mField & matcher.mMask) == matcher.mMatcher.getField()) {
        return true;
    }

    if (matcher.hasAllPositionMatcher() &&
        (mField & (matcher.mMask & kClearAllPositionMatcherMask)) == matcher.mMatcher.getField()) {
        return true;
    }

    return false;
}

vector<Matcher> dedupFieldMatchers(const vector<Matcher>& fieldMatchers) {
    vector<Matcher> dedupedFieldMatchers;
    for (size_t i = 0; i < fieldMatchers.size(); i++) {
        if (std::find(dedupedFieldMatchers.begin(), dedupedFieldMatchers.end(), fieldMatchers[i]) ==
            dedupedFieldMatchers.end()) {
            dedupedFieldMatchers.push_back(fieldMatchers[i]);
        }
    }
    return dedupedFieldMatchers;
}

void translateFieldMatcher(int tag, const FieldMatcher& matcher, int depth, int* pos, int* mask,
                           vector<Matcher>* output) {
    if (depth > kMaxLogDepth) {
        ALOGE("depth > 2");
        return;
    }

    pos[depth] = matcher.field();
    mask[depth] = 0x7f;

    if (matcher.has_position()) {
        depth++;
        if (depth > 2) {
            return;
        }
        switch (matcher.position()) {
            case Position::ALL:
                pos[depth] = 0x00;
                mask[depth] = 0x7f;
                break;
            case Position::ANY:
                pos[depth] = 0;
                mask[depth] = 0;
                break;
            case Position::FIRST:
                pos[depth] = 1;
                mask[depth] = 0x7f;
                break;
            case Position::LAST:
                pos[depth] = 0x80;
                mask[depth] = 0x80;
                break;
            case Position::POSITION_UNKNOWN:
                pos[depth] = 0;
                mask[depth] = 0;
                break;
        }
    }

    if (matcher.child_size() == 0) {
        output->push_back(Matcher(Field(tag, pos, depth), encodeMatcherMask(mask, depth)));
    } else {
        for (const auto& child : matcher.child()) {
            translateFieldMatcher(tag, child, depth + 1, pos, mask, output);
        }
    }
}

void translateFieldMatcher(const FieldMatcher& matcher, vector<Matcher>* output) {
    int pos[] = {1, 1, 1};
    int mask[] = {0x7f, 0x7f, 0x7f};
    int tag = matcher.field();
    for (const auto& child : matcher.child()) {
        translateFieldMatcher(tag, child, 0, pos, mask, output);
    }
}

bool isAttributionUidField(const FieldValue& value) {
    return isAttributionUidField(value.mField, value.mValue);
}

int32_t getUidIfExists(const FieldValue& value) {
    // the field is uid field if the field is the uid field in attribution node
    // or annotated as such in the atom
    bool isUid = isAttributionUidField(value) || isUidField(value);
    return isUid ? value.mValue.get<int32_t>() : -1;
}

bool isAttributionUidField(const Field& field, const Value& value) {
    int f = field.getField() & 0xff007f;
    if (f == 0x10001 && value.getType() == INT) {
        return true;
    }
    return false;
}

bool isUidField(const FieldValue& fieldValue) {
    return fieldValue.mAnnotations.isUidField();
}

bool isPrimitiveRepeatedField(const Field& field) {
    return field.getDepth() == 1;
}

// anonymous namespace for Value variant visitors
namespace {
// Visitor for printing type information currently stored in the Value.
struct ToStringVisitor {
    string operator()(int32_t value) const {
        return std::to_string(value) + "[I]";
    }
    string operator()(int64_t value) const {
        return std::to_string(value) + "[L]";
    }
    string operator()(float value) const {
        return std::to_string(value) + "[F]";
    }
    string operator()(double value) const {
        return std::to_string(value) + "[D]";
    }
    string operator()(const string& value) const {
        return value + "[S]";
    }
    string operator()(const vector<uint8_t>& value) const {
        return "bytes of size " + std::to_string(value.size()) + "[ST]";
    }
    string operator()(std::monostate) const {
        return "[UNKNOWN]";
    }
};

struct GetSizeVisitor {
    size_t operator()(const string& value) const {
        return sizeof(char) * value.length();
    }
    size_t operator()(const vector<uint8_t>& value) const {
        return sizeof(uint8_t) * value.size();
    }
    size_t operator()(const auto& value) const {
        return sizeof(value);
    }
};
}  // namespace

// Keeping the impl in the cpp file and explicitly naming the templates prevents accidentally
// accessing unsupported types.
template <typename V>
V& Value::get() {
    return std::get<V>(mData);
}
template int32_t& Value::get<int32_t>();
template int64_t& Value::get<int64_t>();
template float& Value::get<float>();
template double& Value::get<double>();
template string& Value::get<string>();
template vector<uint8_t>& Value::get<vector<uint8_t>>();

template <typename V>
const V& Value::get() const {
    return std::get<V>(mData);
}
template const int32_t& Value::get<int32_t>() const;
template const int64_t& Value::get<int64_t>() const;
template const float& Value::get<float>() const;
template const double& Value::get<double>() const;
template const string& Value::get<string>() const;
template const vector<uint8_t>& Value::get<vector<uint8_t>>() const;

template <typename V>
void Value::set(V v) {
    mData = v;
}
template void Value::set<int32_t>(int32_t);
template void Value::set<int64_t>(int64_t);

string Value::toString() const {
    return std::visit(ToStringVisitor{}, mData);
}

Value& Value::operator+=(const Value& that) {
    Type type = getType();
    if (type != that.getType()) {
        ALOGE("Can't operate on different value types, %d, %d", type, that.getType());
        return *this;
    }
    if (type == STRING) {
        ALOGE("Can't operate on string value type");
        return *this;
    }
    if (type == STORAGE) {
        ALOGE("Can't operate on storage value type");
        return *this;
    }

    switch (type) {
        case INT:
            mData = get<int32_t>() + that.get<int32_t>();
            break;
        case LONG:
            mData = get<int64_t>() + that.get<int64_t>();
            break;
        case FLOAT:
            mData = get<float>() + that.get<float>();
            break;
        case DOUBLE:
            mData = get<double>() + that.get<double>();
            break;
        default:
            break;
    }
    return *this;
}

size_t Value::getSize() const {
    return std::visit(GetSizeVisitor{}, mData);
}

string Annotations::toString() const {
    string annotations;
    if (isUidField()) {
        annotations += "UID";
    }
    if (isPrimaryField()) {
        annotations += annotations.size() > 0 ? ",PRIMARY" : "PRIMARY";
    }
    if (isExclusiveState()) {
        annotations += annotations.size() > 0 ? ",EXCLUSIVE" : "EXCLUSIVE";
    }
    if (isNested()) {
        annotations += annotations.size() > 0 ? ",NESTED" : "NESTED";
    }
    if (annotations.size()) {
        annotations = "[" + annotations + "]";
    }
    return annotations;
}

bool equalDimensions(const vector<Matcher>& dimension_a, const vector<Matcher>& dimension_b) {
    bool eq = dimension_a.size() == dimension_b.size();
    for (size_t i = 0; eq && i < dimension_a.size(); ++i) {
        if (dimension_b[i] != dimension_a[i]) {
            eq = false;
        }
    }
    return eq;
}

/* Is dimension_a a subset of dimension_b. */
bool subsetDimensions(const vector<Matcher>& dimension_a, const vector<Matcher>& dimension_b) {
    if (dimension_a.size() > dimension_b.size()) {
        return false;
    }
    for (size_t i = 0; i < dimension_a.size(); ++i) {
        bool found = false;
        for (size_t j = 0; j < dimension_b.size(); ++j) {
            if (dimension_a[i] == dimension_b[j]) {
                found = true;
                break;
            }

            // Check equality of repeated fields with different positions.
            // Only position FIRST and LAST are considered subsets of position ALL.
            if (dimension_b[j].hasAllPositionMatcher() &&
                (dimension_a[i].hasFirstPositionMatcher() ||
                 dimension_a[i].hasLastPositionMatcher())) {
                if (dimension_a[i].isEqualWithoutPositionBits(dimension_b[j])) {
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

bool HasPositionANY(const FieldMatcher& matcher) {
    if (matcher.has_position() && matcher.position() == Position::ANY) {
        return true;
    }
    for (const auto& child : matcher.child()) {
        if (HasPositionANY(child)) {
            return true;
        }
    }
    return false;
}

bool HasPositionALL(const FieldMatcher& matcher) {
    if (matcher.has_position() && matcher.position() == Position::ALL) {
        return true;
    }
    for (const auto& child : matcher.child()) {
        if (HasPositionALL(child)) {
            return true;
        }
    }
    return false;
}

bool HasPrimitiveRepeatedField(const FieldMatcher& matcher) {
    for (const auto& child : matcher.child()) {
        if (child.has_position() && child.child_size() == 0) {
            return true;
        }
    }
    return false;
}

bool ShouldUseNestedDimensions(const FieldMatcher& matcher) {
    return HasPositionALL(matcher) || HasPrimitiveRepeatedField(matcher);
}

size_t getSize(const vector<FieldValue>& fieldValues) {
    size_t totalSize = 0;
    for (const FieldValue& fieldValue : fieldValues) {
        totalSize += fieldValue.getSize();
    }
    return totalSize;
}

size_t getFieldValuesSizeV2(const vector<FieldValue>& fieldValues) {
    size_t totalSize = 0;
    for (const FieldValue& fieldValue : fieldValues) {
        totalSize += fieldValue.getSizeV2();
    }
    return totalSize;
}

bool shouldKeepSample(const FieldValue& sampleFieldValue, int shardOffset, int shardCount) {
    int hashValue = 0;
    switch (sampleFieldValue.mValue.getType()) {
        case INT:
            hashValue =
                    Hash32(reinterpret_cast<const char*>(&sampleFieldValue.mValue.get<int32_t>()),
                           sizeof(sampleFieldValue.mValue.get<int32_t>()));
            break;
        case LONG:
            hashValue =
                    Hash32(reinterpret_cast<const char*>(&sampleFieldValue.mValue.get<int64_t>()),
                           sizeof(sampleFieldValue.mValue.get<int64_t>()));
            break;
        case FLOAT:
            hashValue = Hash32(reinterpret_cast<const char*>(&sampleFieldValue.mValue.get<float>()),
                               sizeof(sampleFieldValue.mValue.get<float>()));
            break;
        case DOUBLE:
            hashValue =
                    Hash32(reinterpret_cast<const char*>(&sampleFieldValue.mValue.get<double>()),
                           sizeof(sampleFieldValue.mValue.get<double>()));
            break;
        case STRING:
            hashValue = Hash32(sampleFieldValue.mValue.get<string>());
            break;
        case STORAGE:
            hashValue = Hash32((const char*)sampleFieldValue.mValue.get<vector<uint8_t>>().data(),
                               sampleFieldValue.mValue.get<vector<uint8_t>>().size());
            break;
        default:
            return true;
    }
    return (hashValue + shardOffset) % shardCount == 0;
}

}  // namespace statsd
}  // namespace os
}  // namespace android
