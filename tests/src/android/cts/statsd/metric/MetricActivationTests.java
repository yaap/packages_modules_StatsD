/*
 * Copyright (C) 2019 The Android Open Source Project
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
package android.cts.statsd.metric;

import static com.google.common.truth.Truth.assertThat;
import static com.google.common.truth.Truth.assertWithMessage;

import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.internal.os.StatsdConfigProto.ActivationType;
import com.android.internal.os.StatsdConfigProto.AtomMatcher;
import com.android.internal.os.StatsdConfigProto.EventActivation;
import com.android.internal.os.StatsdConfigProto.EventMetric;
import com.android.internal.os.StatsdConfigProto.MetricActivation;
import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.os.AtomsProto.AppBreadcrumbReported;
import com.android.os.StatsLog.ConfigMetricsReport;
import com.android.os.StatsLog.ConfigMetricsReportList;
import com.android.os.StatsLog.EventMetricData;
import com.android.os.StatsLog.StatsLogReport;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.ExtensionRegistry;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.stream.Collectors;

/**
 * Test Statsd Metric activations and deactivations
 */
public class MetricActivationTests extends DeviceTestCase {
    private final long metric1Id = 1L;
    private final int metric1MatcherId = 1;

    private final long metric2Id = 2L;
    private final int metric2MatcherId = 2;

    private final long metric3Id = 3L;
    private final int metric3MatcherId = 3;

    private final int act1MatcherId = 10;
    private final int act1CancelMatcherId = -10;

