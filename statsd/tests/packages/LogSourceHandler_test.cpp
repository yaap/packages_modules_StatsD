/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/packages/LogSourceHandler.h"

#include <gtest/gtest.h>
#include <private/android_filesystem_config.h>

namespace android {
namespace os {
namespace statsd {

TEST(LogSourceHandlerTest, TestEmptyLogSource) {
    sp<UidMap> uidMap = sp<UidMap>::make();
    LogSourceHandler handler({}, {}, uidMap);

    EXPECT_TRUE(handler.checkLogCredentials(AID_ROOT, 100));
    EXPECT_TRUE(handler.checkLogCredentials(AID_SYSTEM, 100));
    EXPECT_FALSE(handler.checkLogCredentials(10000, 100));
}

TEST(LogSourceHandlerTest, TestAids) {
    sp<UidMap> uidMap = sp<UidMap>::make();
    LogSourceHandler handler({"AID_STATSD", "AID_SHELL"}, {}, uidMap);

    EXPECT_TRUE(handler.checkLogCredentials(AID_STATSD, 100));
    EXPECT_TRUE(handler.checkLogCredentials(AID_SHELL, 100));
    EXPECT_FALSE(handler.checkLogCredentials(AID_NOBODY, 100));
}

TEST(LogSourceHandlerTest, TestPackages) {
    sp<UidMap> uidMap = sp<UidMap>::make();
    uidMap->updateApp(1, "pkg1", 10000, 1, "v1", "h1", {});
    uidMap->updateApp(1, "pkg1", 10001, 1, "v1", "h1", {});

    LogSourceHandler handler({"pkg1"}, {}, uidMap);

    EXPECT_TRUE(handler.checkLogCredentials(10000, 100));
    EXPECT_TRUE(handler.checkLogCredentials(10001, 100));
    EXPECT_FALSE(handler.checkLogCredentials(10002, 100));

    uidMap->updateApp(1, "pkg1", 10002, 1, "v1", "h1", {});
    handler.onAppChanged("pkg1");
    EXPECT_TRUE(handler.checkLogCredentials(10002, 100));
}

TEST(LogSourceHandlerTest, TestAllowlistedAtoms) {
    sp<UidMap> uidMap = sp<UidMap>::make();
    LogSourceHandler handler({}, {100}, uidMap);

    EXPECT_TRUE(handler.checkLogCredentials(10000, 100));
    EXPECT_FALSE(handler.checkLogCredentials(10000, 101));
}

TEST(LogSourceHandlerTest, TestCombined) {
    sp<UidMap> uidMap = sp<UidMap>::make();
    uidMap->updateApp(1, "pkg1", 10000, 1, "v1", "h1", {});
    LogSourceHandler handler({"AID_SHELL", "pkg1"}, {100}, uidMap);

    // Allowlisted atom
    EXPECT_TRUE(handler.checkLogCredentials(40000, 100));

    // System UIDs
    EXPECT_TRUE(handler.checkLogCredentials(AID_ROOT, 101));
    EXPECT_TRUE(handler.checkLogCredentials(AID_SYSTEM, 101));

    // Allowed AID
    EXPECT_TRUE(handler.checkLogCredentials(AID_SHELL, 101));

    // Allowed Package
    EXPECT_TRUE(handler.checkLogCredentials(10000, 101));

    // Not allowed
    EXPECT_FALSE(handler.checkLogCredentials(40000, 101));
}

}  // namespace statsd
}  // namespace os
}  // namespace android
