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
package android.cts.statsd.metric;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.internal.os.StatsdConfigProto;
import com.android.internal.os.StatsdConfigProto.AtomMatcher;
import com.android.internal.os.StatsdConfigProto.FieldMatcher;
import com.android.internal.os.StatsdConfigProto.Predicate;
import com.android.internal.os.StatsdConfigProto.SimplePredicate;
import com.android.os.AtomsProto.AppBreadcrumbReported;
import com.android.os.AtomsProto.Atom;
import com.android.os.StatsLog.DurationBucketInfo;
import com.android.os.StatsLog.StatsLogReport;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.util.RunUtil;

import com.google.common.collect.Range;
import com.google.protobuf.ExtensionRegistry;


public class DurationMetricsTests extends DeviceTestCase {

    private static final int APP_BREADCRUMB_REPORTED_A_MATCH_START_ID = 0;
    private static final int APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID = 1;
    private static final int APP_BREADCRUMB_REPORTED_B_MATCH_START_ID = 2;
    private static final int APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID = 3;

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

    public void testDurationMetric() throws Exception {
        final int label = 1;
        // Add AtomMatchers.
        AtomMatcher startAtomMatcher =
                MetricsUtils.startAtomMatcherWithLabel(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID,
                        label);
        AtomMatcher stopAtomMatcher =
                MetricsUtils.stopAtomMatcherWithLabel(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID,
                        label);

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        builder.addAtomMatcher(startAtomMatcher);
        builder.addAtomMatcher(stopAtomMatcher);

        // Add Predicates.
        SimplePredicate simplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID)
                .build();
        Predicate predicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate"))
                .setSimplePredicate(simplePredicate)
                .build();
        builder.addPredicate(predicate);

