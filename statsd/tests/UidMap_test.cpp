// Copyright (C) 2017 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "packages/UidMap.h"

#include <android/util/ProtoOutputStream.h>
#include <gtest/gtest.h>
#include <private/android_filesystem_config.h>
#include <src/uid_data.pb.h>
#include <stdio.h>

#include "StatsLogProcessor.h"
#include "StatsService.h"
#include "config/ConfigKey.h"
#include "gtest_matchers.h"
#include "guardrail/StatsdStats.h"
#include "hash.h"
#include "logd/LogEvent.h"
#include "statsd_test_util.h"
#include "statslog_statsdtest.h"

using namespace android;

namespace android {
namespace os {
namespace statsd {

using aidl::android::util::StatsEventParcel;
using android::util::ProtoOutputStream;
using android::util::ProtoReader;
using ::ndk::SharedRefBase;
using Change = UidMapping_Change;

#ifdef __ANDROID__

namespace {
const string kApp1 = "app1.sharing.1";
const string kApp2 = "app2.sharing.1";
const string kApp3 = "app3";

const vector<int32_t> kUids{1000, 1000, 1500};
const vector<int64_t> kVersions{4, 5, 6};
const vector<string> kVersionStrings{"v1", "v1", "v2"};
const vector<string> kApps{kApp1, kApp2, kApp3};
const vector<string> kInstallers{"", "", "com.android.vending"};
const vector<vector<uint8_t>> kCertificateHashes{{'a', 'z'}, {'b', 'c'}, {'d', 'e'}};
const vector<uint8_t> kDeleted(3, false);

const UidMapOptions DEFAULT_OPTIONS = {.includeVersionStrings = true,
                                       .includeInstaller = true,
                                       .truncatedCertificateHashSize = 0,
                                       .omitSystemUids = false};

UidData createUidData(const vector<int32_t>& uids, const vector<int64_t>& versions,
                      const vector<string>& versionStrings, const vector<string>& apps,
                      const vector<string>& installers,
                      const vector<vector<uint8_t>>& certificateHashes) {
    // Populate UidData from app data.
    UidData uidData;
    for (size_t i = 0; i < uids.size(); i++) {
        ApplicationInfo* appInfo = uidData.add_app_info();
        appInfo->set_uid(uids[i]);
        appInfo->set_version(versions[i]);
        appInfo->set_version_string(versionStrings[i]);
        appInfo->set_package_name(apps[i]);
        appInfo->set_installer(installers[i]);
        appInfo->set_certificate_hash(certificateHashes[i].data(), certificateHashes[i].size());
    }
    return uidData;
}

void sendPackagesToStatsd(shared_ptr<StatsService> service, const vector<int32_t>& uids,
                          const vector<int64_t>& versions, const vector<string>& versionStrings,
                          const vector<string>& apps, const vector<string>& installers,
                          const vector<vector<uint8_t>>& certificateHashes) {
    // Create file descriptor from serialized UidData.
    // Create a file that lives in memory.
    ScopedFileDescriptor scopedFd(memfd_create("doesn't matter", MFD_CLOEXEC));
    const int fd = scopedFd.get();
    int f = fcntl(fd, F_GETFD);  // Read the file descriptor flags.
    ASSERT_NE(-1, f);            // Ensure there was no error while reading file descriptor flags.
    ASSERT_TRUE(f & FD_CLOEXEC);

    UidData uidData =
            createUidData(uids, versions, versionStrings, apps, installers, certificateHashes);
    ASSERT_TRUE(uidData.SerializeToFileDescriptor(fd));
    ASSERT_EQ(0, lseek(fd, 0, SEEK_SET));

    // Send file descriptor containing app data to statsd.
    service->informAllUidData(scopedFd);
}

// Returns a vector of the same length as values param. Each i-th element in the returned vector is
// the index at which values[i] appears in the list denoted by the begin and end iterators.
template <typename Iterator, typename ValueType>
vector<uint32_t> computeIndices(const Iterator begin, const Iterator end,
                                const vector<ValueType>& values) {
    vector<uint32_t> indices;
    for (const ValueType& value : values) {
        Iterator it = find(begin, end, value);
        indices.emplace_back(distance(begin, it));
    }
    return indices;
}

class UidMapTestAppendUidMapBase : public Test {
protected:
    const ConfigKey cfgKey;
    const sp<UidMap> uidMap;

