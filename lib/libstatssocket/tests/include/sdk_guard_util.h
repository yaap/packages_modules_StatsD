/*
 * Copyright (C) 2024, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define TEST_GUARDED_CLASS_NAME(suite_name, test_name) Guarded##suite_name##test_name

#define TEST_GUARDED_(test_type, suite_name, test_name, sdk)          \
    class TEST_GUARDED_CLASS_NAME(suite_name, test_name) {            \
    public:                                                           \
        static void doTest() __INTRODUCED_IN(sdk);                    \
    };                                                                \
                                                                      \
    test_type(suite_name, test_name) {                                \
        if (__builtin_available(android sdk, *)) {                    \
            TEST_GUARDED_CLASS_NAME(suite_name, test_name)::doTest(); \
        } else {                                                      \
            GTEST_SKIP();                                             \
        }                                                             \
    }                                                                 \
    void TEST_GUARDED_CLASS_NAME(suite_name, test_name)::doTest()

#define TEST_GUARDED(suite_name, test_name, sdk) TEST_GUARDED_(TEST, suite_name, test_name, sdk)

#define TEST_GUARDED_F_OR_P_(test_type, suite_name, test_name, sdk) \
    test_type(suite_name, test_name) {                              \
        if (__builtin_available(android sdk, *)) {                  \
            suite_name::do##test_name();                            \
        } else {                                                    \
            GTEST_SKIP();                                           \
        }                                                           \
    }                                                               \
    void suite_name::do##test_name()

#define TEST_F_GUARDED(suite_name, test_name, sdk) \
    TEST_GUARDED_F_OR_P_(TEST_F, suite_name, test_name, sdk)

#define TEST_P_GUARDED(suite_name, test_name, sdk) \
    TEST_GUARDED_F_OR_P_(TEST_P, suite_name, test_name, sdk)
