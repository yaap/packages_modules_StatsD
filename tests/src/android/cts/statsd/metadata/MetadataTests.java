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
package android.cts.statsd.metadata;

import static com.google.common.truth.Truth.assertThat;
import static com.google.common.truth.Truth.assertWithMessage;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;

import com.android.compatibility.common.util.ApiLevelUtil;
import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.os.AtomsProto;
import com.android.os.AtomsProto.Atom;
import com.android.os.StatsLog.StatsdStatsReport;
import com.android.os.StatsLog.StatsdStatsReport.AtomStats;
import com.android.os.StatsLog.StatsdStatsReport.ConfigStats;
import com.android.os.StatsLog.StatsdStatsReport.LogLossStats;
import com.android.os.StatsLog.StatsdStatsReport.SocketLossStats.LossStatsPerUid;
import com.android.os.StatsLog.StatsdStatsReport.SocketLossStats.LossStatsPerUid.AtomIdLossStats;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.util.RunUtil;

import java.util.HashSet;

/**
 * Statsd Metadata tests.
 */
public class MetadataTests extends MetadataTestCase {

    private static final String TAG = "Statsd.MetadataTests";

    private static final int SHELL_UID = 2000;

    // Tests that the statsd config is reset after the specified ttl.
    public void testConfigTtl() throws Exception {
        final int TTL_TIME_SEC = 8;
        StatsdConfig.Builder config = getBaseConfig();
        config.setTtlInSeconds(TTL_TIME_SEC); // should reset in this many seconds.

        ConfigUtils.uploadConfig(getDevice(), config);
        long startTime = System.currentTimeMillis();
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AtomsProto.AppBreadcrumbReported.State.START.getNumber(), /* irrelevant val */
                6); // Event, within < TTL_TIME_SEC secs.
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        StatsdStatsReport report = getStatsdStatsReport(); // Has only been 1 second
        LogUtil.CLog.d("got following statsdstats report: " + report.toString());
        boolean foundActiveConfig = false;
        int creationTime = 0;
        for (ConfigStats stats : report.getConfigStatsList()) {
            if (stats.getId() == ConfigUtils.CONFIG_ID && stats.getUid() == SHELL_UID) {
                if (!stats.hasDeletionTimeSec()) {
                    assertWithMessage("Found multiple active CTS configs!")
                            .that(foundActiveConfig).isFalse();
                    foundActiveConfig = true;
                    creationTime = stats.getCreationTimeSec();
                }
            }
        }
        assertWithMessage("Did not find an active CTS config").that(foundActiveConfig).isTrue();