        // Add DurationMetric.
        builder.addDurationMetric(StatsdConfigProto.DurationMetric.newBuilder()
                .setId(MetricsUtils.DURATION_METRIC_ID)
                .setWhat(predicate.getId())
                .setAggregationType(StatsdConfigProto.DurationMetric.AggregationType.SUM)
                .setBucket(StatsdConfigProto.TimeUnit.CTS));

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Create AppBreadcrumbReported Start/Stop events.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), label);
        RunUtil.getDefault().sleep(2000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), label);

        // Wait for the metrics to propagate to statsd.
        RunUtil.getDefault().sleep(2000);

        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.DURATION_METRIC_ID);
        LogUtil.CLog.d("Received the following data: " + metricReport.toString());
        assertThat(metricReport.hasDurationMetrics()).isTrue();
        StatsLogReport.DurationMetricDataWrapper durationData
                = metricReport.getDurationMetrics();
        assertThat(durationData.getDataCount()).isEqualTo(1);
        assertThat(durationData.getData(0).getBucketInfo(0).getDurationNanos())
                .isIn(Range.open(0L, (long) 1e9));
    }

    public void testDurationMetricWithCondition() throws Exception {
        final int durationLabel = 1;
        final int conditionLabel = 2;

        // Add AtomMatchers.
        AtomMatcher startAtomMatcher = MetricsUtils.startAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_START_ID, durationLabel);
        AtomMatcher stopAtomMatcher = MetricsUtils.stopAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID, durationLabel);
        AtomMatcher conditionStartAtomMatcher = MetricsUtils.startAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_B_MATCH_START_ID, conditionLabel);
        AtomMatcher conditionStopAtomMatcher = MetricsUtils.stopAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID, conditionLabel);

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                        DeviceUtils.STATSD_ATOM_TEST_PKG)
                .addAtomMatcher(startAtomMatcher)
                .addAtomMatcher(stopAtomMatcher)
                .addAtomMatcher(conditionStartAtomMatcher)
                .addAtomMatcher(conditionStopAtomMatcher);

        // Add Predicates.
        SimplePredicate simplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID)
                .build();
        Predicate predicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate"))
                .setSimplePredicate(simplePredicate)
                .build();

        SimplePredicate conditionSimplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID)
                .build();
        Predicate conditionPredicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("ConditionPredicate"))
                .setSimplePredicate(conditionSimplePredicate)
                .build();

        builder.addPredicate(predicate).addPredicate(conditionPredicate);

        // Add DurationMetric.
        builder.addDurationMetric(StatsdConfigProto.DurationMetric.newBuilder()
                .setId(MetricsUtils.DURATION_METRIC_ID)
                .setWhat(predicate.getId())
                .setAggregationType(StatsdConfigProto.DurationMetric.AggregationType.SUM)
                .setBucket(StatsdConfigProto.TimeUnit.CTS)
                .setCondition(conditionPredicate.getId())
        );

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Start uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Set the condition to true.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Start counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Set the condition to false.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Start uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);
        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.DURATION_METRIC_ID);
        LogUtil.CLog.d("Received the following data: " + metricReport.toString());
        assertThat(metricReport.hasDurationMetrics()).isTrue();
        StatsLogReport.DurationMetricDataWrapper durationData
                = metricReport.getDurationMetrics();
        assertThat(durationData.getDataCount()).isEqualTo(1);
        long totalDuration = durationData.getData(0).getBucketInfoList().stream()
                .mapToLong(bucketInfo -> bucketInfo.getDurationNanos())
                .peek(durationNs -> assertThat(durationNs).isIn(Range.openClosed(0L, (long) 1e9)))
                .sum();
        assertThat(totalDuration).isIn(Range.open((long) 2e9, (long) 3e9));
    }

    public void testDurationMetricWithActivation() throws Exception {
        final int activationMatcherId = 5;
        final int activationMatcherLabel = 5;
        final int ttlSec = 5;
        final int durationLabel = 1;

        // Add AtomMatchers.
        AtomMatcher startAtomMatcher = MetricsUtils.startAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_START_ID, durationLabel);
        AtomMatcher stopAtomMatcher = MetricsUtils.stopAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID, durationLabel);
        StatsdConfigProto.AtomMatcher activationMatcher =
                MetricsUtils.appBreadcrumbMatcherWithLabel(activationMatcherId,
                        activationMatcherLabel);

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                        MetricsUtils.DEVICE_SIDE_TEST_PACKAGE)
                .addAtomMatcher(startAtomMatcher)
                .addAtomMatcher(stopAtomMatcher)
                .addAtomMatcher(activationMatcher);

        // Add Predicates.
        SimplePredicate simplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID)
                .build();
        Predicate predicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate"))
                .setSimplePredicate(simplePredicate)
                .build();
        builder.addPredicate(predicate);

        // Add DurationMetric.
        builder.addDurationMetric(StatsdConfigProto.DurationMetric.newBuilder()
                        .setId(MetricsUtils.DURATION_METRIC_ID)
                        .setWhat(predicate.getId())
                        .setAggregationType(StatsdConfigProto.DurationMetric.AggregationType.SUM)
                        .setBucket(StatsdConfigProto.TimeUnit.CTS))
                .addMetricActivation(StatsdConfigProto.MetricActivation.newBuilder()
                        .setMetricId(MetricsUtils.DURATION_METRIC_ID)
                        .addEventActivation(StatsdConfigProto.EventActivation.newBuilder()
                                .setAtomMatcherId(activationMatcherId)
                                .setActivationType(
                                        StatsdConfigProto.ActivationType.ACTIVATE_IMMEDIATELY)
                                .setTtlSeconds(ttlSec)));

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Start uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Activate the metric.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), activationMatcherLabel);
        RunUtil.getDefault().sleep(10);

        // Start counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);
        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.DURATION_METRIC_ID);
        LogUtil.CLog.d("Received the following data: " + metricReport.toString());
        assertThat(metricReport.hasDurationMetrics()).isTrue();
        StatsLogReport.DurationMetricDataWrapper durationData
                = metricReport.getDurationMetrics();
        assertThat(durationData.getDataCount()).isEqualTo(1);
        long totalDuration = durationData.getData(0).getBucketInfoList().stream()
                .mapToLong(bucketInfo -> bucketInfo.getDurationNanos())
                .peek(durationNs -> assertThat(durationNs).isIn(Range.openClosed(0L, (long) 1e9)))
                .sum();
        assertThat(totalDuration).isIn(Range.open((long) 2e9, (long) 3e9));
    }

    public void testDurationMetricWithConditionAndActivation() throws Exception {
        final int durationLabel = 1;
        final int conditionLabel = 2;
        final int activationMatcherId = 5;
        final int activationMatcherLabel = 5;
        final int ttlSec = 5;

        // Add AtomMatchers.
        AtomMatcher startAtomMatcher = MetricsUtils.startAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_START_ID, durationLabel);
        AtomMatcher stopAtomMatcher = MetricsUtils.stopAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID, durationLabel);
        AtomMatcher conditionStartAtomMatcher = MetricsUtils.startAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_B_MATCH_START_ID, conditionLabel);
        AtomMatcher conditionStopAtomMatcher = MetricsUtils.stopAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID, conditionLabel);
        StatsdConfigProto.AtomMatcher activationMatcher =
                MetricsUtils.appBreadcrumbMatcherWithLabel(activationMatcherId,
                        activationMatcherLabel);

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                        MetricsUtils.DEVICE_SIDE_TEST_PACKAGE)
                .addAtomMatcher(startAtomMatcher)
                .addAtomMatcher(stopAtomMatcher)
                .addAtomMatcher(conditionStartAtomMatcher)
                .addAtomMatcher(conditionStopAtomMatcher)
                .addAtomMatcher(activationMatcher);

        // Add Predicates.
        SimplePredicate simplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID)
                .build();
        Predicate predicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate"))
                .setSimplePredicate(simplePredicate)
                .build();
        builder.addPredicate(predicate);

        SimplePredicate conditionSimplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID)
                .build();
        Predicate conditionPredicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("ConditionPredicate"))
                .setSimplePredicate(conditionSimplePredicate)
                .build();
        builder.addPredicate(conditionPredicate);

        // Add DurationMetric.
        builder.addDurationMetric(StatsdConfigProto.DurationMetric.newBuilder()
                        .setId(MetricsUtils.DURATION_METRIC_ID)
                        .setWhat(predicate.getId())
                        .setAggregationType(StatsdConfigProto.DurationMetric.AggregationType.SUM)
                        .setBucket(StatsdConfigProto.TimeUnit.CTS)
                        .setCondition(conditionPredicate.getId()))
                .addMetricActivation(StatsdConfigProto.MetricActivation.newBuilder()
                        .setMetricId(MetricsUtils.DURATION_METRIC_ID)
                        .addEventActivation(StatsdConfigProto.EventActivation.newBuilder()
                                .setAtomMatcherId(activationMatcherId)
                                .setActivationType(
                                        StatsdConfigProto.ActivationType.ACTIVATE_IMMEDIATELY)
                                .setTtlSeconds(ttlSec)));

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Activate the metric.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), activationMatcherLabel);
        RunUtil.getDefault().sleep(10);

        // Set the condition to true.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Start counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Set the condition to false.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Start uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Let the metric deactivate.
        RunUtil.getDefault().sleep(ttlSec * 1000);
        //doAppBreadcrumbReported(99); // TODO: maybe remove?
        //RunUtil.getDefault().sleep(10);

        // Start uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Set condition to true again.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Start uncounted duration
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Activate the metric.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), activationMatcherLabel);
        RunUtil.getDefault().sleep(10);

        // Start counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop counted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Let the metric deactivate.
        RunUtil.getDefault().sleep(ttlSec * 1000);

        // Start uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        RunUtil.getDefault().sleep(2_000);

        // Stop uncounted duration.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), durationLabel);
        RunUtil.getDefault().sleep(10);

        // Wait for the metrics to propagate to statsd.
        RunUtil.getDefault().sleep(2000);

        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        LogUtil.CLog.d("Received the following data: " + metricReport.toString());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.DURATION_METRIC_ID);
        assertThat(metricReport.hasDurationMetrics()).isTrue();
        StatsLogReport.DurationMetricDataWrapper durationData
                = metricReport.getDurationMetrics();
        assertThat(durationData.getDataCount()).isEqualTo(1);
        long totalDuration = durationData.getData(0).getBucketInfoList().stream()
                .mapToLong(bucketInfo -> bucketInfo.getDurationNanos())
                .peek(durationNs -> assertThat(durationNs).isIn(Range.openClosed(0L, (long) 1e9)))
                .sum();
        assertThat(totalDuration).isIn(Range.open((long) 4e9, (long) 5e9));
    }

    public void testDurationMetricWithDimension() throws Exception {
        // Add AtomMatchers.
        AtomMatcher startAtomMatcherA =
                MetricsUtils.startAtomMatcher(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID);
        AtomMatcher stopAtomMatcherA =
                MetricsUtils.stopAtomMatcher(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID);
        AtomMatcher startAtomMatcherB =
                MetricsUtils.startAtomMatcher(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID);
        AtomMatcher stopAtomMatcherB =
                MetricsUtils.stopAtomMatcher(APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID);

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        builder.addAtomMatcher(startAtomMatcherA);
        builder.addAtomMatcher(stopAtomMatcherA);
        builder.addAtomMatcher(startAtomMatcherB);
        builder.addAtomMatcher(stopAtomMatcherB);

        // Add Predicates.
        SimplePredicate simplePredicateA = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID)
                .build();
        Predicate predicateA = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate_A"))
                .setSimplePredicate(simplePredicateA)
                .build();
        builder.addPredicate(predicateA);

        FieldMatcher.Builder dimensionsBuilder = FieldMatcher.newBuilder()
                .setField(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER);
        dimensionsBuilder
                .addChild(FieldMatcher.newBuilder().setField(
                        AppBreadcrumbReported.LABEL_FIELD_NUMBER));
        Predicate predicateB = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate_B"))
                .setSimplePredicate(SimplePredicate.newBuilder()
                        .setStart(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)
                        .setStop(APP_BREADCRUMB_REPORTED_B_MATCH_STOP_ID)
                        .setDimensions(dimensionsBuilder.build())
                        .build())
                .build();
        builder.addPredicate(predicateB);

        // Add DurationMetric.
        builder.addDurationMetric(StatsdConfigProto.DurationMetric.newBuilder()
                .setId(MetricsUtils.DURATION_METRIC_ID)
                .setWhat(predicateB.getId())
                .setCondition(predicateA.getId())
                .setAggregationType(StatsdConfigProto.DurationMetric.AggregationType.SUM)
                .setBucket(StatsdConfigProto.TimeUnit.CTS)
                .setDimensionsInWhat(
                        FieldMatcher.newBuilder()
                                .setField(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                                .addChild(FieldMatcher.newBuilder().setField(
                                        AppBreadcrumbReported.LABEL_FIELD_NUMBER))));

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Trigger events.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(2000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 2);
        RunUtil.getDefault().sleep(2000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        RunUtil.getDefault().sleep(2000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 2);

        // Wait for the metrics to propagate to statsd.
        RunUtil.getDefault().sleep(2000);

        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.DURATION_METRIC_ID);
        assertThat(metricReport.hasDurationMetrics()).isTrue();
        StatsLogReport.DurationMetricDataWrapper durationData
                = metricReport.getDurationMetrics();
        assertThat(durationData.getDataCount()).isEqualTo(2);
        assertThat(durationData.getData(0).getBucketInfoCount()).isGreaterThan(3);
        assertThat(durationData.getData(1).getBucketInfoCount()).isGreaterThan(3);
        long totalDuration = 0;
        for (DurationBucketInfo bucketInfo : durationData.getData(0).getBucketInfoList()) {
            assertThat(bucketInfo.getDurationNanos()).isIn(Range.openClosed(0L, (long) 1e9));
            totalDuration += bucketInfo.getDurationNanos();
        }
        // Duration for both labels is expected to be 4s.
        assertThat(totalDuration).isIn(Range.open((long) 3e9, (long) 8e9));
        totalDuration = 0;
        for (DurationBucketInfo bucketInfo : durationData.getData(1).getBucketInfoList()) {
            assertThat(bucketInfo.getDurationNanos()).isIn(Range.openClosed(0L, (long) 1e9));
            totalDuration += bucketInfo.getDurationNanos();
        }
        assertThat(totalDuration).isIn(Range.open((long) 3e9, (long) 8e9));
    }
}
