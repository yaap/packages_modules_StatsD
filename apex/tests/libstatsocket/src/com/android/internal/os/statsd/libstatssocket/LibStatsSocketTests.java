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

package com.android.internal.os.statsd.libstatssocket;

import static com.android.internal.os.statsdutils.StatsConfigUtils.SHORT_WAIT;

import static com.google.common.truth.Truth.assertThat;
import static com.google.common.truth.Truth.assertWithMessage;

import android.app.StatsManager;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.os.SystemProperties;
import android.platform.test.annotations.RequiresFlagsEnabled;
import android.platform.test.flag.junit.CheckFlagsRule;
import android.platform.test.flag.junit.DeviceFlagsValueProvider;
import android.util.Log;
import android.util.StatsLog;
import android.util.StatsdTestStatsLog;

import androidx.test.filters.FlakyTest;
import androidx.test.filters.LargeTest;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.runner.AndroidJUnit4;

import com.android.internal.os.StatsdConfigProto.FieldFilter;
import com.android.internal.os.StatsdConfigProto.GaugeMetric;
import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.internal.os.StatsdConfigProto.TimeUnit;
import com.android.internal.os.statsdutils.StatsConfigUtils;
import com.android.os.AtomsProto.Atom;
import com.android.os.StatsLog.ConfigMetricsReport;
import com.android.os.StatsLog.StatsdStatsReport;
import com.android.os.StatsLog.StatsdStatsReport.AtomStats;
import com.android.os.StatsLog.StatsdStatsReport.LogLossStats;
import com.android.os.StatsLog.StatsdStatsReport.SocketLossStats.LossStatsPerUid;
import com.android.os.StatsLog.StatsdStatsReport.SocketLossStats.LossStatsPerUid.AtomIdLossStats;
import com.android.os.StatsLogEnums.DataCorruptedReason;
import com.android.os.statsd.StatsdExtensionAtoms;
import com.android.os.statsd.flags.Flags;

import com.google.protobuf.ExtensionRegistryLite;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.util.List;

/** Tests for statsd pushed atoms log control. */
@RunWith(AndroidJUnit4.class)
public class LibStatsSocketTests {
    public static final String TAG = LibStatsSocketTests.class.getSimpleName();

    @Rule
    public final CheckFlagsRule mCheckFlagsRule = DeviceFlagsValueProvider.createCheckFlagsRule();

    private Context mContext;

    private static final int STATSD_INIT_DELAY_MS = 90_000; // 90 seconds

    private static final int LIBSTATSSOCKET_TTL_MS = 30_000; // 30 seconds

    private static final int ATOM_TAG = StatsdTestStatsLog.TEST_ATOM_REPORTED;
    private static final int UNUSED_ATOM_TAG = StatsdTestStatsLog.TEST_EXTENSION_ATOM_REPORTED;

    private long activeConfig = 0;

    /** Test specific set up */
    @Before
    public void setUp() throws Exception {
        assertThat(InstrumentationRegistry.getInstrumentation()).isNotNull();
        mContext = InstrumentationRegistry.getInstrumentation().getTargetContext();

        StatsManager statsManager = mContext.getSystemService(StatsManager.class);
        // add config with only one atom
        activeConfig = createAndAddConfigPushedToStatsd(statsManager, new int[] {ATOM_TAG});
        assertWithMessage("Error while setup test config").that(activeConfig).isGreaterThan(0);
    }

    /** Test specific set up */
    @After
    public void tearDown() throws Exception {
        assertThat(InstrumentationRegistry.getInstrumentation()).isNotNull();
        mContext = InstrumentationRegistry.getInstrumentation().getTargetContext();

        StatsManager statsManager = mContext.getSystemService(StatsManager.class);
        statsManager.removeConfig(activeConfig);
    }