    private final int act2MatcherId = 20;
    private final int act2CancelMatcherId = -20;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        RunUtil.getDefault().sleep(1000);
    }

    @Override
    protected void tearDown() throws Exception {
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        super.tearDown();
    }

    private StatsdConfig.Builder createConfig(final int act1TtlSecs, final int act2TtlSecs) {
        AtomMatcher metric1Matcher =
                MetricsUtils.simpleAtomMatcher(metric1MatcherId, metric1MatcherId);
        AtomMatcher metric2Matcher =
                MetricsUtils.simpleAtomMatcher(metric2MatcherId, metric2MatcherId);
        AtomMatcher metric3Matcher =
                MetricsUtils.simpleAtomMatcher(metric3MatcherId, metric3MatcherId);
        AtomMatcher act1Matcher = MetricsUtils.simpleAtomMatcher(act1MatcherId, act1MatcherId);
        AtomMatcher act1CancelMatcher =
                MetricsUtils.simpleAtomMatcher(act1CancelMatcherId, act1CancelMatcherId);
        AtomMatcher act2Matcher = MetricsUtils.simpleAtomMatcher(act2MatcherId, act2MatcherId);
        AtomMatcher act2CancelMatcher =
                MetricsUtils.simpleAtomMatcher(act2CancelMatcherId, act2CancelMatcherId);

        EventMetric metric1 =
                EventMetric.newBuilder().setId(metric1Id).setWhat(metric1MatcherId).build();

        EventMetric metric2 =
                EventMetric.newBuilder().setId(metric2Id).setWhat(metric2MatcherId).build();

        EventMetric metric3 =
                EventMetric.newBuilder().setId(metric3Id).setWhat(metric3MatcherId).build();

        EventActivation metric1Act1 =
                MetricsUtils.createEventActivation(act1TtlSecs, act1MatcherId, act1CancelMatcherId)
                        .setActivationType(ActivationType.ACTIVATE_IMMEDIATELY)
                        .build();

        EventActivation metric1Act2 =
                MetricsUtils.createEventActivation(act2TtlSecs, act2MatcherId, act2CancelMatcherId)
                        .setActivationType(ActivationType.ACTIVATE_ON_BOOT)
                        .build();

        EventActivation metric2Act1 =
                MetricsUtils.createEventActivation(act1TtlSecs, act1MatcherId, act1CancelMatcherId)
                        .setActivationType(ActivationType.ACTIVATE_ON_BOOT)
                        .build();

        EventActivation metric2Act2 =
                MetricsUtils.createEventActivation(act2TtlSecs, act2MatcherId, act2CancelMatcherId)
                        .setActivationType(ActivationType.ACTIVATE_IMMEDIATELY)
                        .build();

        MetricActivation metric1Activation = MetricActivation.newBuilder()
                .setMetricId(metric1Id)
                .addEventActivation(metric1Act1)
                .addEventActivation(metric1Act2)
                .build();

        MetricActivation metric2Activation = MetricActivation.newBuilder()
                .setMetricId(metric2Id)
                .addEventActivation(metric2Act1)
                .addEventActivation(metric2Act2)
                .build();


        return ConfigUtils.createConfigBuilder(MetricsUtils.DEVICE_SIDE_TEST_PACKAGE)
                .addAtomMatcher(metric1Matcher)
                .addAtomMatcher(metric2Matcher)
                .addAtomMatcher(metric3Matcher)
                .addAtomMatcher(act1Matcher)
                .addAtomMatcher(act1CancelMatcher)
                .addAtomMatcher(act2Matcher)
                .addAtomMatcher(act2CancelMatcher)
                .addEventMetric(metric1)
                .addEventMetric(metric2)
                .addEventMetric(metric3)
                .addMetricActivation(metric1Activation)
                .addMetricActivation(metric2Activation);
    }

    /**
     * Metric 1:
     * Activation 1:
     * - Ttl: 5 seconds
     * - Type: IMMEDIATE
     * Activation 2:
     * - Ttl: 8 seconds
     * - Type: ON_BOOT
     *
     * Metric 2:
     * Activation 1:
     * - Ttl: 5 seconds
     * - Type: ON_BOOT
     * Activation 2:
     * - Ttl: 8 seconds
     * - Type: IMMEDIATE
     *
     * Metric 3: No activations; always active
     **/
    public void testCancellation() throws Exception {
        final int act1TtlSecs = 5;
        final int act2TtlSecs = 8;
        ConfigUtils.uploadConfig(getDevice(), createConfig(act1TtlSecs, act2TtlSecs));

        // Ignored, metric not active.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Trigger cancel for already inactive event activation 1.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1CancelMatcherId);
        RunUtil.getDefault().sleep(10L);

        // Trigger event activation 1.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // First logged event.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Second logged event.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Cancel event activation 1.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1CancelMatcherId);
        RunUtil.getDefault().sleep(10L);

        // Ignored, metric not active.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Trigger event activation 1.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Trigger event activation 2.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act2MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Third logged event.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Cancel event activation 2.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act2CancelMatcherId);
        RunUtil.getDefault().sleep(10L);

        // Fourth logged event.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Expire event activation 1
        RunUtil.getDefault().sleep(act1TtlSecs * 1000);

        // Ignored, metric 1 not active. Activation 1 expired and Activation 2 was cancelled.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Trigger event activation 2.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act2MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Metric 1 log ignored, Activation 1 expired and Activation 2 needs reboot to activate.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // First logged event for Metric 3.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric3MatcherId);
        RunUtil.getDefault().sleep(10L);

        ConfigMetricsReportList reportList = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        List<ConfigMetricsReport> reports = getSortedConfigMetricsReports(reportList);
        ConfigMetricsReport report = reports.get(0);
        verifyMetrics(report, 4, 0, 1);
    }

    /**
     * Metric 1:
     * Activation 1:
     * - Ttl: 100 seconds
     * - Type: IMMEDIATE
     * Activation 2:
     * - Ttl: 200 seconds
     * - Type: ON_BOOT
     *
     * Metric 2:
     * Activation 1:
     * - Ttl: 100 seconds
     * - Type: ON_BOOT
     * Activation 2:
     * - Ttl: 200 seconds
     * - Type: IMMEDIATE
     *
     * Metric 3: No activations; always active
     **/
    public void testRestart() throws Exception {
        final int act1TtlSecs = 200;
        final int act2TtlSecs = 400;
        ConfigUtils.uploadConfig(getDevice(), createConfig(act1TtlSecs, act2TtlSecs));

        // Trigger Metric 1 Activation 1 and Metric 2 Activation 1.
        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds (will activate after boot)
        // Metric 2 Activation 2: 0 seconds
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // First logged event for Metric 1.
        // Metric 2 event ignored, will activate after boot.
        // First logged event for Metric 3.
        logAllMetrics();

        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 200 seconds
        // Metric 2 Activation 2: 0 seconds
        DeviceUtils.rebootDeviceAndWaitUntilReady(getDevice());

        // Second logged event for Metric 1.
        // First logged event for Metric 2.
        // Second logged event for Metric 3.
        logAllMetrics();

        // Time remaining:
        // Metric 1 Activation 1: 0 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds
        // Metric 2 Activation 2: 0 seconds
        RunUtil.getDefault().sleep(act1TtlSecs * 1000L);

        // Metric 1 event ignored, Activation 1 expired.
        // Metric 2 event ignored, Activation 1 expired.
        // Third logged event for Metric 3.
        logAllMetrics();

        // Trigger Metric 1 Activation 2 and Metric 2 Activation 2.
        // Time remaining:
        // Metric 1 Activation 1: 0 seconds
        // Metric 1 Activation 2: 0 seconds (will activate after boot)
        // Metric 2 Activation 1: 0 seconds
        // Metric 2 Activation 2: 400 seconds
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act2MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Metric 1 event ignored, will activate after boot.
        // Second logged event for Metric 2.
        // Fourth logged event for Metric 3.
        logAllMetrics();

        // Trigger Metric 1 Activation 1 and Metric 2 Activation 1.
        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds (will activate after boot)
        // Metric 2 Activation 1: 0 seconds (will activate after boot)
        // Metric 2 Activation 2: 400 seconds
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Third logged event for Metric 1.
        // Third logged event for Metric 2.
        // Fifth logged event for Metric 3.
        logAllMetrics();

        // Time remaining:
        // Metric 1 Activation 1: 100 seconds
        // Metric 1 Activation 2: 0 seconds (will activate after boot)
        // Metric 2 Activation 1: 0 seconds (will activate after boot)
        // Metric 2 Activation 2: 300 seconds
        RunUtil.getDefault().sleep(act1TtlSecs * 1000L / 2);

        // Time remaining:
        // Metric 1 Activation 1: 100 seconds
        // Metric 1 Activation 2: 400 seconds
        // Metric 2 Activation 1: 200 seconds
        // Metric 2 Activation 2: 300 seconds
        DeviceUtils.rebootDeviceAndWaitUntilReady(getDevice());

        // Fourth logged event for Metric 1.
        // Fourth logged event for Metric 2.
        // Sixth logged event for Metric 3.
        logAllMetrics();

        // Expire Metric 1 Activation 1.
        // Time remaining:
        // Metric 1 Activation 1: 0 seconds
        // Metric 1 Activation 2: 300 seconds
        // Metric 2 Activation 1: 100 seconds
        // Metric 2 Activation 2: 200 seconds
        RunUtil.getDefault().sleep(act1TtlSecs * 1000L / 2);

        // Fifth logged event for Metric 1.
        // Fifth logged event for Metric 2.
        // Seventh logged event for Metric 3.
        logAllMetrics();

        // Expire all activations.
        // Time remaining:
        // Metric 1 Activation 1: 0 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds
        // Metric 2 Activation 2: 0 seconds
        RunUtil.getDefault().sleep(act2TtlSecs * 1000L);

        // Metric 1 event ignored.
        // Metric 2 event ignored.
        // Eighth logged event for Metric 3.
        logAllMetrics();

        ConfigMetricsReportList reportList = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        List<ConfigMetricsReport> reports = getSortedConfigMetricsReports(reportList);
        assertThat(reports).hasSize(3);

        // Report before restart.
        ConfigMetricsReport report = reports.get(0);
        verifyMetrics(report, 1, 0, 1);

        // Report after first restart.
        report = reports.get(1);
        verifyMetrics(report, 2, 3, 4);

        // Report after second restart.
        report = reports.get(2);
        verifyMetrics(report, 2, 2, 3);
    }

    /**
     * Metric 1:
     * Activation 1:
     * - Ttl: 100 seconds
     * - Type: IMMEDIATE
     * Activation 2:
     * - Ttl: 200 seconds
     * - Type: ON_BOOT
     *
     * Metric 2:
     * Activation 1:
     * - Ttl: 100 seconds
     * - Type: ON_BOOT
     * Activation 2:
     * - Ttl: 200 seconds
     * - Type: IMMEDIATE
     *
     * Metric 3: No activations; always active
     **/
    public void testMultipleActivations() throws Exception {
        final int act1TtlSecs = 200;
        final int act2TtlSecs = 400;
        ConfigUtils.uploadConfig(getDevice(), createConfig(act1TtlSecs, act2TtlSecs));

        // Trigger Metric 1 Activation 1 and Metric 2 Activation 1.
        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds (will activate after boot)
        // Metric 2 Activation 2: 0 seconds
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // First logged event for Metric 1.
        // Metric 2 event ignored, will activate after boot.
        // First logged event for Metric 3.
        logAllMetrics();

        // Time remaining:
        // Metric 1 Activation 1: 100 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds (will activate after boot)
        // Metric 2 Activation 2: 0 seconds
        RunUtil.getDefault().sleep(act1TtlSecs * 1000L / 2);

        // Second logged event for Metric 1.
        // Metric 2 event ignored, will activate after boot.
        // Second logged event for Metric 3.
        logAllMetrics();

        // Trigger Metric 1 Activation 1 and Metric 2 Activation 1.
        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds (will activate after boot)
        // Metric 2 Activation 2: 0 seconds
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Third logged event for Metric 1.
        // Metric 2 event ignored, will activate after boot.
        // Third logged event for Metric 3.
        logAllMetrics();

        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 200 seconds
        // Metric 2 Activation 2: 0 seconds
        DeviceUtils.rebootDeviceAndWaitUntilReady(getDevice());

        // Fourth logged event for Metric 1.
        // First logged event for Metric 2.
        // Fourth logged event for Metric 3.
        logAllMetrics();

        // Trigger Metric 1 Activation 1 and Metric 2 Activation 1.
        // Time remaining:
        // Metric 1 Activation 1: 200 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 200 seconds
        // Metric 2 Activation 2: 0 seconds
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), act1MatcherId);
        RunUtil.getDefault().sleep(10L);

        // Fifth logged event for Metric 1.
        // Second logged event for Metric 2.
        // Fifth logged event for Metric 3.
        logAllMetrics();

        // Expire all activations.
        // Time remaining:
        // Metric 1 Activation 1: 0 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds
        // Metric 2 Activation 2: 0 seconds
        RunUtil.getDefault().sleep(act1TtlSecs * 1000L);

        // Metric 1 event ignored.
        // Metric 2 event ignored.
        // Sixth logged event for Metric 3.
        logAllMetrics();

        // Time remaining:
        // Metric 1 Activation 1: 0 seconds
        // Metric 1 Activation 2: 0 seconds
        // Metric 2 Activation 1: 0 seconds
        // Metric 2 Activation 2: 0 seconds
        DeviceUtils.rebootDeviceAndWaitUntilReady(getDevice());
        RunUtil.getDefault().sleep(10_000L);

        // Metric 1 event ignored.
        // Metric 2 event ignored.
        // Seventh logged event for Metric 3.
        logAllMetrics();

        ConfigMetricsReportList reportList = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        List<ConfigMetricsReport> reports = getSortedConfigMetricsReports(reportList);
        assertThat(reports).hasSize(3);

        // Report before restart.
        ConfigMetricsReport report = reports.get(0);
        verifyMetrics(report, 3, 0, 3);

        // Report after first restart.
        report = reports.get(1);
        verifyMetrics(report, 2, 2, 3);

        // Report after second restart.
        report = reports.get(2);
        verifyMetrics(report, 0, 0, 1);
    }

    /**
     * Gets a List of sorted ConfigMetricsReports from ConfigMetricsReportList.
     */
    private List<ConfigMetricsReport> getSortedConfigMetricsReports(
            ConfigMetricsReportList configMetricsReportList) {
        return configMetricsReportList.getReportsList().stream().sorted(
                Comparator.comparing(ConfigMetricsReport::getCurrentReportWallClockNanos)).collect(
                Collectors.toList());
    }

    private void logAllMetrics() throws Exception {
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric1MatcherId);
        RunUtil.getDefault().sleep(10L);

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric2MatcherId);
        RunUtil.getDefault().sleep(10L);

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), metric3MatcherId);
        RunUtil.getDefault().sleep(10L);
    }

    private void verifyMetrics(ConfigMetricsReport report, int metric1Count, int metric2Count,
            int metric3Count) throws Exception {
        assertThat(report.getMetricsCount()).isEqualTo(3);

        verifyMetric(report.getMetrics(0),   // StatsLogReport
                1,                      // Metric Id
                1,                      // Metric what atom matcher label
                metric1Count            // Data count
        );
        verifyMetric(report.getMetrics(1),   // StatsLogReport
                2,                      // Metric Id
                2,                      // Metric what atom matcher label
                metric2Count            // Data count
        );
        verifyMetric(report.getMetrics(2),   // StatsLogReport
                3,                      // Metric Id
                3,                      // Metric what atom matcher label
                metric3Count            // Data count
        );
    }

    private void verifyMetric(StatsLogReport metricReport, long metricId, int metricMatcherLabel,
            int dataCount) {
        LogUtil.CLog.d("Got the following event metric data: " + metricReport.toString());
        assertThat(metricReport.getMetricId()).isEqualTo(metricId);
        assertThat(metricReport.hasEventMetrics()).isEqualTo(dataCount > 0);

        StatsLogReport.EventMetricDataWrapper eventData = metricReport.getEventMetrics();
        List<EventMetricData> eventMetricDataList = new ArrayList<>();
        for (EventMetricData eventMetricData : eventData.getDataList()) {
            eventMetricDataList.addAll(
                    ReportUtils.backfillAggregatedAtomsInEventMetric(eventMetricData));
        }
        assertThat(eventMetricDataList).hasSize(dataCount);
        for (EventMetricData eventMetricData : eventMetricDataList) {
            AppBreadcrumbReported atom = eventMetricData.getAtom().getAppBreadcrumbReported();
            assertThat(atom.getLabel()).isEqualTo(metricMatcherLabel);
        }
    }
}