        while (System.currentTimeMillis() - startTime < 8_000) {
            RunUtil.getDefault().sleep(10);
        }
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AtomsProto.AppBreadcrumbReported.State.START.getNumber(), /* irrelevant val */
                6); // Event, after TTL_TIME_SEC secs.
        RunUtil.getDefault().sleep(2_000);
        report = getStatsdStatsReport();
        LogUtil.CLog.d("got following statsdstats report: " + report.toString());
        foundActiveConfig = false;
        int expectedTime = creationTime + TTL_TIME_SEC;
        for (ConfigStats stats : report.getConfigStatsList()) {
            if (stats.getId() == ConfigUtils.CONFIG_ID && stats.getUid() == SHELL_UID) {
                // Original config should be TTL'd
                if (stats.getCreationTimeSec() == creationTime) {
                    assertWithMessage("Config should have TTL'd but is still active")
                            .that(stats.hasDeletionTimeSec()).isTrue();
                    assertWithMessage(
                            "Config deletion time should be about %s after creation", TTL_TIME_SEC
                    ).that(Math.abs(stats.getDeletionTimeSec() - expectedTime)).isAtMost(3);
                }
                // There should still be one active config, that is marked as reset.
                if (!stats.hasDeletionTimeSec()) {
                    assertWithMessage("Found multiple active CTS configs!")
                            .that(foundActiveConfig).isFalse();
                    foundActiveConfig = true;
                    creationTime = stats.getCreationTimeSec();
                    assertWithMessage("Active config after TTL should be marked as reset")
                            .that(stats.hasResetTimeSec()).isTrue();
                    assertWithMessage("Reset and creation time should be equal for TTl'd configs")
                            .that(stats.getResetTimeSec()).isEqualTo(stats.getCreationTimeSec());
                    assertWithMessage(
                            "Reset config should be created when the original config TTL'd"
                    ).that(Math.abs(stats.getCreationTimeSec() - expectedTime)).isAtMost(3);
                }
            }
        }
        assertWithMessage("Did not find an active CTS config after the TTL")
                .that(foundActiveConfig).isTrue();
    }

    private static final int LIB_STATS_SOCKET_QUEUE_OVERFLOW_ERROR_CODE = 1;
    private static final int EVENT_STORM_ITERATIONS_COUNT = 10;

    /**
     * Tests that logging many atoms back to back potentially leads to socket overflow and data
     * loss. And if it happens the corresponding info is propagated to statsd stats
     */
    public void testAtomLossInfoCollection() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".StatsdStressLogging", "testLogAtomsBackToBack");

        StatsdStatsReport report = getStatsdStatsReport();
        assertThat(report).isNotNull();

        if (report.getDetectedLogLossList().size() == 0) {
            return;
        }
        // it can be the case that system throughput is sufficient to overcome the
        // simulated event storm, but if loss happens report can contain information about
        // atom of interest
        for (LogLossStats lossStats : report.getDetectedLogLossList()) {
            if (lossStats.getLastTag() == Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER) {
                return;
            }
        }

        if (report.getSocketLossStats() == null) {
            return;
        }
        // if many atoms were lost the information in DetectedLogLoss can be overwritten
        // looking into alternative stats to find the information
        for (LossStatsPerUid lossStats : report.getSocketLossStats().getLossStatsPerUidList()) {
            for (AtomIdLossStats atomLossStats : lossStats.getAtomIdLossStatsList()) {
                if (atomLossStats.getAtomId() == Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER) {
                    return;
                }
            }
        }
        org.junit.Assert.fail("Socket loss detected but no info about atom of interest");
    }

    /** Tests that SystemServer logged atoms in case of loss event has error code 1. */
    public void testSystemServerLossErrorCode() throws Exception {
        if (!sdkLevelAtLeast(34, "V")) {
            return;
        }

        // Starting from VanillaIceCream libstatssocket uses worker thread & dedicated logging queue
        // to handle atoms for system server (logged with UID 1000)
        // this test might fail for previous versions due to loss stats last error code check
        // will not pass

        // Due to info about system server atom loss could be overwritten by APP_BREADCRUMB_REPORTED
        // loss info run several iterations of this test
        for (int i = 0; i < EVENT_STORM_ITERATIONS_COUNT; i++) {
            LogUtil.CLog.d("testSystemServerLossErrorCode iteration #" + i);
            // logging back to back many atoms to force socket overflow
            DeviceUtils.runDeviceTests(
                    getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, ".StatsdStressLogging",
                    "testLogAtomsBackToBack");

            // Delay to allow statsd socket recover after overflow
            RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);

            // There is some un-deterministic component in AtomLossStats propagation:
            // - the dumpAtomsLossStats() from the libstatssocket happens ONLY after the
            //   next successful atom write to socket.
            // - to avoid socket flood there is also cooldown timer incorporated. If no new atoms -
            //   loss info will not be propagated, which is intention by design.
            // Log atoms into socket successfully to trigger libstatsocket dumpAtomsLossStats()
            AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                    AtomsProto.AppBreadcrumbReported.State.START.getNumber(), /* irrelevant val */
                    6); // Event, after TTL_TIME_SEC secs.

            // Delay to allow libstatssocket loss info to be propagated to statsdstats
            RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);

            StatsdStatsReport report = getStatsdStatsReport();
            assertThat(report).isNotNull();
            for (LogLossStats lossStats : report.getDetectedLogLossList()) {
                // it should not happen due to atoms from system servers logged via queue
                // which should be sufficient to hold them for some time to overcome the
                // socket overflow time frame
                if (lossStats.getUid() == 1000) {
                    // but if loss happens it should be annotated with predefined error code == 1
                    assertThat(lossStats.getLastError())
                            .isEqualTo(LIB_STATS_SOCKET_QUEUE_OVERFLOW_ERROR_CODE);
                }
            }

            // it can be the case that system throughput is sufficient to overcome the
            // simulated event storm
            if (report.getSocketLossStats() == null) {
                return;
            }

            // Verify that loss info is propagated via SocketLossStats atom
            for (LossStatsPerUid lossStats : report.getSocketLossStats().getLossStatsPerUidList()) {
                for (AtomIdLossStats atomLossStats : lossStats.getAtomIdLossStatsList()) {
                    // it should not happen due to atoms from system servers logged via queue
                    // which should be sufficient to hold them for some time to overcome the
                    // socket overflow time frame
                    if (lossStats.getUid() == 1000) {
                        // but if loss happens it should be annotated with predefined error
                        // code == 1
                        assertThat(atomLossStats.getError())
                                .isEqualTo(LIB_STATS_SOCKET_QUEUE_OVERFLOW_ERROR_CODE);
                    }
                }
            }
        }
    }

    /** Test libstatssocket logging queue atom id distribution collection */
    public void testAtomIdLossDistributionCollection() throws Exception {
        if (!sdkLevelAtLeast(34, "V")) {
            return;
        }

        final String testPkgName = "com.android.statsd.app.atomstorm";
        final String testApk = "StatsdAtomStormApp.apk";
        final int runAttemptsPerPackage = 10;

        String[][] testPkgs = {
            {testPkgName, ".StatsdAtomStorm", "testLogManyAtomsBackToBack"},
            {
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".StatsdStressLogging",
                "testLogAtomsBackToBack"
            }
        };

        DeviceUtils.uninstallTestApp(getDevice(), testPkgName);

        DeviceUtils.installTestApp(getDevice(), testApk, testPkgName, mCtsBuild);

        StatsdStatsReport report = getStatsdStatsReport();
        assertThat(report).isNotNull();

        HashSet<Integer> reportedUids = getSocketLossUids(report);

        // intention is to run two distinct package tests to collect 2 different uids
        for (String[] pkg : testPkgs) {
            report = runTestUntilLossAtomReported(pkg[0], pkg[1], pkg[2], runAttemptsPerPackage);
            if (report == null) {
                // the test run failed or the system throughput is sufficiently
                // high to consume all event from stress test
                DeviceUtils.uninstallTestApp(getDevice(), testPkgName);
                return;
            }
            reportedUids.addAll(getSocketLossUids(report));
        }

        assertThat(reportedUids.size()).isGreaterThan(1);

        DeviceUtils.uninstallTestApp(getDevice(), testPkgName);
    }

    private StatsdStatsReport runTestUntilLossAtomReported(
            String pkg, String cls, String test, int maxIterationCount) throws Exception {
        final int StatsSocketLossReportedAtomId = 752;

        // idea is to run event store test app to simulate atom loss event and
        // see if new StatsSocketLossReported instances are reported
        StatsdStatsReport report = getStatsdStatsReport();
        int socketLossAtomCount = getAtomStatsCount(report, StatsSocketLossReportedAtomId);
        for (int attempt = 0; attempt < maxIterationCount; attempt++) {
            DeviceUtils.runDeviceTests(getDevice(), pkg, cls, test);

            // the sleep is required since atoms are processed in async way by statsd
            // need to give time so statsd will process SocketLossStats atom
            RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
            LogUtil.CLog.d("runTestUntilLossAtomReported iteration " + attempt + " for " + pkg);
            report = getStatsdStatsReport();
            int newCount = getAtomStatsCount(report, StatsSocketLossReportedAtomId);
            if (socketLossAtomCount < newCount) {
                return report;
            }
        }
        return null;
    }

    private int getAtomStatsCount(StatsdStatsReport report, int atomId) {
        assertThat(report).isNotNull();
        for (AtomStats atomStats : report.getAtomStatsList()) {
            if (atomStats.getTag() == atomId) {
                return atomStats.getCount();
            }
        }
        return 0;
    }

    static private HashSet<Integer> getSocketLossUids(StatsdStatsReport report) {
        HashSet<Integer> result = new HashSet<Integer>();
        assertThat(report.getSocketLossStats()).isNotNull();
        for (LossStatsPerUid lossStats : report.getSocketLossStats().getLossStatsPerUidList()) {
            LogUtil.CLog.d(
                    "getSocketLossUids() collecting loss stats for uid "
                            + lossStats.getUid());
            result.add(lossStats.getUid());
        }
        return result;
    }

    private boolean sdkLevelAtLeast(int sdkLevel, String codename) throws Exception {
        return ApiLevelUtil.isAtLeast(getDevice(), sdkLevel)
                || ApiLevelUtil.codenameEquals(getDevice(), codename);
    }
}