    /**
     * Test that a generates 2 atoms while config collects only 1. Second atom should not be pushed
     * to the socket due to being unused
     */
    @Test(timeout = 180_000)
    @LargeTest
    @FlakyTest
    @RequiresFlagsEnabled(Flags.FLAG_LOGGING_CONTROL_ENABLED)
    public void testLoggingControlAtomNotInUse() throws Exception {
        // This test must be executed at least in 90 seconds after statsd start
        // Socket Logging Control inactive during 90 seconds after statsd start
        // That is why wait for STATSD_INIT_DELAY_MS incorporated if necessary

        ApplicationInfo appInfo =
                mContext.getPackageManager().getApplicationInfo(mContext.getPackageName(), 0);

        StatsManager statsManager = mContext.getSystemService(StatsManager.class);

        // store number of atoms pushed so far
        StatsdStatsReport report = getStatsdStatsReport(statsManager);
        AtomStats prevAtomStats = getAtomStats(report, ATOM_TAG);
        AtomStats prevAtomNotInUseStats = getAtomStats(report, UNUSED_ATOM_TAG);

        assertWithMessage("StatsdLoggingControl.atoms_in_use_list_version() should be > 0")
                .that(waitForStatsServiceLoggingControl(STATSD_INIT_DELAY_MS + SHORT_WAIT))
                .isTrue();

        // to enforce logging control ttl timer to expire from any past atoms logs
        sleep(LIBSTATSSOCKET_TTL_MS + SHORT_WAIT);
        // at this moment libstatssocket TTL (30 sec) of cache should be over and it
        // will re-load list of atoms in use from file

        writeTestAtom(appInfo);
        writeExtensionTestAtom(appInfo);

        sleep(SHORT_WAIT);

        // collect statsdstats to validate number of atoms logged via socket
        report = getStatsdStatsReport(statsManager);
        AtomStats currentAtomStats = getAtomStats(report, ATOM_TAG);
        AtomStats currentAtomNotInUseStats = getAtomStats(report, UNUSED_ATOM_TAG);

        compareAtomStatsIncreased(prevAtomStats, currentAtomStats);
        compareAtomStatsEqual(prevAtomNotInUseStats, currentAtomNotInUseStats);
        prevAtomStats = currentAtomStats;
        prevAtomNotInUseStats = currentAtomNotInUseStats;

        {
            List<Atom> data = StatsConfigUtils.getGaugeMetricDataList(statsManager, activeConfig);
            assertThat(data).hasSize(1);
            assertThat(data.get(0).hasTestAtomReported()).isTrue();
        }
        statsManager.removeConfig(activeConfig);

        sleep(LIBSTATSSOCKET_TTL_MS + SHORT_WAIT);

        // at this moment libstatssocket TTL (30 sec) of cache should be over and it
        // will re-load list of atoms in use from file
        // now both atoms should become disabled

        writeTestAtom(appInfo);
        writeExtensionTestAtom(appInfo);

        sleep(SHORT_WAIT);

        // collect statsdstats to validate number of atoms logged via socket
        report = getStatsdStatsReport(statsManager);
        currentAtomStats = getAtomStats(report, ATOM_TAG);
        currentAtomNotInUseStats = getAtomStats(report, UNUSED_ATOM_TAG);

        compareAtomStatsEqual(prevAtomStats, currentAtomStats);
        compareAtomStatsEqual(prevAtomNotInUseStats, currentAtomNotInUseStats);
        prevAtomStats = currentAtomStats;
        prevAtomNotInUseStats = currentAtomNotInUseStats;

        // update config to collect 2 atoms - atom logging control must enable
        // previously disabled atoms
        activeConfig =
                createAndAddConfigPushedToStatsd(
                        statsManager, new int[] {ATOM_TAG, UNUSED_ATOM_TAG});

        sleep(LIBSTATSSOCKET_TTL_MS + SHORT_WAIT);
        // at this moment libstatssocket TTL (30 sec) of cache should be over and it
        // will re-load list of atoms in use from file

        writeTestAtom(appInfo);
        writeExtensionTestAtom(appInfo);

        sleep(SHORT_WAIT);

        report = getStatsdStatsReport(statsManager);
        currentAtomStats = getAtomStats(report, ATOM_TAG);
        currentAtomNotInUseStats = getAtomStats(report, UNUSED_ATOM_TAG);
        compareAtomStatsIncreased(prevAtomStats, currentAtomStats);
        compareAtomStatsIncreased(prevAtomNotInUseStats, currentAtomNotInUseStats);

        {
            ExtensionRegistryLite extensionRegistry = ExtensionRegistryLite.newInstance();
            StatsdExtensionAtoms.registerAllExtensions(extensionRegistry);
            List<Atom> data =
                    StatsConfigUtils.getGaugeMetricDataList(
                            statsManager, activeConfig, extensionRegistry);
            assertThat(data).hasSize(2);
            assertThat(data.get(0).hasTestAtomReported()).isTrue();
            assertThat(data.get(1).getExtension(StatsdExtensionAtoms.testExtensionAtomReported))
                    .isNotNull();
        }
    }

