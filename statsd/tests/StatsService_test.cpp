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

#include "StatsService.h"

#include <android/binder_interface_utils.h>
#include <fcntl.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "config/ConfigKey.h"
#include "packages/UidMap.h"
#include "src/statsd_config.pb.h"
#include "tests/statsd_test_util.h"

namespace android {
namespace os {
namespace statsd {

using namespace android;
using namespace testing;

using android::base::StringPrintf;
using ::ndk::SharedRefBase;
using Status = ::ndk::ScopedAStatus;
using std::nullopt;
using std::shared_ptr;
using std::vector;

#ifdef __ANDROID__

namespace {

const int64_t metricId = 123456;
const int32_t ATOM_TAG = util::SUBSYSTEM_SLEEP_STATE;

StatsdConfig CreateStatsdConfig(const GaugeMetric::SamplingType samplingType) {
    StatsdConfig config;
    config.add_default_pull_packages("AID_ROOT");  // Fake puller is registered with root.
    auto atomMatcher = CreateSimpleAtomMatcher("TestMatcher", ATOM_TAG);
    *config.add_atom_matcher() = atomMatcher;
    *config.add_gauge_metric() =
            createGaugeMetric("GAUGE1", atomMatcher.id(), samplingType, nullopt, nullopt);
    config.set_hash_strings_in_metric_report(false);
    return config;
}

Status exception(int32_t code, const std::string& msg) {
    return Status::fromExceptionCodeWithMessage(code, msg.c_str());
}

class FakeSubsystemSleepCallbackWithTiming : public FakeSubsystemSleepCallback {
public:
    Status onPullAtom(int atomTag,
                      const shared_ptr<IPullAtomResultReceiver>& resultReceiver) override {
        mPullTimeNs = getElapsedRealtimeNs();
        return FakeSubsystemSleepCallback::onPullAtom(atomTag, resultReceiver);
    }
    int64_t mPullTimeNs = 0;
};

using AddConfigFunc =
        std::function<Status(int64_t, int32_t, const string&, shared_ptr<StatsService>&)>;

}  // namespace

class StatsServiceTestAddConfig : public TestWithParam<AddConfigFunc> {};

INSTANTIATE_TEST_SUITE_P(
        ParseFuncs, StatsServiceTestAddConfig,
        Values(
                // Add config by passing a byte array.
                [](int64_t key, int32_t callingUid, const string& configStr,
                   shared_ptr<StatsService>& service) -> Status {
                    return service->addConfiguration(key, {configStr.begin(), configStr.end()},
                                                     callingUid);
                },

                // Add config by passing a file descriptor.
                [](int64_t key, int32_t callingUid, const string& configStr,
                   shared_ptr<StatsService>& service) -> Status {
                    ScopedFileDescriptor scopedFd(memfd_create("add_config_fd", MFD_CLOEXEC));
                    const int fd = scopedFd.get();
                    int f = fcntl(fd, F_GETFD);  // Read the file descriptor flags.
                    if (f == -1 ||
                        !(f & FD_CLOEXEC)) {  // Ensure there was no error while reading the flags.
                        return exception(EX_ILLEGAL_STATE, "Error creating file descriptor.");
                    }
                    ssize_t bytesWritten = write(fd, configStr.data(), configStr.size());
                    if (bytesWritten == -1) {
                        return exception(
                                EX_ILLEGAL_STATE,
                                StringPrintf("Error writing to memfd: %s", strerror(errno)));
                    } else if (bytesWritten != configStr.size()) {
                        return exception(
                                EX_ILLEGAL_STATE,
                                StringPrintf("Partial write to memfd. Expected: %zu, Actual: %zu",
                                             configStr.size(), bytesWritten));
                    } else if (lseek(fd, 0, SEEK_SET) != 0) {
                        return exception(EX_ILLEGAL_STATE,
                                         "Error moving file descriptor pointer to beginning.");
                    }

                    return service->addConfigurationFd(key, scopedFd, callingUid);
                }));

TEST_P(StatsServiceTestAddConfig, TestAddConfig_simple) {
    const sp<UidMap> uidMap = new UidMap();
    shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    const int kConfigKey = 12345;
    const int kCallingUid = 123;
    StatsdConfig config;
    config.set_id(kConfigKey);
    string serialized = config.SerializeAsString();
    AddConfigFunc addConfigFunc = GetParam();
    EXPECT_TRUE(addConfigFunc(kConfigKey, kCallingUid, serialized, service).isOk());
    service->removeConfiguration(kConfigKey, kCallingUid);
    ConfigKey configKey(kCallingUid, kConfigKey);
    service->mProcessor->onDumpReport(configKey, getElapsedRealtimeNs(),
                                      false /* include_current_bucket*/, true /* erase_data */,
                                      ADB_DUMP, NO_TIME_CONSTRAINTS, nullptr);
}

TEST_P(StatsServiceTestAddConfig, TestAddConfig_empty) {
    const sp<UidMap> uidMap = new UidMap();
    shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    string serialized = "";
    const int kConfigKey = 12345;
    const int kCallingUid = 123;
    AddConfigFunc addConfigFunc = GetParam();
    EXPECT_TRUE(addConfigFunc(kConfigKey, kCallingUid, serialized, service).isOk());
    service->removeConfiguration(kConfigKey, kCallingUid);
    ConfigKey configKey(kCallingUid, kConfigKey);
    service->mProcessor->onDumpReport(configKey, getElapsedRealtimeNs(),
                                      false /* include_current_bucket*/, true /* erase_data */,
                                      ADB_DUMP, NO_TIME_CONSTRAINTS, nullptr);
}

TEST_P(StatsServiceTestAddConfig, TestAddConfig_invalid) {
    const sp<UidMap> uidMap = new UidMap();
    shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    string serialized = "Invalid config!";

    AddConfigFunc addConfigFunc = GetParam();
    EXPECT_FALSE(addConfigFunc(12345, 123, serialized, service).isOk());
}

TEST(StatsServiceTest, TestGetUidFromArgs) {
    Vector<String8> args;
    args.push(String8("-1"));
    args.push(String8("0"));
    args.push(String8("1"));
    args.push(String8("a1"));
    args.push(String8(""));

    int32_t uid;

    const sp<UidMap> uidMap = new UidMap();
    shared_ptr<StatsService> service = SharedRefBase::make<StatsService>(
            uidMap, /* queue */ nullptr, std::make_shared<LogEventFilter>());
    service->mEngBuild = true;

    // "-1"
    EXPECT_FALSE(service->getUidFromArgs(args, 0, uid));

    // "0"
    EXPECT_TRUE(service->getUidFromArgs(args, 1, uid));
    EXPECT_EQ(0, uid);

    // "1"
    EXPECT_TRUE(service->getUidFromArgs(args, 2, uid));
    EXPECT_EQ(1, uid);

    // "a1"
    EXPECT_FALSE(service->getUidFromArgs(args, 3, uid));

    // ""
    EXPECT_FALSE(service->getUidFromArgs(args, 4, uid));

    // For a non-userdebug, uid "1" cannot be impersonated.
    service->mEngBuild = false;
    EXPECT_FALSE(service->getUidFromArgs(args, 2, uid));
}

TEST_F(StatsServiceConfigTest, StatsServiceStatsdInitTest) {
    // used for error threshold tolerance due to sleep() is involved
    const int64_t ERROR_THRESHOLD_NS = 25 * 1000000;  // 25 ms
    const int INIT_DELAY_SEC = 3;

    auto pullAtomCallback = SharedRefBase::make<FakeSubsystemSleepCallbackWithTiming>();

    // TODO: evaluate to use service->registerNativePullAtomCallback() API
    service->mPullerManager->RegisterPullAtomCallback(/*uid=*/0, ATOM_TAG, NS_PER_SEC,
                                                      NS_PER_SEC * 10, {}, pullAtomCallback);

    StatsdConfig config = CreateStatsdConfig(GaugeMetric::RANDOM_ONE_SAMPLE);
    config.set_id(kConfigKey);
    const int64_t createConfigTimeNs = getElapsedRealtimeNs();
    ASSERT_TRUE(sendConfig(config));
    ASSERT_EQ(2, pullAtomCallback->pullNum);

    service->mProcessor->mPullerManager->ForceClearPullerCache();

    const int64_t initCompletedTimeNs = getElapsedRealtimeNs();
    service->onStatsdInitCompleted(INIT_DELAY_SEC);
    ASSERT_EQ(3, pullAtomCallback->pullNum);

    // Checking pull with or without delay according to the flag value
    const int64_t lastPullNs = pullAtomCallback->mPullTimeNs;

    EXPECT_GE(lastPullNs, initCompletedTimeNs + INIT_DELAY_SEC * NS_PER_SEC);
    EXPECT_LE(lastPullNs, initCompletedTimeNs + INIT_DELAY_SEC * NS_PER_SEC + ERROR_THRESHOLD_NS);

    const int64_t bucketSizeNs =
            TimeUnitToBucketSizeInMillis(config.gauge_metric(0).bucket()) * 1000000;
    const int64_t dumpReportTsNanos = createConfigTimeNs + bucketSizeNs + NS_PER_SEC;

    vector<uint8_t> output;
    ConfigKey configKey(kCallingUid, kConfigKey);
    service->mProcessor->onDumpReport(configKey, dumpReportTsNanos,
                                      /*include_current_bucket=*/false, /*erase_data=*/true,
                                      ADB_DUMP, FAST, &output);
    ConfigMetricsReportList reports;
    reports.ParseFromArray(output.data(), output.size());
    ASSERT_EQ(1, reports.reports_size());

    backfillDimensionPath(&reports);
    backfillStartEndTimestamp(&reports);
    backfillAggregatedAtoms(&reports);
    StatsLogReport::GaugeMetricDataWrapper gaugeMetrics =
            reports.reports(0).metrics(0).gauge_metrics();
    ASSERT_EQ(gaugeMetrics.skipped_size(), 0);
    ASSERT_GT((int)gaugeMetrics.data_size(), 0);
    const auto data = gaugeMetrics.data(0);
    ASSERT_EQ(2, data.bucket_info_size());

    const auto bucketInfo0 = data.bucket_info(0);
    const auto bucketInfo1 = data.bucket_info(1);

    EXPECT_GE(NanoToMillis(bucketInfo0.start_bucket_elapsed_nanos()),
              NanoToMillis(createConfigTimeNs));
    EXPECT_LE(NanoToMillis(bucketInfo0.start_bucket_elapsed_nanos()),
              NanoToMillis(createConfigTimeNs + ERROR_THRESHOLD_NS));

    EXPECT_EQ(NanoToMillis(bucketInfo0.end_bucket_elapsed_nanos()),
              NanoToMillis(bucketInfo1.start_bucket_elapsed_nanos()));

    ASSERT_EQ(1, bucketInfo1.atom_size());
    ASSERT_GT(bucketInfo1.atom(0).subsystem_sleep_state().time_millis(), 0);

    EXPECT_GE(NanoToMillis(bucketInfo1.start_bucket_elapsed_nanos()),
              NanoToMillis(initCompletedTimeNs + INIT_DELAY_SEC * NS_PER_SEC));
    EXPECT_LE(NanoToMillis(bucketInfo1.start_bucket_elapsed_nanos()),
              NanoToMillis(initCompletedTimeNs + INIT_DELAY_SEC * NS_PER_SEC + ERROR_THRESHOLD_NS));

    // this check confirms that bucket end is not affected by the StatsService init delay
    EXPECT_EQ(NanoToMillis(bucketInfo1.end_bucket_elapsed_nanos()),
              NanoToMillis(service->mProcessor->mTimeBaseNs + bucketSizeNs));
}

TEST_F(StatsServiceConfigTest, LogEventFilterOnSetPrintLogs) {
    shared_ptr<MockLogEventFilter> mockLogEventFilter = std::make_shared<MockLogEventFilter>();

    EXPECT_CALL(*mockLogEventFilter, setAtomIds(StatsLogProcessor::getDefaultAtomIdSet(), _))
            .Times(1);
    Expectation filterSetFalse =
            EXPECT_CALL(*mockLogEventFilter, setFilteringEnabled(false)).Times(1);
    EXPECT_CALL(*mockLogEventFilter, setFilteringEnabled(true)).Times(1).After(filterSetFalse);

    auto service = createStatsService(mockLogEventFilter);

    Vector<String8> argsEnable;
    argsEnable.push(String8("print-logs"));
    argsEnable.push(String8("1"));

    Vector<String8> argsDisable;
    argsDisable.push(String8("print-logs"));
    argsDisable.push(String8("0"));

    service->cmd_print_logs(0, argsEnable);
    service->cmd_print_logs(0, argsDisable);
}

#else
GTEST_LOG_(INFO) << "This test does nothing.\n";
#endif

}  // namespace statsd
}  // namespace os
}  // namespace android
