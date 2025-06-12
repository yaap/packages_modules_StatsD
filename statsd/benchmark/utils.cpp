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
#include "utils.h"

#include <cstdlib>
#include <ctime>
#include <limits>
#include <random>
#include <unordered_set>

namespace android {
namespace os {
namespace statsd {

std::vector<int> generateRandomIds(int count, int maxRange) {
    std::srand(std::time(nullptr));

    std::unordered_set<int> unique_values;

    while (unique_values.size() <= count) {
        unique_values.insert(std::rand() % maxRange);
    }

    std::vector<int> result(unique_values.begin(), unique_values.end());

    return result;
}

std::vector<int64_t> generateRandomHashIds(int count) {
    std::srand(std::time(nullptr));

    std::random_device rd;
    std::mt19937_64 eng(rd());

    std::uniform_int_distribution<int64_t> distr;

    std::unordered_set<int> unique_values;

    while (unique_values.size() <= count) {
        unique_values.insert(distr(eng));
    }

    std::vector<int64_t> result(unique_values.begin(), unique_values.end());

    return result;
}

}  // namespace statsd
}  // namespace os
}  // namespace android