    private static final int LIB_STATS_SOCKET_RATE_LIMIT_ERROR_CODE = 2;

    /** Tests logging rate limiting applied by libstatssocket */
    @Test
    public void testSocketRateLimiting() throws Exception {

        logAtomsBackToBack();

        triggerAtomLossStatsPropagation();

        StatsManager statsManager = mContext.getSystemService(StatsManager.class);
        StatsdStatsReport report = getStatsdStatsReport(statsManager);
        assertThat(report).isNotNull();

        if (report.getDetectedLogLossList().size() == 0) {
            return;
        }
        // it can be the case that system throughput is sufficient to overcome the
        // simulated event storm, but if loss happened - report can contain information about
        // atom of interest
        for (LogLossStats lossStats : report.getDetectedLogLossList()) {
            if (lossStats.getLastTag() == Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER) {
                assertThat(lossStats.getLastError())
                        .isEqualTo(LIB_STATS_SOCKET_RATE_LIMIT_ERROR_CODE);

                // log loss due to rate limit should not contribute to report level data corruption
                // reason
                ConfigMetricsReport metricsReport =
                        StatsConfigUtils.getConfigMetricsReport(statsManager, activeConfig);
                for (DataCorruptedReason reason : metricsReport.getDataCorruptedReasonList()) {
                    assertThat(reason).isNotEqualTo(DataCorruptedReason.DATA_CORRUPTED_SOCKET_LOSS);
                }
                return;
            }
        }

        if (!report.hasSocketLossStats()) {
            return;
        }
        // if many atoms were lost the information in DetectedLogLoss can be overwritten
        // looking into alternative stats to find the information
        for (LossStatsPerUid lossStats : report.getSocketLossStats().getLossStatsPerUidList()) {
            for (AtomIdLossStats atomLossStats : lossStats.getAtomIdLossStatsList()) {
                if (atomLossStats.getAtomId() == Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER) {
                    assertThat(atomLossStats.getError())
                            .isEqualTo(LIB_STATS_SOCKET_RATE_LIMIT_ERROR_CODE);
                    return;
                }
            }
        }
        org.junit.Assert.fail("Socket loss detected but no info about atom of interest");
    }

    private static boolean waitForStatsServiceLoggingControl(long waitTime) throws Exception {
        int counter = 1;
        long startTime = System.currentTimeMillis();
        while ((System.currentTimeMillis() - startTime) < waitTime) {
            String version = SystemProperties.get("statsd.config.atoms_in_use_list.version");
            if (version != null && !version.isEmpty() && Long.parseLong(version) > 0) {
                return true;
            }
            sleep(Math.min(200 * counter, 2_000));
            counter++;
        }
        return false;
    }

    private AtomStats getAtomStats(StatsdStatsReport report, int atomTag) {
        for (AtomStats atomStats : report.getAtomStatsList()) {
            if (atomStats.getTag() == atomTag) {
                return atomStats;
            }
        }
        return null;
    }

    private void compareAtomStatsIncreased(
            AtomStats prevAtomStats, AtomStats currentAtomStats) {
        assertThat(currentAtomStats).isNotNull();
        if (prevAtomStats == null) {
            assertThat(currentAtomStats.getCount()).isEqualTo(1);
            assertThat(currentAtomStats.getErrorCount()).isEqualTo(0);
            assertThat(currentAtomStats.getDroppedCount()).isEqualTo(0);
            assertThat(currentAtomStats.getSkipCount()).isEqualTo(0);
        } else {
            assertThat(currentAtomStats.getCount()).isEqualTo(prevAtomStats.getCount() + 1);
            assertThat(currentAtomStats.getErrorCount()).isEqualTo(prevAtomStats.getErrorCount());
            assertThat(currentAtomStats.getDroppedCount())
                    .isEqualTo(prevAtomStats.getDroppedCount());
            assertThat(currentAtomStats.getSkipCount()).isEqualTo(prevAtomStats.getSkipCount());
        }
    }

