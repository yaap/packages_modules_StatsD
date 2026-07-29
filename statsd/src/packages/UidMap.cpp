/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, versionCode 2.0 (the "License");
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

#include "packages/UidMap.h"

#include <inttypes.h>
#include <private/android_filesystem_config.h>

#include "guardrail/StatsdStats.h"
#include "hash.h"
#include "stats_log_util.h"

namespace android {
namespace os {
namespace statsd {

using namespace android;

using android::util::FIELD_COUNT_REPEATED;
using android::util::FIELD_TYPE_BOOL;
using android::util::FIELD_TYPE_BYTES;
using android::util::FIELD_TYPE_FLOAT;
using android::util::FIELD_TYPE_INT32;
using android::util::FIELD_TYPE_INT64;
using android::util::FIELD_TYPE_MESSAGE;
using android::util::FIELD_TYPE_STRING;
using android::util::FIELD_TYPE_UINT32;
using android::util::FIELD_TYPE_UINT64;
using android::util::ProtoOutputStream;

using std::map;
using std::set;
using std::string;
using std::vector;

namespace {

const int FIELD_ID_SNAPSHOT_PACKAGE_NAME = 1;
const int FIELD_ID_SNAPSHOT_PACKAGE_VERSION = 2;
const int FIELD_ID_SNAPSHOT_PACKAGE_UID = 3;
const int FIELD_ID_SNAPSHOT_PACKAGE_DELETED = 4;
const int FIELD_ID_SNAPSHOT_PACKAGE_NAME_HASH = 5;
const int FIELD_ID_SNAPSHOT_PACKAGE_VERSION_STRING = 6;
const int FIELD_ID_SNAPSHOT_PACKAGE_VERSION_STRING_HASH = 7;
const int FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER = 8;
const int FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER_HASH = 9;
const int FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER_INDEX = 10;
const int FIELD_ID_SNAPSHOT_PACKAGE_TRUNCATED_CERTIFICATE_HASH = 11;
const int FIELD_ID_SNAPSHOT_TIMESTAMP = 1;
const int FIELD_ID_SNAPSHOT_PACKAGE_INFO = 2;
const int FIELD_ID_SNAPSHOTS = 1;
const int FIELD_ID_CHANGES = 2;
const int FIELD_ID_INSTALLER_HASH = 3;
const int FIELD_ID_INSTALLER_NAME = 4;
const int FIELD_ID_CHANGE_DELETION = 1;
const int FIELD_ID_CHANGE_TIMESTAMP = 2;
const int FIELD_ID_CHANGE_PACKAGE = 3;
const int FIELD_ID_CHANGE_UID = 4;
const int FIELD_ID_CHANGE_NEW_VERSION = 5;
const int FIELD_ID_CHANGE_PREV_VERSION = 6;
const int FIELD_ID_CHANGE_PACKAGE_HASH = 7;
const int FIELD_ID_CHANGE_NEW_VERSION_STRING = 8;
const int FIELD_ID_CHANGE_PREV_VERSION_STRING = 9;
const int FIELD_ID_CHANGE_NEW_VERSION_STRING_HASH = 10;
const int FIELD_ID_CHANGE_PREV_VERSION_STRING_HASH = 11;

bool omitUid(int32_t uid, const string& packageName, const UidMapOptions& options) {
    // Always allow allowlisted packages
    if (options.allowlistedPackages.contains(packageName)) {
        return false;
    }
    // If omitSystemUids is true, uids for which (uid % AID_USER_OFFSET) is in [0, AID_APP_START)
    // should be excluded. This takes precedence over if the uid is used or not.
    if (options.omitSystemUids && uid >= 0 && uid % AID_USER_OFFSET < AID_APP_START) {
        return true;
    }

    // If omitUnusedUids is false, then we should not omit other uids.
    if (!options.omitUnusedUids) {
        return false;
    }

    // If the uid is used, then we should not omit it.
    if (options.usedUids.contains(uid)) {
        return false;
    }

    // If the uid is an app uid, then we should check if the sdk sandbox or pcc component uid is
    // used. If so, then we should not omit the app uid.
    if (uid >= 0) {
        const int appId = uid % AID_USER_OFFSET;
        if (appId >= AID_APP_START && appId <= AID_APP_END) {
            const int32_t sdkSandboxUid = uid + (AID_SDK_SANDBOX_PROCESS_START - AID_APP_START);
            const int32_t pccComponentUid = uid + (AID_PCC_COMPONENT_PROCESS_START - AID_APP_START);
            if (options.usedUids.contains(sdkSandboxUid) ||
                options.usedUids.contains(pccComponentUid)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

UidMap::UidMap() : mBytesUsed(0) {
}

UidMap::~UidMap() {}

sp<UidMap> UidMap::getInstance() {
    static sp<UidMap> sInstance = new UidMap();
    return sInstance;
}

bool UidMap::hasApp(int uid, const string& packageName) const {
    std::lock_guard lock(mMutex);

    auto it = mMap.find(std::make_pair(uid, packageName));
    return it != mMap.end() && !it->second.deleted;
}

string UidMap::normalizeAppName(const string& appName) const {
    string normalizedName = appName;
    std::transform(normalizedName.begin(), normalizedName.end(), normalizedName.begin(), ::tolower);
    return normalizedName;
}

std::set<string> UidMap::getAppNamesFromUid(const int32_t uid, bool returnNormalized) const {
    std::lock_guard lock(mMutex);
    return getAppNamesFromUidLocked(uid,returnNormalized);
}

std::set<string> UidMap::getAppNamesFromUidLocked(const int32_t uid, bool returnNormalized) const {
    std::set<string> names;
    for (const auto& kv : mMap) {
        if (kv.first.first == uid && !kv.second.deleted) {
            names.insert(returnNormalized ? normalizeAppName(kv.first.second) : kv.first.second);
        }
    }
    return names;
}

int64_t UidMap::getAppVersion(int uid, const string& packageName) const {
    std::lock_guard lock(mMutex);

    auto it = mMap.find(std::make_pair(uid, packageName));
    if (it == mMap.end() || it->second.deleted) {
        return 0;
    }
    return it->second.versionCode;
}

void UidMap::updateMap(const int64_t timestamp, const UidData& uidData) {
    wp<PackageInfoListener> broadcast = NULL;
    {
        std::lock_guard lock(mMutex);  // Exclusively lock for updates.

        std::unordered_map<std::pair<int, string>, AppData, PairHash> deletedApps;

        // Copy all the deleted apps.
        for (const auto& kv : mMap) {
            if (kv.second.deleted) {
                deletedApps[kv.first] = kv.second;
            }
        }

        mMap.clear();
        for (const auto& appInfo : uidData.app_info()) {
            mMap[std::make_pair(appInfo.uid(), appInfo.package_name())] =
                    AppData(appInfo.version(), appInfo.version_string(), appInfo.installer(),
                            appInfo.certificate_hash());
        }

        for (const auto& kv : deletedApps) {
            auto mMapIt = mMap.find(kv.first);
            if (mMapIt != mMap.end()) {
                // Insert this deleted app back into the current map.
                mMap[kv.first] = kv.second;
            }
        }

        ensureBytesUsedBelowLimit();
        StatsdStats::getInstance().setCurrentUidMapMemory(mBytesUsed);
        broadcast = mSubscriber;
    }
    // To avoid invoking callback while holding the internal lock. we get a copy of the listener
    // and invoke the callback. It's still possible that after we copy the listener, it removes
    // itself before we call it. It's then the listener's job to handle it (expect the callback to
    // be called after listener is removed, and the listener should properly ignore it).
    auto strongPtr = broadcast.promote();
    if (strongPtr != nullptr) {
        strongPtr->onUidMapReceived(timestamp);
    }
}

void UidMap::updateApp(const int64_t timestamp, const string& appName, const int32_t uid,
                       const int64_t versionCode, const string& versionString,
                       const string& installer, const vector<uint8_t>& certificateHash) {
    wp<PackageInfoListener> broadcast = NULL;

    const string certificateHashString = string(certificateHash.begin(), certificateHash.end());
    {
        std::lock_guard lock(mMutex);
        int32_t prevVersion = 0;
        string prevVersionString = "";
        auto key = std::make_pair(uid, appName);
        auto it = mMap.find(key);
        if (it != mMap.end()) {
            prevVersion = it->second.versionCode;
            prevVersionString = it->second.versionString;
            it->second.versionCode = versionCode;
            it->second.versionString = versionString;
            it->second.installer = installer;
            it->second.deleted = false;
            it->second.certificateHash = certificateHashString;

            // Only notify the listeners if this is an app upgrade. If this app is being installed
            // for the first time, then we don't notify the listeners.
            // It's also OK to split again if we're forming a partial bucket after re-installing an
            // app after deletion.
            broadcast = mSubscriber;
        } else {
            // Otherwise, we need to add an app at this uid.
            mMap[key] = AppData(versionCode, versionString, installer, certificateHashString);
        }

        mChanges.emplace_back(false, timestamp, appName, uid, versionCode, versionString,
                              prevVersion, prevVersionString);
        mBytesUsed += kBytesChangeRecord;
        ensureBytesUsedBelowLimit();
        StatsdStats::getInstance().setCurrentUidMapMemory(mBytesUsed);
        StatsdStats::getInstance().setUidMapChanges(mChanges.size());
    }

    auto strongPtr = broadcast.promote();
    if (strongPtr != nullptr) {
        strongPtr->notifyAppUpgrade(timestamp, appName, uid, versionCode);
    }
}

void UidMap::ensureBytesUsedBelowLimit() {
    size_t limit;
    if (maxBytesOverride <= 0) {
        limit = StatsdStats::kMaxBytesUsedUidMap;
    } else {
        limit = maxBytesOverride;
    }
    while (mBytesUsed > limit) {
        ALOGI("Bytes used %zu is above limit %zu, need to delete something", mBytesUsed, limit);
        if (mChanges.size() > 0) {
            mBytesUsed -= kBytesChangeRecord;
            mChanges.pop_front();
            StatsdStats::getInstance().noteUidMapDropped(1);
        }
    }
}

void UidMap::removeApp(const int64_t timestamp, const string& app, const int32_t uid) {
    wp<PackageInfoListener> broadcast = NULL;
    {
        std::lock_guard lock(mMutex);

        int64_t prevVersion = 0;
        string prevVersionString = "";
        auto key = std::make_pair(uid, app);
        auto it = mMap.find(key);
        if (it != mMap.end() && !it->second.deleted) {
            prevVersion = it->second.versionCode;
            prevVersionString = it->second.versionString;
            it->second.deleted = true;
            mDeletedApps.push_back(key);
        }
        if (mDeletedApps.size() > StatsdStats::kMaxDeletedAppsInUidMap) {
            // Delete the oldest one.
            auto oldest = mDeletedApps.front();
            mDeletedApps.pop_front();
            mMap.erase(oldest);
            StatsdStats::getInstance().noteUidMapAppDeletionDropped();
        }
        mChanges.emplace_back(true, timestamp, app, uid, 0, "", prevVersion, prevVersionString);
        mBytesUsed += kBytesChangeRecord;
        ensureBytesUsedBelowLimit();
        StatsdStats::getInstance().setCurrentUidMapMemory(mBytesUsed);
        StatsdStats::getInstance().setUidMapChanges(mChanges.size());
        broadcast = mSubscriber;
    }

    auto strongPtr = broadcast.promote();
    if (strongPtr != nullptr) {
        strongPtr->notifyAppRemoved(timestamp, app, uid);
    }
}

void UidMap::setListener(const wp<PackageInfoListener>& listener) {
    std::lock_guard lock(mMutex);  // Lock for updates
    mSubscriber = listener;
}

void UidMap::assignIsolatedUid(int isolatedUid, int parentUid) {
    std::lock_guard lock(mIsolatedMutex);

    mIsolatedUidMap[isolatedUid] = parentUid;
}

void UidMap::removeIsolatedUid(int isolatedUid) {
    std::lock_guard lock(mIsolatedMutex);

    auto it = mIsolatedUidMap.find(isolatedUid);
    if (it != mIsolatedUidMap.end()) {
        mIsolatedUidMap.erase(it);
    }
}

int UidMap::getHostUidOrSelf(int uid) const {
    std::lock_guard lock(mIsolatedMutex);

    auto it = mIsolatedUidMap.find(uid);
    if (it != mIsolatedUidMap.end()) {
        return it->second;
    }
    return uid;
}

void UidMap::clearOutput() {
    mChanges.clear();
    // Also update the guardrail trackers.
    StatsdStats::getInstance().setUidMapChanges(0);
    mBytesUsed = 0;
    StatsdStats::getInstance().setCurrentUidMapMemory(mBytesUsed);
}

int64_t UidMap::getMinimumTimestampNs() {
    int64_t m = 0;
    for (const auto& kv : mLastUpdatePerConfigKey) {
        if (m == 0) {
            m = kv.second;
        } else if (kv.second < m) {
            m = kv.second;
        }
    }
    return m;
}

size_t UidMap::getBytesUsed() const {
    return mBytesUsed;
}

void UidMap::writeUidMapSnapshot(int64_t timestamp, const UidMapOptions& options,
                                 map<string, int>* installerIndices, std::set<string>* str_set,
                                 ProtoOutputStream* proto) const {
    std::lock_guard lock(mMutex);

    writeUidMapSnapshotLocked(timestamp, options, installerIndices, str_set, proto);
}

void UidMap::writeUidMapSnapshotLocked(const int64_t timestamp, const UidMapOptions& options,
                                       map<string, int>* installerIndices,
                                       std::set<string>* str_set, ProtoOutputStream* proto) const {
    int curInstallerIndex = 0;

    proto->write(FIELD_TYPE_INT64 | FIELD_ID_SNAPSHOT_TIMESTAMP, (long long)timestamp);
    for (const auto& [keyPair, appData] : mMap) {
        const auto& [uid, packageName] = keyPair;
        if (omitUid(uid, packageName, options)) {
            continue;
        }
        uint64_t token = proto->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED |
                                      FIELD_ID_SNAPSHOT_PACKAGE_INFO);
        // Get installer index.
        int installerIndex = -1;
        if (options.includeInstaller && installerIndices != nullptr) {
            const auto& it = installerIndices->find(appData.installer);
            if (it == installerIndices->end()) {
                // We have not encountered this installer yet; add it to installerIndices.
                (*installerIndices)[appData.installer] = curInstallerIndex;
                installerIndex = curInstallerIndex;
                curInstallerIndex++;
            } else {
                installerIndex = it->second;
            }
        }

        if (str_set != nullptr) {  // Hash strings in report
            str_set->insert(packageName);
            proto->write(FIELD_TYPE_UINT64 | FIELD_ID_SNAPSHOT_PACKAGE_NAME_HASH,
                         (long long)Hash64(packageName));
            if (options.includeVersionStrings) {
                str_set->insert(appData.versionString);
                proto->write(FIELD_TYPE_UINT64 | FIELD_ID_SNAPSHOT_PACKAGE_VERSION_STRING_HASH,
                             (long long)Hash64(appData.versionString));
            }
            if (options.includeInstaller) {
                str_set->insert(appData.installer);
                if (installerIndex != -1) {
                    // Write installer index.
                    proto->write(FIELD_TYPE_UINT32 | FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER_INDEX,
                                 installerIndex);
                } else {
                    proto->write(FIELD_TYPE_UINT64 | FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER_HASH,
                                 (long long)Hash64(appData.installer));
                }
            }
        } else {  // Strings not hashed in report
            proto->write(FIELD_TYPE_STRING | FIELD_ID_SNAPSHOT_PACKAGE_NAME, packageName);
            if (options.includeVersionStrings) {
                proto->write(FIELD_TYPE_STRING | FIELD_ID_SNAPSHOT_PACKAGE_VERSION_STRING,
                             appData.versionString);
            }
            if (options.includeInstaller) {
                if (installerIndex != -1) {
                    proto->write(FIELD_TYPE_UINT32 | FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER_INDEX,
                                 installerIndex);
                } else {
                    proto->write(FIELD_TYPE_STRING | FIELD_ID_SNAPSHOT_PACKAGE_INSTALLER,
                                 appData.installer);
                }
            }
        }

        const size_t dumpHashSize =
                options.truncatedCertificateHashSize <= appData.certificateHash.size()
                        ? options.truncatedCertificateHashSize
                        : appData.certificateHash.size();
        if (dumpHashSize > 0) {
            proto->write(FIELD_TYPE_BYTES | FIELD_ID_SNAPSHOT_PACKAGE_TRUNCATED_CERTIFICATE_HASH,
                         appData.certificateHash.c_str(), dumpHashSize);
        }

        proto->write(FIELD_TYPE_INT64 | FIELD_ID_SNAPSHOT_PACKAGE_VERSION,
                     (long long)appData.versionCode);
        proto->write(FIELD_TYPE_INT32 | FIELD_ID_SNAPSHOT_PACKAGE_UID, uid);
        proto->write(FIELD_TYPE_BOOL | FIELD_ID_SNAPSHOT_PACKAGE_DELETED, appData.deleted);
        proto->end(token);
    }
}

void UidMap::appendUidMap(const int64_t timestamp, const ConfigKey& key,
                          const UidMapOptions& options, std::set<string>* str_set,
                          ProtoOutputStream* proto) {
    std::lock_guard lock(mMutex);  // Lock for updates

    for (const ChangeRecord& record : mChanges) {
        if (omitUid(record.uid, record.package, options) ||
            record.timestampNs <= mLastUpdatePerConfigKey[key]) {
            continue;
        }

        uint64_t changesToken =
                proto->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_CHANGES);
        proto->write(FIELD_TYPE_BOOL | FIELD_ID_CHANGE_DELETION, (bool)record.deletion);
        proto->write(FIELD_TYPE_INT64 | FIELD_ID_CHANGE_TIMESTAMP, (long long)record.timestampNs);
        if (str_set != nullptr) {
            str_set->insert(record.package);
            proto->write(FIELD_TYPE_UINT64 | FIELD_ID_CHANGE_PACKAGE_HASH,
                         (long long)Hash64(record.package));
            if (options.includeVersionStrings) {
                str_set->insert(record.versionString);
                proto->write(FIELD_TYPE_UINT64 | FIELD_ID_CHANGE_NEW_VERSION_STRING_HASH,
                             (long long)Hash64(record.versionString));
                str_set->insert(record.prevVersionString);
                proto->write(FIELD_TYPE_UINT64 | FIELD_ID_CHANGE_PREV_VERSION_STRING_HASH,
                             (long long)Hash64(record.prevVersionString));
            }
        } else {
            proto->write(FIELD_TYPE_STRING | FIELD_ID_CHANGE_PACKAGE, record.package);
            if (options.includeVersionStrings) {
                proto->write(FIELD_TYPE_STRING | FIELD_ID_CHANGE_NEW_VERSION_STRING,
                             record.versionString);
                proto->write(FIELD_TYPE_STRING | FIELD_ID_CHANGE_PREV_VERSION_STRING,
                             record.prevVersionString);
            }
        }

        proto->write(FIELD_TYPE_INT32 | FIELD_ID_CHANGE_UID, (int)record.uid);
        proto->write(FIELD_TYPE_INT64 | FIELD_ID_CHANGE_NEW_VERSION, (long long)record.version);
        proto->write(FIELD_TYPE_INT64 | FIELD_ID_CHANGE_PREV_VERSION,
                     (long long)record.prevVersion);
        proto->end(changesToken);
    }

    map<string, int> installerIndices;

    // Write snapshot from current uid map state.
    uint64_t snapshotsToken =
            proto->start(FIELD_TYPE_MESSAGE | FIELD_COUNT_REPEATED | FIELD_ID_SNAPSHOTS);
    writeUidMapSnapshotLocked(timestamp, options, &installerIndices, str_set, proto);
    proto->end(snapshotsToken);

    vector<string> installers(installerIndices.size(), "");
    for (const auto& [installer, index] : installerIndices) {
        // index is guaranteed to be < installers.size().
        installers[index] = installer;
    }

    if (options.includeInstaller) {
        // Write installer list; either strings or hashes.
        for (const string& installerName : installers) {
            if (str_set == nullptr) {  // Strings not hashed
                proto->write(FIELD_TYPE_STRING | FIELD_COUNT_REPEATED | FIELD_ID_INSTALLER_NAME,
                             installerName);
            } else {  // Strings are hashed
                proto->write(FIELD_TYPE_UINT64 | FIELD_COUNT_REPEATED | FIELD_ID_INSTALLER_HASH,
                             (long long)Hash64(installerName));
            }
        }
    }

    int64_t prevMin = getMinimumTimestampNs();
    mLastUpdatePerConfigKey[key] = timestamp;
    int64_t newMin = getMinimumTimestampNs();

    if (newMin > prevMin) {  // Delete anything possible now that the minimum has
                             // moved forward.
        int64_t cutoff_nanos = newMin;
        for (auto it_changes = mChanges.begin(); it_changes != mChanges.end();) {
            if (it_changes->timestampNs < cutoff_nanos) {
                mBytesUsed -= kBytesChangeRecord;
                it_changes = mChanges.erase(it_changes);
            } else {
                ++it_changes;
            }
        }
    }
    StatsdStats::getInstance().setCurrentUidMapMemory(mBytesUsed);
    StatsdStats::getInstance().setUidMapChanges(mChanges.size());
}

void UidMap::printUidMap(int out, bool includeCertificateHash) const {
    std::lock_guard lock(mMutex);

    for (const auto& [keyPair, appData] : mMap) {
        const auto& [uid, packageName] = keyPair;
        if (!appData.deleted) {
            if (includeCertificateHash) {
                const string& certificateHashHexString = toHexString(appData.certificateHash);
                dprintf(out, "%s, v%" PRId64 ", %s, %s (%i), %s\n", packageName.c_str(),
                        appData.versionCode, appData.versionString.c_str(),
                        appData.installer.c_str(), uid, certificateHashHexString.c_str());
            } else {
                dprintf(out, "%s, v%" PRId64 ", %s, %s (%i)\n", packageName.c_str(),
                        appData.versionCode, appData.versionString.c_str(),
                        appData.installer.c_str(), uid);
            }
        }
    }
}

void UidMap::OnConfigUpdated(const ConfigKey& key) {
    mLastUpdatePerConfigKey[key] = -1;
}

void UidMap::OnConfigRemoved(const ConfigKey& key) {
    mLastUpdatePerConfigKey.erase(key);
}

set<int32_t> UidMap::getAppUid(const string& package) const {
    std::lock_guard lock(mMutex);

    set<int32_t> results;
    for (const auto& kv : mMap) {
        if (kv.first.second == package && !kv.second.deleted) {
            results.insert(kv.first.first);
        }
    }
    return results;
}

// Note not all the following AIDs are used as uids. Some are used only for gids.
// It's ok to leave them in the map, but we won't ever see them in the log's uid field.
// App's uid starts from 10000, and will not overlap with the following AIDs.
const std::map<string, uint32_t> UidMap::sAidToUidMapping = {
        {"AID_ROOT", AID_ROOT},
        {"AID_SYSTEM", AID_SYSTEM},
        {"AID_RADIO", AID_RADIO},
        {"AID_BLUETOOTH", AID_BLUETOOTH},
        {"AID_GRAPHICS", AID_GRAPHICS},
        {"AID_INPUT", AID_INPUT},
        {"AID_AUDIO", AID_AUDIO},
        {"AID_CAMERA", AID_CAMERA},
        {"AID_LOG", AID_LOG},
        {"AID_COMPASS", AID_COMPASS},
        {"AID_MOUNT", AID_MOUNT},
        {"AID_WIFI", AID_WIFI},
        {"AID_ADB", AID_ADB},
        {"AID_INSTALL", AID_INSTALL},
        {"AID_MEDIA", AID_MEDIA},
        {"AID_DHCP", AID_DHCP},
        {"AID_SDCARD_RW", AID_SDCARD_RW},
        {"AID_VPN", AID_VPN},
        {"AID_KEYSTORE", AID_KEYSTORE},
        {"AID_USB", AID_USB},
        {"AID_DRM", AID_DRM},
        {"AID_MDNSR", AID_MDNSR},
        {"AID_GPS", AID_GPS},
        // {"AID_UNUSED1", 1022},
        {"AID_MEDIA_RW", AID_MEDIA_RW},
        {"AID_MTP", AID_MTP},
        // {"AID_UNUSED2", 1025},
        {"AID_DRMRPC", AID_DRMRPC},
        {"AID_NFC", AID_NFC},
        {"AID_SDCARD_R", AID_SDCARD_R},
        {"AID_CLAT", AID_CLAT},
        {"AID_LOOP_RADIO", AID_LOOP_RADIO},
        {"AID_MEDIA_DRM", AID_MEDIA_DRM},
        {"AID_PACKAGE_INFO", AID_PACKAGE_INFO},
        {"AID_SDCARD_PICS", AID_SDCARD_PICS},
        {"AID_SDCARD_AV", AID_SDCARD_AV},
        {"AID_SDCARD_ALL", AID_SDCARD_ALL},
        {"AID_LOGD", AID_LOGD},
        {"AID_SHARED_RELRO", AID_SHARED_RELRO},
        {"AID_DBUS", AID_DBUS},
        {"AID_TLSDATE", AID_TLSDATE},
        {"AID_MEDIA_EX", AID_MEDIA_EX},
        {"AID_AUDIOSERVER", AID_AUDIOSERVER},
        {"AID_METRICS_COLL", AID_METRICS_COLL},
        {"AID_METRICSD", AID_METRICSD},
        {"AID_WEBSERV", AID_WEBSERV},
        {"AID_DEBUGGERD", AID_DEBUGGERD},
        {"AID_MEDIA_CODEC", AID_MEDIA_CODEC},
        {"AID_CAMERASERVER", AID_CAMERASERVER},
        {"AID_FIREWALL", AID_FIREWALL},
        {"AID_TRUNKS", AID_TRUNKS},
        {"AID_NVRAM", AID_NVRAM},
        {"AID_DNS", AID_DNS},
        {"AID_DNS_TETHER", AID_DNS_TETHER},
        {"AID_WEBVIEW_ZYGOTE", AID_WEBVIEW_ZYGOTE},
        {"AID_VEHICLE_NETWORK", AID_VEHICLE_NETWORK},
        {"AID_MEDIA_AUDIO", AID_MEDIA_AUDIO},
        {"AID_MEDIA_VIDEO", AID_MEDIA_VIDEO},
        {"AID_MEDIA_IMAGE", AID_MEDIA_IMAGE},
        {"AID_TOMBSTONED", AID_TOMBSTONED},
        {"AID_MEDIA_OBB", AID_MEDIA_OBB},
        {"AID_ESE", AID_ESE},
        {"AID_OTA_UPDATE", AID_OTA_UPDATE},
        {"AID_AUTOMOTIVE_EVS", AID_AUTOMOTIVE_EVS},
        {"AID_LOWPAN", AID_LOWPAN},
        {"AID_HSM", AID_HSM},
        {"AID_RESERVED_DISK", AID_RESERVED_DISK},
        {"AID_STATSD", AID_STATSD},
        {"AID_INCIDENTD", AID_INCIDENTD},
        {"AID_SECURE_ELEMENT", AID_SECURE_ELEMENT},
        {"AID_LMKD", AID_LMKD},
        {"AID_LLKD", AID_LLKD},
        {"AID_IORAPD", AID_IORAPD},
        {"AID_GPU_SERVICE", AID_GPU_SERVICE},
        {"AID_NETWORK_STACK", AID_NETWORK_STACK},
        {"AID_GSID", AID_GSID},
        {"AID_FSVERITY_CERT", AID_FSVERITY_CERT},
        {"AID_CREDSTORE", AID_CREDSTORE},
        {"AID_EXTERNAL_STORAGE", AID_EXTERNAL_STORAGE},
        {"AID_EXT_DATA_RW", AID_EXT_DATA_RW},
        {"AID_EXT_OBB_RW", AID_EXT_OBB_RW},
        {"AID_CONTEXT_HUB", AID_CONTEXT_HUB},
        {"AID_VIRTUALIZATIONSERVICE", AID_VIRTUALIZATIONSERVICE},
        {"AID_ARTD", AID_ARTD},
        {"AID_UWB", AID_UWB},
        {"AID_THREAD_NETWORK", AID_THREAD_NETWORK},
        {"AID_DICED", AID_DICED},
        {"AID_DMESGD", AID_DMESGD},
        {"AID_JC_WEAVER", AID_JC_WEAVER},
        {"AID_JC_STRONGBOX", AID_JC_STRONGBOX},
        {"AID_JC_IDENTITYCRED", AID_JC_IDENTITYCRED},
        {"AID_SDK_SANDBOX", AID_SDK_SANDBOX},
        {"AID_SECURITY_LOG_WRITER", AID_SECURITY_LOG_WRITER},
        {"AID_PRNG_SEEDER", AID_PRNG_SEEDER},
        {"AID_UPROBESTATS", AID_UPROBESTATS},
        {"AID_CROS_EC", AID_CROS_EC},
        {"AID_MMD", AID_MMD},
        {"AID_UPDATE_ENGINE_LOG", AID_UPDATE_ENGINE_LOG},
        {"AID_AP_FIRMWARE", AID_AP_FIRMWARE},
        {"AID_PMGD", AID_PMGD},
        {"AID_SDV_SD_AGENT", AID_SDV_SD_AGENT},
        {"AID_SDV_DT_AGENT", AID_SDV_DT_AGENT},
        {"AID_SDV_RPC_AGENT", AID_SDV_RPC_AGENT},
        {"AID_SDV_INIT_OPEN_DICE", AID_SDV_INIT_OPEN_DICE},
        {"AID_SHELL", AID_SHELL},
        {"AID_CACHE", AID_CACHE},
        {"AID_DIAG", AID_DIAG},
        {"AID_NOBODY", AID_NOBODY}};

}  // namespace statsd
}  // namespace os
}  // namespace android