    UidMapTestAppendUidMapBase() : cfgKey(1, StringToId("config1")), uidMap(new UidMap()) {
    }
};

}  // anonymous namespace

TEST(UidMapTest, TestIsolatedUID) {
    sp<UidMap> m = new UidMap();
    sp<StatsPullerManager> pullerManager = new StatsPullerManager();
    sp<AlarmMonitor> anomalyAlarmMonitor;
    sp<AlarmMonitor> subscriberAlarmMonitor;
    // Construct the processor with a no-op sendBroadcast function that does nothing.
    StatsLogProcessor p(
            m, pullerManager, anomalyAlarmMonitor, subscriberAlarmMonitor, 0,
            [](const ConfigKey& key) { return true; },
            [](const int&, const vector<int64_t>&) { return true; },
            [](const ConfigKey&, const string&, const vector<int64_t>&) {},
            std::make_shared<LogEventFilter>());

    std::unique_ptr<LogEvent> addEvent = CreateIsolatedUidChangedEvent(
            1 /*timestamp*/, 100 /*hostUid*/, 101 /*isolatedUid*/, 1 /*is_create*/);
    EXPECT_EQ(101, m->getHostUidOrSelf(101));
    p.OnLogEvent(addEvent.get());
    EXPECT_EQ(100, m->getHostUidOrSelf(101));

    std::unique_ptr<LogEvent> removeEvent = CreateIsolatedUidChangedEvent(
            1 /*timestamp*/, 100 /*hostUid*/, 101 /*isolatedUid*/, 0 /*is_create*/);
    p.OnLogEvent(removeEvent.get());
    EXPECT_EQ(101, m->getHostUidOrSelf(101));
}

TEST(UidMapTest, TestUpdateMap) {
    const sp<UidMap> uidMap = new UidMap();
    const shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    sendPackagesToStatsd(service, kUids, kVersions, kVersionStrings, kApps, kInstallers,
                         kCertificateHashes);

    EXPECT_TRUE(uidMap->hasApp(1000, kApp1));
    EXPECT_TRUE(uidMap->hasApp(1000, kApp2));
    EXPECT_TRUE(uidMap->hasApp(1500, kApp3));
    EXPECT_FALSE(uidMap->hasApp(1000, "not.app"));

    std::set<string> name_set = uidMap->getAppNamesFromUid(1000u, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre(kApp1, kApp2));

    name_set = uidMap->getAppNamesFromUid(1500u, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre(kApp3));

    name_set = uidMap->getAppNamesFromUid(12345, true /* returnNormalized */);
    EXPECT_THAT(name_set, IsEmpty());

    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(kApps, kUids, kVersions, kVersionStrings, kInstallers,
                              kCertificateHashes, kDeleted, /* installerIndices */ {},
                              /* hashStrings */ false);

    PackageInfoSnapshot packageInfoSnapshot = getPackageInfoSnapshot(uidMap);

    EXPECT_THAT(packageInfoSnapshot.package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

TEST(UidMapTest, TestUpdateMapMultiple) {
    const sp<UidMap> uidMap = new UidMap();
    const shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    sendPackagesToStatsd(service, kUids, kVersions, kVersionStrings, kApps, kInstallers,
                         kCertificateHashes);

    // Remove kApp3, and add NewApp
    vector<int32_t> uids(kUids);
    uids.back() = 2000;
    vector<string> apps(kApps);
    apps.back() = "NewApp";
    vector<string> installers(kInstallers);
    installers.back() = "NewInstaller";

    sendPackagesToStatsd(service, uids, kVersions, kVersionStrings, apps, installers,
                         kCertificateHashes);

    EXPECT_TRUE(uidMap->hasApp(1000, kApp1));
    EXPECT_TRUE(uidMap->hasApp(1000, kApp2));
    EXPECT_TRUE(uidMap->hasApp(2000, "NewApp"));
    EXPECT_FALSE(uidMap->hasApp(1500, kApp3));
    EXPECT_FALSE(uidMap->hasApp(1000, "not.app"));

    std::set<string> name_set = uidMap->getAppNamesFromUid(1000u, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre(kApp1, kApp2));

    name_set = uidMap->getAppNamesFromUid(2000, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre("newapp"));

    name_set = uidMap->getAppNamesFromUid(1500, true /* returnNormalized */);
    EXPECT_THAT(name_set, IsEmpty());

    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(apps, uids, kVersions, kVersionStrings, installers,
                              kCertificateHashes, kDeleted, /* installerIndices */ {},
                              /* hashStrings */ false);

    PackageInfoSnapshot packageInfoSnapshot = getPackageInfoSnapshot(uidMap);

    EXPECT_THAT(packageInfoSnapshot.package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

TEST(UidMapTest, TestRemoveApp) {
    const sp<UidMap> uidMap = new UidMap();
    const shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    sendPackagesToStatsd(service, kUids, kVersions, kVersionStrings, kApps, kInstallers,
                         kCertificateHashes);

    service->informOnePackageRemoved(kApp1, 1000);
    EXPECT_FALSE(uidMap->hasApp(1000, kApp1));
    EXPECT_TRUE(uidMap->hasApp(1000, kApp2));
    EXPECT_TRUE(uidMap->hasApp(1500, kApp3));
    std::set<string> name_set = uidMap->getAppNamesFromUid(1000, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre(kApp2));

    vector<uint8_t> deleted(kDeleted);
    deleted[0] = true;
    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(kApps, kUids, kVersions, kVersionStrings, kInstallers,
                              kCertificateHashes, deleted, /* installerIndices */ {},
                              /* hashStrings */ false);
    PackageInfoSnapshot packageInfoSnapshot = getPackageInfoSnapshot(uidMap);
    EXPECT_THAT(packageInfoSnapshot.package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));

    service->informOnePackageRemoved(kApp2, 1000);
    EXPECT_FALSE(uidMap->hasApp(1000, kApp1));
    EXPECT_FALSE(uidMap->hasApp(1000, kApp2));
    EXPECT_TRUE(uidMap->hasApp(1500, kApp3));
    EXPECT_FALSE(uidMap->hasApp(1000, "not.app"));
    name_set = uidMap->getAppNamesFromUid(1000, true /* returnNormalized */);
    EXPECT_THAT(name_set, IsEmpty());

    deleted[1] = true;
    expectedPackageInfos = buildPackageInfos(kApps, kUids, kVersions, kVersionStrings, kInstallers,
                                             kCertificateHashes, deleted, /* installerIndices */ {},
                                             /* hashStrings */ false);
    packageInfoSnapshot = getPackageInfoSnapshot(uidMap);
    EXPECT_THAT(packageInfoSnapshot.package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));

    service->informOnePackageRemoved(kApp3, 1500);
    EXPECT_FALSE(uidMap->hasApp(1000, kApp1));
    EXPECT_FALSE(uidMap->hasApp(1000, kApp2));
    EXPECT_FALSE(uidMap->hasApp(1500, kApp3));
    EXPECT_FALSE(uidMap->hasApp(1000, "not.app"));
    name_set = uidMap->getAppNamesFromUid(1500, true /* returnNormalized */);
    EXPECT_THAT(name_set, IsEmpty());

    deleted[2] = true;
    expectedPackageInfos = buildPackageInfos(kApps, kUids, kVersions, kVersionStrings, kInstallers,
                                             kCertificateHashes, deleted, /* installerIndices */ {},
                                             /* hashStrings */ false);
    packageInfoSnapshot = getPackageInfoSnapshot(uidMap);
    EXPECT_THAT(packageInfoSnapshot.package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

TEST(UidMapTest, TestUpdateApp) {
    const sp<UidMap> uidMap = new UidMap();
    const shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    sendPackagesToStatsd(service, kUids, kVersions, kVersionStrings, kApps, kInstallers,
                         kCertificateHashes);

    // Update app1 version.
    service->informOnePackage(kApps[0].c_str(), kUids[0], /* version */ 40,
                              /* versionString */ "v40", kInstallers[0], kCertificateHashes[0]);
    EXPECT_THAT(uidMap->getAppVersion(kUids[0], kApps[0]), Eq(40));
    std::set<string> name_set = uidMap->getAppNamesFromUid(1000, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre(kApp1, kApp2));

    // Add a new name for uid 1000.
    service->informOnePackage("NeW_aPP1_NAmE", 1000, /* version */ 40,
                              /* versionString */ "v40", /* installer */ "com.android.vending",
                              /* certificateHash */ {'a'});
    name_set = uidMap->getAppNamesFromUid(1000, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre(kApp1, kApp2, "new_app1_name"));

    // Re-add the same name for another uid 2000
    service->informOnePackage("NeW_aPP1_NAmE", 2000, /* version */ 1,
                              /* versionString */ "v1", /* installer */ "",
                              /* certificateHash */ {'b'});
    name_set = uidMap->getAppNamesFromUid(2000, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre("new_app1_name"));

    // Re-add existing package with different installer
    service->informOnePackage("NeW_aPP1_NAmE", 2000, /* version */ 1,
                              /* versionString */ "v1", /* installer */ "new_installer",
                              /* certificateHash */ {'b'});
    name_set = uidMap->getAppNamesFromUid(2000, true /* returnNormalized */);
    EXPECT_THAT(name_set, UnorderedElementsAre("new_app1_name"));

    vector<int32_t> uids = concatenate(kUids, {1000, 2000});
    vector<int64_t> versions = concatenate(kVersions, {40, 1});
    versions[0] = 40;
    vector<string> versionStrings = concatenate(kVersionStrings, {"v40", "v1"});
    versionStrings[0] = "v40";
    vector<string> apps = concatenate(kApps, {"NeW_aPP1_NAmE", "NeW_aPP1_NAmE"});
    vector<string> installers = concatenate(kInstallers, {"com.android.vending", "new_installer"});
    vector<uint8_t> deleted = concatenate(kDeleted, {false, false});
    vector<vector<uint8_t>> certHashes = concatenate(kCertificateHashes, {{'a'}, {'b'}});
    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(apps, uids, versions, versionStrings, installers, certHashes, deleted,
                              /* installerIndices */ {},
                              /* hashStrings */ false);

    PackageInfoSnapshot packageInfoSnapshot = getPackageInfoSnapshot(uidMap);
    EXPECT_THAT(packageInfoSnapshot.package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

// Test that uid map returns at least one snapshot even if we already obtained
// this snapshot from a previous call to getData.
TEST(UidMapTest, TestOutputIncludesAtLeastOneSnapshot) {
    UidMap m;
    // Initialize single config key.
    ConfigKey config1(1, StringToId("config1"));
    m.OnConfigUpdated(config1);

    UidData uidData;
    *uidData.add_app_info() = createApplicationInfo(/*uid*/ 1000, /*version*/ 5, "v1", kApp2);

    m.updateMap(1 /* timestamp */, uidData);

    // Set the last timestamp for this config key to be newer.
    m.mLastUpdatePerConfigKey[config1] = 2;

    ProtoOutputStream proto;
    m.appendUidMap(/* timestamp */ 3, config1, DEFAULT_OPTIONS, /* str_set */ nullptr, &proto);

    // Check there's still a uidmap attached this one.
    UidMapping results;
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots_size());
    EXPECT_EQ("v1", results.snapshots(0).package_info(0).version_string());
}

TEST(UidMapTest, TestRemovedAppRetained) {
    UidMap m;
    // Initialize single config key.
    ConfigKey config1(1, StringToId("config1"));
    m.OnConfigUpdated(config1);

    UidData uidData;
    *uidData.add_app_info() = createApplicationInfo(/*uid*/ 1000, /*version*/ 5, "v5", kApp2);

    m.updateMap(1 /* timestamp */, uidData);
    m.removeApp(2, kApp2, 1000);

    ProtoOutputStream proto;
    m.appendUidMap(/* timestamp */ 3, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);

    // Snapshot should still contain this item as deleted.
    UidMapping results;
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots(0).package_info_size());
    EXPECT_EQ(true, results.snapshots(0).package_info(0).deleted());
}

TEST(UidMapTest, TestRemovedAppOverGuardrail) {
    UidMap m;
    // Initialize single config key.
    ConfigKey config1(1, StringToId("config1"));
    m.OnConfigUpdated(config1);
    const int maxDeletedApps = StatsdStats::kMaxDeletedAppsInUidMap;

    UidData uidData;
    for (int j = 0; j < maxDeletedApps + 10; j++) {
        *uidData.add_app_info() = createApplicationInfo(/*uid*/ j, /*version*/ j, "v", kApp1);
    }
    m.updateMap(1 /* timestamp */, uidData);

    // First, verify that we have the expected number of items.
    UidMapping results;
    ProtoOutputStream proto;
    m.appendUidMap(/* timestamp */ 3, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(maxDeletedApps + 10, results.snapshots(0).package_info_size());

    // Now remove all the apps.
    m.updateMap(1 /* timestamp */, uidData);
    for (int j = 0; j < maxDeletedApps + 10; j++) {
        m.removeApp(4, kApp1, j);
    }

    proto.clear();
    m.appendUidMap(/* timestamp */ 5, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    // Snapshot drops the first nine items.
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(maxDeletedApps, results.snapshots(0).package_info_size());
}

TEST(UidMapTest, TestClearingOutput) {
    UidMap m;

    ConfigKey config1(1, StringToId("config1"));
    ConfigKey config2(1, StringToId("config2"));

    m.OnConfigUpdated(config1);

    UidData uidData;
    *uidData.add_app_info() = createApplicationInfo(/*uid*/ 1000, /*version*/ 4, "v4", kApp1);
    *uidData.add_app_info() = createApplicationInfo(/*uid*/ 1000, /*version*/ 5, "v5", kApp2);
    m.updateMap(1 /* timestamp */, uidData);

    ProtoOutputStream proto;
    m.appendUidMap(/* timestamp */ 2, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    UidMapping results;
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots_size());

    // We have to keep at least one snapshot in memory at all times.
    proto.clear();
    m.appendUidMap(/* timestamp */ 2, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots_size());

    // Now add another configuration.
    m.OnConfigUpdated(config2);
    m.updateApp(5, kApp1, 1000, 40, "v40", "", /* certificateHash */ {});
    ASSERT_EQ(1U, m.mChanges.size());
    proto.clear();
    m.appendUidMap(/* timestamp */ 6, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots_size());
    ASSERT_EQ(1, results.changes_size());
    ASSERT_EQ(1U, m.mChanges.size());

    // Add another delta update.
    m.updateApp(7, kApp2, 1001, 41, "v41", "", /* certificateHash */ {});
    ASSERT_EQ(2U, m.mChanges.size());

    // We still can't remove anything.
    proto.clear();
    m.appendUidMap(/* timestamp */ 8, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots_size());
    ASSERT_EQ(1, results.changes_size());
    ASSERT_EQ(2U, m.mChanges.size());

    proto.clear();
    m.appendUidMap(/* timestamp */ 9, config2, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    outputStreamToProto(&proto, &results);
    ASSERT_EQ(1, results.snapshots_size());
    ASSERT_EQ(2, results.changes_size());
    // At this point both should be cleared.
    ASSERT_EQ(0U, m.mChanges.size());
}

TEST(UidMapTest, TestMemoryComputed) {
    UidMap m;

    ConfigKey config1(1, StringToId("config1"));
    m.OnConfigUpdated(config1);

    size_t startBytes = m.mBytesUsed;
    UidData uidData;
    *uidData.add_app_info() = createApplicationInfo(/*uid*/ 1000, /*version*/ 1, "v1", kApp1);
    m.updateMap(1 /* timestamp */, uidData);

    m.updateApp(3, kApp1, 1000, 40, "v40", "", /* certificateHash */ {});

    ProtoOutputStream proto;
    m.appendUidMap(/* timestamp */ 2, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    size_t prevBytes = m.mBytesUsed;

    m.appendUidMap(/* timestamp */ 4, config1, DEFAULT_OPTIONS,
                   /* str_set */ nullptr, &proto);
    EXPECT_TRUE(m.mBytesUsed < prevBytes);
}

TEST(UidMapTest, TestMemoryGuardrail) {
    UidMap m;
    string buf;

    ConfigKey config1(1, StringToId("config1"));
    m.OnConfigUpdated(config1);

    size_t startBytes = m.mBytesUsed;
    UidData uidData;
    for (int i = 0; i < 100; i++) {
        buf = "EXTREMELY_LONG_STRING_FOR_APP_TO_WASTE_MEMORY." + to_string(i);
        *uidData.add_app_info() = createApplicationInfo(/*uid*/ 1, /*version*/ 1, "v1", buf);
    }
    m.updateMap(1 /* timestamp */, uidData);

    m.updateApp(3, "EXTREMELY_LONG_STRING_FOR_APP_TO_WASTE_MEMORY.0", 1000, 2, "v2", "",
                /* certificateHash */ {});
    ASSERT_EQ(1U, m.mChanges.size());

    // Now force deletion by limiting the memory to hold one delta change.
    m.maxBytesOverride = 120; // Since the app string alone requires >45 characters.
    m.updateApp(5, "EXTREMELY_LONG_STRING_FOR_APP_TO_WASTE_MEMORY.0", 1000, 4, "v4", "",
                /* certificateHash */ {});
    ASSERT_EQ(1U, m.mChanges.size());
}

namespace {
class UidMapTestAppendUidMap : public UidMapTestAppendUidMapBase {
protected:
    const shared_ptr<StatsService> service;

    set<string> installersSet;
    set<uint64_t> installerHashSet;
    vector<uint64_t> installerHashes;

    UidMapTestAppendUidMap()
        : UidMapTestAppendUidMapBase(),
          service(SharedRefBase::make<StatsService>(uidMap, /* queue */ nullptr,
                                                    std::make_shared<LogEventFilter>())) {
    }

    void SetUp() override {
        sendPackagesToStatsd(service, kUids, kVersions, kVersionStrings, kApps, kInstallers,
                             kCertificateHashes);

        for (const string& installer : kInstallers) {
            installersSet.insert(installer);
            uint64_t installerHash = Hash64(installer);
            installerHashes.emplace_back(installerHash);
            installerHashSet.insert(installerHash);
        }
    }
};

TEST_F(UidMapTestAppendUidMap, TestInstallersInReportIncludeInstallerAndHashStrings) {
    ProtoOutputStream proto;
    set<string> strSet;
    uidMap->appendUidMap(/* timestamp */ 3, cfgKey, DEFAULT_OPTIONS, &strSet, &proto);

    UidMapping results;
    outputStreamToProto(&proto, &results);

    // Verify hashes for all installers are in the installer_hash list.
    EXPECT_THAT(results.installer_hash(), UnorderedElementsAreArray(installerHashSet));

    EXPECT_THAT(results.installer_name(), IsEmpty());

    // Verify all installer names are added to the strSet argument.
    EXPECT_THAT(strSet, IsSupersetOf(installersSet));

    ASSERT_THAT(results.snapshots_size(), Eq(1));

    // Compute installer indices for each package.
    // Find the location of each installerHash from the input in the results.
    // installerIndices[i] is the index in results.installer_hash() that matches installerHashes[i].
    vector<uint32_t> installerIndices = computeIndices(
            results.installer_hash().begin(), results.installer_hash().end(), installerHashes);

    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(kApps, kUids, kVersions, kVersionStrings, kInstallers,
                              /* certHashes */ {}, kDeleted, installerIndices,
                              /* hashStrings */ true);

    EXPECT_THAT(strSet, IsSupersetOf(kApps));

    EXPECT_THAT(results.snapshots(0).package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

TEST_F(UidMapTestAppendUidMap, TestInstallersInReportIncludeInstallerAndDontHashStrings) {
    ProtoOutputStream proto;
    uidMap->appendUidMap(/* timestamp */ 3, cfgKey, DEFAULT_OPTIONS,
                         /* str_set */ nullptr, &proto);

    UidMapping results;
    outputStreamToProto(&proto, &results);

    // Verify all installers are in the installer_name list.
    EXPECT_THAT(results.installer_name(), UnorderedElementsAreArray(installersSet));

    EXPECT_THAT(results.installer_hash(), IsEmpty());

    ASSERT_THAT(results.snapshots_size(), Eq(1));

    vector<uint32_t> installerIndices = computeIndices(results.installer_name().begin(),
                                                       results.installer_name().end(), kInstallers);

    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(kApps, kUids, kVersions, kVersionStrings, kInstallers,
                              /* certHashes */ {}, kDeleted, installerIndices,
                              /* hashStrings */ false);

    EXPECT_THAT(results.snapshots(0).package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

// Set up parameterized test with set<string>* parameter to control whether strings are hashed
// or not in the report. A value of nullptr indicates strings should not be hashed and non-null
// values indicates strings are hashed in the report and the original strings are added to this set.
class UidMapTestAppendUidMapHashStrings : public UidMapTestAppendUidMap,
                                          public WithParamInterface<set<string>*> {
public:
    inline static set<string> strSet;

protected:
    void SetUp() override {
        strSet.clear();
    }
};

INSTANTIATE_TEST_SUITE_P(
        HashStrings, UidMapTestAppendUidMapHashStrings,
        Values(nullptr, &(UidMapTestAppendUidMapHashStrings::strSet)),
        [](const TestParamInfo<UidMapTestAppendUidMapHashStrings::ParamType>& info) {
            return info.param == nullptr ? "NoHashStrings" : "HashStrings";
        });

TEST_P(UidMapTestAppendUidMapHashStrings, TestNoIncludeInstallersInReport) {
    ProtoOutputStream proto;
    UidMapOptions options = DEFAULT_OPTIONS;
    options.includeInstaller = false;
    uidMap->appendUidMap(/* timestamp */ 3, cfgKey, options,
                         /* str_set */ GetParam(), &proto);

    UidMapping results;
    outputStreamToProto(&proto, &results);

    // Verify installer lists are empty.
    EXPECT_THAT(results.installer_name(), IsEmpty());
    EXPECT_THAT(results.installer_hash(), IsEmpty());

    ASSERT_THAT(results.snapshots_size(), Eq(1));

    // Verify that none of installer, installer_hash, installer_index fields in PackageInfo are
    // populated.
    EXPECT_THAT(results.snapshots(0).package_info(),
                Each(Property(&PackageInfo::has_installer, IsFalse())));
    EXPECT_THAT(results.snapshots(0).package_info(),
                Each(Property(&PackageInfo::has_installer_hash, IsFalse())));
    EXPECT_THAT(results.snapshots(0).package_info(),
                Each(Property(&PackageInfo::has_installer_index, IsFalse())));
}

// Set up parameterized test for testing with different truncation hash sizes for the certificates.
class UidMapTestTruncateCertificateHash : public UidMapTestAppendUidMap,
                                          public WithParamInterface<uint8_t> {};

INSTANTIATE_TEST_SUITE_P(ZeroOneTwoThree, UidMapTestTruncateCertificateHash,
                         Range(uint8_t{0}, uint8_t{4}));

TEST_P(UidMapTestTruncateCertificateHash, TestCertificateHashesTruncated) {
    const uint8_t hashSize = GetParam();
    ProtoOutputStream proto;
    UidMapOptions options = DEFAULT_OPTIONS;
    options.includeInstaller = false;
    options.truncatedCertificateHashSize = hashSize;
    uidMap->appendUidMap(/* timestamp */ 3, cfgKey, options,
                         /* str_set */ nullptr, &proto);

    UidMapping results;
    outputStreamToProto(&proto, &results);

    ASSERT_THAT(results.snapshots_size(), Eq(1));

    vector<vector<uint8_t>> certHashes = kCertificateHashes;
    for (vector<uint8_t>& certHash : certHashes) {
        certHash.resize(certHash.size() < hashSize ? certHash.size() : hashSize);
    }
    vector<PackageInfo> expectedPackageInfos =
            buildPackageInfos(kApps, kUids, kVersions, kVersionStrings,
                              /* installers */ {}, certHashes, kDeleted,
                              /* installerIndices*/ {},
                              /* hashStrings */ false);

    EXPECT_THAT(results.snapshots(0).package_info(),
                UnorderedPointwise(EqPackageInfo(), expectedPackageInfos));
}

class UidMapTestAppendUidMapSystemUsedUids : public UidMapTestAppendUidMapBase {
protected:
    static const uint64_t bucketStartTimeNs = 10000000000;  // 0:10
    uint64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(TEN_MINUTES) * 1000000LL;
    StatsdConfig config;

    void SetUp() override {
        UidData uidData =
                createUidData({AID_LMKD, AID_APP_START + 1, AID_USER_OFFSET + AID_UWB,
                               AID_USER_OFFSET + AID_APP_START + 2, AID_ROOT, AID_APP_START - 1,
                               AID_APP_START} /* uids */,
                              {1, 2, 3, 4, 5, 6, 7} /* versions */,
                              {"v1", "v2", "v3", "v4", "v5", "v6", "v7"} /* versionStrings */,
                              {"LMKD", "app1", "UWB", "app2", "root", "app3", "app4"} /* apps */,
                              vector(7, string("installer")) /* installers */,
                              vector(7, vector<uint8_t>{}) /* certificateHashes */);

        uidMap->updateMap(/* timestamp */ 1, uidData);

        uidMap->updateApp(/* timestamp */ 5, "LMKD", AID_LMKD, /* versionCode */ 10, "v10",
                          /* installer */ "", /* certificateHash */ {});
        uidMap->updateApp(/* timestamp */ 6, "UWB", AID_USER_OFFSET + AID_UWB, /* versionCode */ 20,
                          "v20", /* installer */ "", /* certificateHash */ {});
        uidMap->updateApp(/* timestamp */ 7, "root", AID_ROOT, /* versionCode */ 50, "v50",
                          /* installer */ "", /* certificateHash */ {});
        uidMap->updateApp(/* timestamp */ 8, "app3", AID_APP_START - 1, /* versionCode */ 60, "v60",
                          /* installer */ "", /* certificateHash */ {});
        uidMap->updateApp(/* timestamp */ 9, "app4", AID_APP_START, /* versionCode */ 70, "v70",
                          /* installer */ "", /* certificateHash */ {});

        *config.add_atom_matcher() =
                CreateSimpleAtomMatcher("TestAtomMatcher", util::SYNC_STATE_CHANGED);
        *config.add_event_metric() =
                createEventMetric("TestAtomReported", config.atom_matcher(0).id(), nullopt);
    }

    inline sp<StatsLogProcessor> createStatsLogProcessor(const StatsdConfig& config) const {
        return CreateStatsLogProcessor(bucketStartTimeNs, bucketStartTimeNs, config, cfgKey,
                                       /* puller */ nullptr, /* puller atomTag */ 0, uidMap);
    }

    UidMapping getUidMapping(const sp<StatsLogProcessor>& processor) const {
        vector<uint8_t> buffer;
        processor->onDumpReport(cfgKey, bucketStartTimeNs + bucketSizeNs + 1, false, true, ADB_DUMP,
                                FAST, &buffer);
        ConfigMetricsReportList reports;
        reports.ParseFromArray(&buffer[0], buffer.size());
        return reports.reports(0).uid_map();
    }
};

TEST_F(UidMapTestAppendUidMapSystemUsedUids, testHasSystemAndUnusedUids) {
    sp<StatsLogProcessor> processor = createStatsLogProcessor(config);
    UidMapping results = getUidMapping(processor);

    ASSERT_EQ(results.snapshots_size(), 1);
    EXPECT_THAT(
            results.snapshots(0).package_info(),
            UnorderedElementsAre(Property(&PackageInfo::uid, AID_LMKD),
                                 Property(&PackageInfo::uid, AID_USER_OFFSET + AID_UWB),
                                 Property(&PackageInfo::uid, AID_ROOT),
                                 Property(&PackageInfo::uid, AID_APP_START - 1),
                                 Property(&PackageInfo::uid, AID_APP_START),
                                 Property(&PackageInfo::uid, AID_APP_START + 1),
                                 Property(&PackageInfo::uid, AID_USER_OFFSET + AID_APP_START + 2)));

    EXPECT_THAT(results.changes(),
                UnorderedElementsAre(Property(&Change::uid, AID_LMKD),
                                     Property(&Change::uid, AID_USER_OFFSET + AID_UWB),
                                     Property(&Change::uid, AID_ROOT),
                                     Property(&Change::uid, AID_APP_START - 1),
                                     Property(&Change::uid, AID_APP_START)));
}

TEST_F(UidMapTestAppendUidMapSystemUsedUids, testHasNoSystemUids) {
    config.mutable_statsd_config_options()->set_omit_system_uids_in_uidmap(true);
    sp<StatsLogProcessor> processor = createStatsLogProcessor(config);
    UidMapping results = getUidMapping(processor);

    ASSERT_EQ(results.snapshots_size(), 1);
    EXPECT_THAT(results.snapshots(0).package_info(),
                Each(Property(&PackageInfo::uid,
                              AllOf(Not(Eq(AID_LMKD)), Not(Eq(AID_USER_OFFSET + AID_UWB)),
                                    Not(Eq(AID_ROOT)), Not(Eq(AID_APP_START - 1))))));

    EXPECT_THAT(results.changes(), ElementsAre(Property(&Change::uid, AID_APP_START)));
}

TEST_F(UidMapTestAppendUidMapSystemUsedUids, testOmitSystemAndUnusedUidsEmpty) {
    config.mutable_statsd_config_options()->set_omit_system_uids_in_uidmap(true);
    config.mutable_statsd_config_options()->set_omit_unused_uids_in_uidmap(true);

    sp<StatsLogProcessor> processor = createStatsLogProcessor(config);
    UidMapping results = getUidMapping(processor);

    ASSERT_EQ(results.snapshots_size(), 1);
    ASSERT_EQ(results.snapshots(0).package_info_size(), 0);
    ASSERT_EQ(results.changes_size(), 0);
}

TEST_F(UidMapTestAppendUidMapSystemUsedUids, testOmitSystemAndUnusedUids) {
    config.mutable_statsd_config_options()->set_omit_system_uids_in_uidmap(true);
    config.mutable_statsd_config_options()->set_omit_unused_uids_in_uidmap(true);

    sp<StatsLogProcessor> processor = createStatsLogProcessor(config);

    auto event = CreateSyncStartEvent(bucketStartTimeNs + 1, {AID_LMKD, AID_APP_START + 1},
                                      {"tag", "tag"}, "sync_name");
    processor->OnLogEvent(event.get());

    UidMapping results = getUidMapping(processor);

    ASSERT_EQ(results.snapshots_size(), 1);
    EXPECT_THAT(results.snapshots(0).package_info(),
                UnorderedElementsAre(Property(&PackageInfo::uid, AID_APP_START + 1)));
    ASSERT_EQ(results.changes_size(), 0);
}

TEST_F(UidMapTestAppendUidMapSystemUsedUids, testOmitSystemAndUnusedUidsEmptyWithAllowlist) {
    config.mutable_statsd_config_options()->set_omit_system_uids_in_uidmap(true);
    config.mutable_statsd_config_options()->set_omit_unused_uids_in_uidmap(true);
    config.mutable_statsd_config_options()->add_uidmap_package_allowlist("LMKD");
    config.mutable_statsd_config_options()->add_uidmap_package_allowlist("app4");

    sp<StatsLogProcessor> processor = createStatsLogProcessor(config);
    UidMapping results = getUidMapping(processor);

    ASSERT_EQ(results.snapshots_size(), 1);
    EXPECT_THAT(results.snapshots(0).package_info(),
                UnorderedElementsAre(Property(&PackageInfo::uid, AID_LMKD),
                                     Property(&PackageInfo::uid, AID_APP_START)));
    EXPECT_THAT(results.changes(), UnorderedElementsAre(Property(&Change::uid, AID_LMKD),
                                                        Property(&Change::uid, AID_APP_START)));
}

TEST_F(UidMapTestAppendUidMapSystemUsedUids, testOmitSystemAndUnusedUidsWithAllowlist) {
    config.mutable_statsd_config_options()->set_omit_system_uids_in_uidmap(true);
    config.mutable_statsd_config_options()->set_omit_unused_uids_in_uidmap(true);
    config.mutable_statsd_config_options()->add_uidmap_package_allowlist("LMKD");
    config.mutable_statsd_config_options()->add_uidmap_package_allowlist("app1");

    sp<StatsLogProcessor> processor = createStatsLogProcessor(config);
    auto event = CreateSyncStartEvent(bucketStartTimeNs + 1, {AID_ROOT, AID_LMKD, AID_APP_START},
                                      {"tag", "tag", "tag"}, "sync_name");
    processor->OnLogEvent(event.get());
    UidMapping results = getUidMapping(processor);

    ASSERT_EQ(results.snapshots_size(), 1);
    EXPECT_THAT(results.snapshots(0).package_info(),
                UnorderedElementsAre(Property(&PackageInfo::uid, AID_LMKD),
                                     Property(&PackageInfo::uid, AID_APP_START),
                                     Property(&PackageInfo::uid, AID_APP_START + 1)));
    EXPECT_THAT(results.changes(), UnorderedElementsAre(Property(&Change::uid, AID_LMKD),
                                                        Property(&Change::uid, AID_APP_START)));
}

TEST(UidMapTest, TestUsedUidsE2e) {
    const int ATOM_1 = 1, ATOM_2 = 2, ATOM_3 = 3, ATOM_4 = 4, ATOM_5 = 10001, ATOM_6 = 6;
    StatsdConfig config;
    config.mutable_statsd_config_options()->set_omit_unused_uids_in_uidmap(true);
    config.add_default_pull_packages("AID_ROOT");  // Fake puller is registered with root.
    AtomMatcher eventMatcher = CreateSimpleAtomMatcher("M1", ATOM_1);
    *config.add_atom_matcher() = eventMatcher;
    AtomMatcher countMatcher = CreateSimpleAtomMatcher("M2", ATOM_2);
    *config.add_atom_matcher() = countMatcher;
    AtomMatcher durationStartMatcher = CreateSimpleAtomMatcher("M3_START", ATOM_3);
    auto fvmStart = durationStartMatcher.mutable_simple_atom_matcher()->add_field_value_matcher();
    fvmStart->set_field(2);  // State field.
    fvmStart->set_eq_int(0);
    *config.add_atom_matcher() = durationStartMatcher;
    AtomMatcher durationStopMatcher = CreateSimpleAtomMatcher("M3_STOP", ATOM_3);
    auto fvmStop = durationStopMatcher.mutable_simple_atom_matcher()->add_field_value_matcher();
    fvmStop->set_field(2);
    fvmStop->set_eq_int(1);
    *config.add_atom_matcher() = durationStopMatcher;
    AtomMatcher gaugeMatcher = CreateSimpleAtomMatcher("M4", ATOM_4);
    *config.add_atom_matcher() = gaugeMatcher;
    AtomMatcher valueMatcher = CreateSimpleAtomMatcher("M5", ATOM_5);
    *config.add_atom_matcher() = valueMatcher;
    AtomMatcher kllMatcher = CreateSimpleAtomMatcher("M6", ATOM_6);
    *config.add_atom_matcher() = kllMatcher;

    Predicate predicate;
    predicate.set_id(StringToId("P1"));
    predicate.mutable_simple_predicate()->set_start(StringToId("M3_START"));
    predicate.mutable_simple_predicate()->set_stop(StringToId("M3_STOP"));
    FieldMatcher durDims = CreateDimensions(ATOM_3, {1});
    *predicate.mutable_simple_predicate()->mutable_dimensions() = durDims;
    *config.add_predicate() = predicate;

    *config.add_event_metric() = createEventMetric("EVENT", eventMatcher.id(), nullopt);
    CountMetric countMetric = createCountMetric("COUNT", countMatcher.id(), nullopt, {});
    *countMetric.mutable_dimensions_in_what() =
            CreateAttributionUidDimensions(ATOM_2, {Position::FIRST});
    *config.add_count_metric() = countMetric;
    DurationMetric durationMetric = createDurationMetric("DUR", predicate.id(), nullopt, {});
    *durationMetric.mutable_dimensions_in_what() = durDims;
    *config.add_duration_metric() = durationMetric;
    GaugeMetric gaugeMetric = createGaugeMetric("GAUGE", gaugeMatcher.id(),
                                                GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    *gaugeMetric.mutable_dimensions_in_what() = CreateDimensions(ATOM_4, {1});
    *config.add_gauge_metric() = gaugeMetric;
    ValueMetric valueMetric = createValueMetric("VALUE", valueMatcher, 2, nullopt, {});
    valueMetric.set_skip_zero_diff_output(false);
    *valueMetric.mutable_dimensions_in_what() =
            CreateAttributionUidDimensions(ATOM_5, {Position::FIRST});
    *config.add_value_metric() = valueMetric;
    KllMetric kllMetric = createKllMetric("KLL", kllMatcher, 2, nullopt);
    *kllMetric.mutable_dimensions_in_what() = CreateDimensions(ATOM_6, {1});
    *config.add_kll_metric() = kllMetric;

    int64_t startTimeNs = getElapsedRealtimeNs();
    sp<UidMap> uidMap = new UidMap();
    const int UID_1 = 11, UID_2 = 12, UID_3 = 13, UID_4 = 14, UID_5 = 15, UID_6 = 16, UID_7 = 17,
              UID_8 = 18, UID_9 = 19;
    int extraUids = 10;  // Extra uids in the uid map that aren't referenced in the metric report.
    int extraUidStart = 1000;
    vector<int> uids = {UID_1, UID_2, UID_3, UID_4, UID_5, UID_6, UID_7, UID_8, UID_9};
    int numUids = extraUids + uids.size();
    for (int i = 0; i < extraUids; i++) {
        uids.push_back(extraUidStart + i);
    }
    // We only care about the uids for this test. Give defaults to everything else.
    vector<int64_t> versions(numUids, 0);
    vector<string> versionStrings(numUids, "");
    vector<string> apps(numUids, "");
    vector<string> installers(numUids, "");
    vector<uint8_t> hash;
    vector<vector<uint8_t>> certHashes(numUids, hash);
    uidMap->updateMap(startTimeNs,
                      createUidData(uids, versions, versionStrings, apps, installers, certHashes));

    class FakePullAtomCallback : public BnPullAtomCallback {
    public:
        int pullNum = 1;
        Status onPullAtom(int atomTag,
                          const shared_ptr<IPullAtomResultReceiver>& resultReceiver) override {
            std::vector<StatsEventParcel> parcels;
            AStatsEvent* event = makeAttributionStatsEvent(atomTag, 0, {UID_8}, {""}, pullNum, 0);
            AStatsEvent_build(event);

            size_t size;
            uint8_t* buffer = AStatsEvent_getBuffer(event, &size);

            StatsEventParcel p;
            p.buffer.assign(buffer, buffer + size);
            parcels.push_back(std::move(p));
            AStatsEvent_release(event);
            pullNum++;
            resultReceiver->pullFinished(atomTag, /*success=*/true, parcels);
            return Status::ok();
        }
    };

    ConfigKey key(123, 987);
    sp<StatsLogProcessor> p =
            CreateStatsLogProcessor(startTimeNs, startTimeNs, config, key,
                                    SharedRefBase::make<FakePullAtomCallback>(), ATOM_5, uidMap);

    const uint64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(TEN_MINUTES) * 1000000LL;
    std::vector<std::shared_ptr<LogEvent>> events;
    events.push_back(makeUidLogEvent(ATOM_1, startTimeNs + 10, UID_1, 0, 0));
    events.push_back(makeUidLogEvent(ATOM_1, startTimeNs + 11, UID_2, 0, 0));
    events.push_back(makeAttributionLogEvent(ATOM_2, startTimeNs + 12, {UID_3}, {""}, 0, 0));
    events.push_back(makeUidLogEvent(ATOM_3, startTimeNs + 15, UID_5, 0, 0));  // start
    events.push_back(makeUidLogEvent(ATOM_3, startTimeNs + 18, UID_5, 1, 0));  // stop
    events.push_back(makeExtraUidsLogEvent(ATOM_4, startTimeNs + 20, UID_6, 0, 0, {UID_7}));
    events.push_back(makeUidLogEvent(ATOM_6, startTimeNs + 22, UID_9, 0, 0));

    events.push_back(
            makeAttributionLogEvent(ATOM_2, startTimeNs + bucketSizeNs + 10, {UID_4}, {""}, 0, 0));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        p->OnLogEvent(event.get());
    }

    int64_t dumpTimeNs = startTimeNs + bucketSizeNs + 100 * NS_PER_SEC;

    {
        ConfigMetricsReportList reports;
        vector<uint8_t> buffer;
        p->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, NO_TIME_CONSTRAINTS, &buffer);
        EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
        ASSERT_EQ(reports.reports_size(), 1);

        UidMapping uidMappingProto = reports.reports(0).uid_map();
        ASSERT_EQ(uidMappingProto.snapshots_size(), 1);
        const RepeatedPtrField<PackageInfo>& pkgs = uidMappingProto.snapshots(0).package_info();
        set<int32_t> actualUsedUids;
        std::for_each(pkgs.begin(), pkgs.end(),
                      [&actualUsedUids](const PackageInfo& p) { actualUsedUids.insert(p.uid()); });

        EXPECT_THAT(actualUsedUids, UnorderedElementsAre(UID_1, UID_2, UID_3, UID_4, UID_5, UID_6,
                                                         UID_7, UID_8, UID_9));
    }

    // Verify the set is cleared and only contains the correct ids on the next dump.
    p->OnLogEvent(makeUidLogEvent(ATOM_1, dumpTimeNs + 10, UID_1, 0, 0).get());
    {
        ConfigMetricsReportList reports;
        vector<uint8_t> buffer;
        p->onDumpReport(key, dumpTimeNs + 20, true, false, ADB_DUMP, FAST, &buffer);
        EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
        ASSERT_EQ(reports.reports_size(), 1);

        UidMapping uidMappingProto = reports.reports(0).uid_map();
        ASSERT_EQ(uidMappingProto.snapshots_size(), 1);
        const RepeatedPtrField<PackageInfo>& pkgs = uidMappingProto.snapshots(0).package_info();
        set<int32_t> actualUsedUids;
        std::for_each(pkgs.begin(), pkgs.end(),
                      [&actualUsedUids](const PackageInfo& p) { actualUsedUids.insert(p.uid()); });

        EXPECT_THAT(actualUsedUids, UnorderedElementsAre(UID_1));
    }
}

TEST(UidMapTest, TestUsedUidsFromMetricE2e) {
    const int ATOM_1 = 1, ATOM_2 = 2, ATOM_3 = 3, ATOM_4 = 4, ATOM_5 = 10001, ATOM_6 = 6;
    StatsdConfig config;
    config.mutable_statsd_config_options()->set_omit_unused_uids_in_uidmap(true);
    config.add_default_pull_packages("AID_ROOT");  // Fake puller is registered with root.
    AtomMatcher eventMatcher = CreateSimpleAtomMatcher("M1", ATOM_1);
    *config.add_atom_matcher() = eventMatcher;
    AtomMatcher countMatcher = CreateSimpleAtomMatcher("M2", ATOM_2);
    *config.add_atom_matcher() = countMatcher;
    AtomMatcher durationStartMatcher = CreateSimpleAtomMatcher("M3_START", ATOM_3);
    auto fvmStart = durationStartMatcher.mutable_simple_atom_matcher()->add_field_value_matcher();
    fvmStart->set_field(2);  // State field.
    fvmStart->set_eq_int(0);
    *config.add_atom_matcher() = durationStartMatcher;
    AtomMatcher durationStopMatcher = CreateSimpleAtomMatcher("M3_STOP", ATOM_3);
    auto fvmStop = durationStopMatcher.mutable_simple_atom_matcher()->add_field_value_matcher();
    fvmStop->set_field(2);
    fvmStop->set_eq_int(1);
    *config.add_atom_matcher() = durationStopMatcher;
    AtomMatcher gaugeMatcher = CreateSimpleAtomMatcher("M4", ATOM_4);
    *config.add_atom_matcher() = gaugeMatcher;
    AtomMatcher valueMatcher = CreateSimpleAtomMatcher("M5", ATOM_5);
    *config.add_atom_matcher() = valueMatcher;
    AtomMatcher kllMatcher = CreateSimpleAtomMatcher("M6", ATOM_6);
    *config.add_atom_matcher() = kllMatcher;

    Predicate predicate;
    predicate.set_id(StringToId("P1"));
    predicate.mutable_simple_predicate()->set_start(StringToId("M3_START"));
    predicate.mutable_simple_predicate()->set_stop(StringToId("M3_STOP"));
    FieldMatcher durDims = CreateDimensions(ATOM_3, {1});
    *predicate.mutable_simple_predicate()->mutable_dimensions() = durDims;
    *config.add_predicate() = predicate;

    EventMetric eventMetric = createEventMetric("EVENT", eventMatcher.id(), nullopt);
    *eventMetric.mutable_uid_fields() = CreateDimensions(ATOM_1, {1});
    *config.add_event_metric() = eventMetric;
    CountMetric countMetric = createCountMetric("COUNT", countMatcher.id(), nullopt, {});
    *countMetric.mutable_dimensions_in_what() = CreateDimensions(ATOM_2, {1});
    *countMetric.mutable_uid_fields() = CreateDimensions(ATOM_2, {1});
    *config.add_count_metric() = countMetric;
    DurationMetric durationMetric = createDurationMetric("DUR", predicate.id(), nullopt, {});
    *durationMetric.mutable_dimensions_in_what() = durDims;
    *durationMetric.mutable_uid_fields() = durDims;
    *config.add_duration_metric() = durationMetric;
    GaugeMetric gaugeMetric = createGaugeMetric("GAUGE", gaugeMatcher.id(),
                                                GaugeMetric::FIRST_N_SAMPLES, nullopt, nullopt);
    *gaugeMetric.mutable_dimensions_in_what() = CreateDimensions(ATOM_4, {1});
    *gaugeMetric.mutable_uid_fields() = CreateDimensions(ATOM_4, {1, 2});
    *config.add_gauge_metric() = gaugeMetric;
    ValueMetric valueMetric = createValueMetric("VALUE", valueMatcher, 2, nullopt, {});
    valueMetric.set_skip_zero_diff_output(false);
    *valueMetric.mutable_dimensions_in_what() = CreateDimensions(ATOM_5, {1});
    *valueMetric.mutable_uid_fields() = CreateDimensions(ATOM_5, {1});
    *config.add_value_metric() = valueMetric;
    KllMetric kllMetric = createKllMetric("KLL", kllMatcher, 2, nullopt);
    *kllMetric.mutable_dimensions_in_what() = CreateDimensions(ATOM_6, {1});
    *kllMetric.mutable_uid_fields() = CreateDimensions(ATOM_6, {1});
    *config.add_kll_metric() = kllMetric;

    int64_t startTimeNs = getElapsedRealtimeNs();
    sp<UidMap> uidMap = new UidMap();
    const int UID_1 = 11, UID_2 = 12, UID_3 = 13, UID_4 = 14, UID_5 = 15, UID_6 = 16, UID_7 = 17,
              UID_8 = 18, UID_9 = 19;
    int extraUids = 10;  // Extra uids in the uid map that aren't referenced in the metric report.
    int extraUidStart = 1000;
    vector<int> uids = {UID_1, UID_2, UID_3, UID_4, UID_5, UID_6, UID_7, UID_8, UID_9};
    int numUids = extraUids + uids.size();
    for (int i = 0; i < extraUids; i++) {
        uids.push_back(extraUidStart + i);
    }
    // We only care about the uids for this test. Give defaults to everything else.
    vector<int64_t> versions(numUids, 0);
    vector<string> versionStrings(numUids, "");
    vector<string> apps(numUids, "");
    vector<string> installers(numUids, "");
    vector<uint8_t> hash;
    vector<vector<uint8_t>> certHashes(numUids, hash);
    uidMap->updateMap(startTimeNs,
                      createUidData(uids, versions, versionStrings, apps, installers, certHashes));

    class FakePullAtomCallback : public BnPullAtomCallback {
    public:
        int pullNum = 1;
        Status onPullAtom(int atomTag,
                          const shared_ptr<IPullAtomResultReceiver>& resultReceiver) override {
            std::vector<StatsEventParcel> parcels;
            AStatsEvent* event = makeTwoValueStatsEvent(atomTag, 0, UID_8, pullNum);
            AStatsEvent_build(event);

            size_t size;
            uint8_t* buffer = AStatsEvent_getBuffer(event, &size);

            StatsEventParcel p;
            p.buffer.assign(buffer, buffer + size);
            parcels.push_back(std::move(p));
            AStatsEvent_release(event);
            pullNum++;
            resultReceiver->pullFinished(atomTag, /*success=*/true, parcels);
            return Status::ok();
        }
    };

    ConfigKey key(123, 987);
    sp<StatsLogProcessor> p =
            CreateStatsLogProcessor(startTimeNs, startTimeNs, config, key,
                                    SharedRefBase::make<FakePullAtomCallback>(), ATOM_5, uidMap);

    const uint64_t bucketSizeNs = TimeUnitToBucketSizeInMillis(TEN_MINUTES) * 1000000LL;
    std::vector<std::shared_ptr<LogEvent>> events;
    events.push_back(CreateTwoValueLogEvent(ATOM_1, startTimeNs + 10, UID_1, 0));
    events.push_back(CreateTwoValueLogEvent(ATOM_1, startTimeNs + 11, UID_2, 0));
    events.push_back(CreateTwoValueLogEvent(ATOM_2, startTimeNs + 12, UID_3, 0));
    events.push_back(CreateTwoValueLogEvent(ATOM_3, startTimeNs + 15, UID_5, 0));  // start
    events.push_back(CreateTwoValueLogEvent(ATOM_3, startTimeNs + 18, UID_5, 1));  // stop
    events.push_back(CreateTwoValueLogEvent(ATOM_4, startTimeNs + 20, UID_6, UID_7));
    events.push_back(CreateTwoValueLogEvent(ATOM_6, startTimeNs + 22, UID_9, 0));

    events.push_back(CreateTwoValueLogEvent(ATOM_2, startTimeNs + bucketSizeNs + 10, UID_4, 0));

    // Send log events to StatsLogProcessor.
    for (auto& event : events) {
        p->OnLogEvent(event.get());
    }

    int64_t dumpTimeNs = startTimeNs + bucketSizeNs + 100 * NS_PER_SEC;

    {
        ConfigMetricsReportList reports;
        vector<uint8_t> buffer;
        p->onDumpReport(key, dumpTimeNs, true, true, ADB_DUMP, NO_TIME_CONSTRAINTS, &buffer);
        EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
        ASSERT_EQ(reports.reports_size(), 1);

        UidMapping uidMappingProto = reports.reports(0).uid_map();
        ASSERT_EQ(uidMappingProto.snapshots_size(), 1);
        const RepeatedPtrField<PackageInfo>& pkgs = uidMappingProto.snapshots(0).package_info();
        set<int32_t> actualUsedUids;
        std::for_each(pkgs.begin(), pkgs.end(),
                      [&actualUsedUids](const PackageInfo& p) { actualUsedUids.insert(p.uid()); });

        EXPECT_THAT(actualUsedUids, UnorderedElementsAre(UID_1, UID_2, UID_3, UID_4, UID_5, UID_6,
                                                         UID_7, UID_8, UID_9));
    }

    // Verify the set is cleared and only contains the correct ids on the next dump.
    p->OnLogEvent(CreateTwoValueLogEvent(ATOM_1, dumpTimeNs + 10, UID_1, 0).get());
    {
        ConfigMetricsReportList reports;
        vector<uint8_t> buffer;
        p->onDumpReport(key, dumpTimeNs + 20, true, false, ADB_DUMP, FAST, &buffer);
        EXPECT_TRUE(reports.ParseFromArray(&buffer[0], buffer.size()));
        ASSERT_EQ(reports.reports_size(), 1);

        UidMapping uidMappingProto = reports.reports(0).uid_map();
        ASSERT_EQ(uidMappingProto.snapshots_size(), 1);
        const RepeatedPtrField<PackageInfo>& pkgs = uidMappingProto.snapshots(0).package_info();
        set<int32_t> actualUsedUids;
        std::for_each(pkgs.begin(), pkgs.end(),
                      [&actualUsedUids](const PackageInfo& p) { actualUsedUids.insert(p.uid()); });

        EXPECT_THAT(actualUsedUids, UnorderedElementsAre(UID_1));
    }
}

}  // anonymous namespace
#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif

}  // namespace statsd
}  // namespace os
}  // namespace android