    private void compareAtomStatsEqual(
            AtomStats prevAtomNotInUseStats, AtomStats currentAtomNotInUseStats) {
        if (prevAtomNotInUseStats == null) {
            assertThat(currentAtomNotInUseStats).isNull();
        } else {
            assertThat(currentAtomNotInUseStats).isNotNull();
            assertThat(currentAtomNotInUseStats.getCount())
                    .isEqualTo(prevAtomNotInUseStats.getCount());
            assertThat(currentAtomNotInUseStats.getErrorCount())
                    .isEqualTo(prevAtomNotInUseStats.getErrorCount());
            assertThat(currentAtomNotInUseStats.getDroppedCount())
                    .isEqualTo(prevAtomNotInUseStats.getDroppedCount());
            assertThat(currentAtomNotInUseStats.getSkipCount())
                    .isEqualTo(prevAtomNotInUseStats.getSkipCount());
        }
    }

    private StatsdStatsReport getStatsdStatsReport(StatsManager statsManager) {
        StatsdStatsReport report = null;
        try {
            report = StatsdStatsReport.parser().parseFrom(statsManager.getStatsMetadata());
        } catch (Exception e) {
            Log.e(TAG, "getMetadata failed", e);
        }
        assertThat(report).isNotNull();
        return report;
    }

    private long createAndAddConfigPushedToStatsd(StatsManager statsManager, int[] atomIds)
            throws Exception {

        // pushing config to collect atom TEST_EXTENSION_ATOM_REPORTED while test will
        // report
        // atoms TEST_EXTENSION_ATOM_REPORTED and TEST_ATOM_REPORTED.
        // Since TEST_ATOM_REPORTED is not referenced by config
        // statsd should prevent logging it via libstatssocket logging control
        // propagation
        // which can be validated via statsdstats for pushed atoms.

        long configId = System.currentTimeMillis();
        long triggerMatcherIdStart = configId + 10;
        long metricIdStart = configId + 100;
        StatsdConfig.Builder configBuilder = StatsConfigUtils.getSimpleTestConfig(configId,
                LibStatsSocketTests.class.getPackageName());

        for (int atomIdx = 0; atomIdx < atomIds.length; atomIdx++) {

            final int atomId = atomIds[atomIdx];
            final long matcherId = triggerMatcherIdStart + atomIdx;
            final long metricId = metricIdStart + atomIdx;

            configBuilder.addAtomMatcher(StatsConfigUtils.getSimpleAtomMatcher(atomId, matcherId));
            configBuilder.addGaugeMetric(
                    GaugeMetric.newBuilder()
                            .setId(metricId)
                            .setWhat(matcherId)
                            .setGaugeFieldsFilter(FieldFilter.newBuilder().setIncludeAll(true))
                            .setBucket(TimeUnit.CTS)
                            .setSamplingType(GaugeMetric.SamplingType.FIRST_N_SAMPLES)
                            .setMaxNumGaugeAtomsPerBucket(10000000));

            configBuilder.addWhitelistedAtomIds(atomId);
        }
        StatsdConfig config = configBuilder.build();
        statsManager.addConfig(configId, config.toByteArray());
        assertThat(StatsConfigUtils.verifyValidConfigExists(statsManager, configId)).isTrue();

        return configId;
    }

    /** Puts the current thread to sleep. */
    static void sleep(int millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Log.e(TAG, "Interrupted exception while sleeping", e);
        }
    }

    static void writeTestAtom(ApplicationInfo appInfo) {
        int[] uids = {1234, appInfo.uid};
        String[] tags = {"tag1", "tag2"};
        byte[] experimentIds = {8, 1, 8, 2, 8, 3}; // Corresponds to 1, 2, 3.

        int[] int32Array = {3, 6};
        long[] int64Array = {1000L, 1002L};
        float[] floatArray = {0.3f, 0.09f};
        String[] stringArray = {"str1", "str2"};
        boolean[] boolArray = {true, false};
        int[] enumArray = {
            StatsdTestStatsLog.TEST_ATOM_REPORTED__STATE__OFF,
            StatsdTestStatsLog.TEST_ATOM_REPORTED__STATE__ON
        };

        StatsdTestStatsLog.write(
                StatsdTestStatsLog.TEST_ATOM_REPORTED,
                uids,
                tags,
                42,
                Long.MAX_VALUE,
                3.14f,
                "This is a basic test!",
                false,
                StatsdTestStatsLog.TEST_ATOM_REPORTED__STATE__ON,
                experimentIds,
                int32Array,
                int64Array,
                floatArray,
                stringArray,
                boolArray,
                enumArray);
    }

    static void writeExtensionTestAtom(ApplicationInfo appInfo) {
        int[] uids = {1234, appInfo.uid};
        String[] tags = {"tag1", "tag2"};
        byte[] testAtomNestedMsg = {8, 1, 8, 2, 8, 3}; // Corresponds to 1, 2, 3.

        int[] int32Array = {3, 6};
        long[] int64Array = {1000L, 1002L};
        float[] floatArray = {0.3f, 0.09f};
        String[] stringArray = {"str1", "str2"};
        boolean[] boolArray = {true, false};
        int[] enumArray = {
            StatsdTestStatsLog.TEST_EXTENSION_ATOM_REPORTED__STATE__OFF,
            StatsdTestStatsLog.TEST_EXTENSION_ATOM_REPORTED__STATE__ON
        };

        StatsdTestStatsLog.write(
                StatsdTestStatsLog.TEST_EXTENSION_ATOM_REPORTED,
                uids,
                tags,
                42,
                Long.MAX_VALUE,
                3.14f,
                "This is a basic test!",
                false,
                StatsdTestStatsLog.TEST_EXTENSION_ATOM_REPORTED__STATE__ON,
                testAtomNestedMsg,
                int32Array,
                int64Array,
                floatArray,
                stringArray,
                boolArray,
                enumArray,
                int32Array,
                int32Array,
                int32Array);
    }

    private static final int EVENT_STORM_ATOMS_COUNT = 50000;
    private static final int RELAXED_LOGGING_ATOMS_COUNT = 10;
    private static final int RECOMMENDED_LOGGING_INTERVAL_MS = 10;

    private void logAtomsBackToBack() throws Exception {
        // logging back to back many atoms to force socket overflow
        logAtomsBackToBack(EVENT_STORM_ATOMS_COUNT, 0);

        // Due to the nature of stress test there is some unpredictability aspect, repeating
        // natural atom logging flow several times to have higher guaranty of atom delivery
        // including recommended delay between logging atoms
        for (int i = 0; i < RELAXED_LOGGING_ATOMS_COUNT; i++) {
            sleep(RECOMMENDED_LOGGING_INTERVAL_MS);
            // give chance for libstatssocket send loss stats to statsd triggering
            // successful logging
            logAtomsBackToBack(1, RECOMMENDED_LOGGING_INTERVAL_MS);
        }
    }

    private void logAtomsBackToBack(int iterations, int loggingDelayMillis) throws Exception {
        // single atom logging takes ~2us excluding JNI interactions
        for (int i = 0; i < iterations; i++) {
            StatsLog.logStart(i);
            if (loggingDelayMillis > 0) {
                sleep(loggingDelayMillis);
            }
            StatsLog.logStop(i);
        }
    }

    private static final int STATS_SOCKET_LOSS_INFO_CACHE_TTL_MS = 60_000;

    private void triggerAtomLossStatsPropagation() throws Exception {
        // Delay to allow statsd socket recover after overflow
        sleep(STATS_SOCKET_LOSS_INFO_CACHE_TTL_MS);

        // There is some un-deterministic component in AtomLossStats propagation:
        // - the dumpAtomsLossStats() from the libstatssocket happens ONLY after the
        //   next successful atom write to socket.
        // - to avoid socket flood there is also cooldown timer incorporated. If no new atoms -
        //   loss info will not be propagated, which is intention by design.
        // Log atoms into socket successfully to trigger libstatsocket dumpAtomsLossStats()
        logAtomsBackToBack(1, 10);
        // Delay to allow libstatssocket loss info to be propagated to statsdstats
        sleep(SHORT_WAIT);
    }
}
