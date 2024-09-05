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
import android.cts.statsdatom.lib.ReportUtils;

import com.android.internal.os.StatsdConfigProto;
import com.android.internal.os.StatsdConfigProto.ActivationType;
import com.android.internal.os.StatsdConfigProto.AtomMatcher;
import com.android.internal.os.StatsdConfigProto.EventActivation;
import com.android.internal.os.StatsdConfigProto.FieldFilter;
import com.android.internal.os.StatsdConfigProto.FieldMatcher;
import com.android.internal.os.StatsdConfigProto.GaugeMetric;
import com.android.internal.os.StatsdConfigProto.MetricActivation;
import com.android.internal.os.StatsdConfigProto.Predicate;
import com.android.internal.os.StatsdConfigProto.SimpleAtomMatcher;
import com.android.internal.os.StatsdConfigProto.SimplePredicate;
import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.internal.os.StatsdConfigProto.TimeUnit;
import com.android.os.AtomsProto.AppBreadcrumbReported;
import com.android.os.AtomsProto.Atom;
import com.android.os.StatsLog;
import com.android.os.StatsLog.GaugeBucketInfo;
import com.android.os.StatsLog.GaugeMetricData;
import com.android.os.StatsLog.StatsLogReport;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.util.Pair;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.ExtensionRegistry;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

public class GaugeMetricsTests extends DeviceTestCase {

    private static final int APP_BREADCRUMB_REPORTED_A_MATCH_START_ID = 0;
    private static final int APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID = 1;
    private static final int APP_BREADCRUMB_REPORTED_B_MATCH_START_ID = 2;

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

    public void testGaugeMetric() throws Exception {
        // Add AtomMatcher's.
        AtomMatcher startAtomMatcher =
                MetricsUtils.startAtomMatcher(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID);
        AtomMatcher stopAtomMatcher =
                MetricsUtils.stopAtomMatcher(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID);
        AtomMatcher atomMatcher =
                MetricsUtils.simpleAtomMatcher(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID);

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        builder.addAtomMatcher(startAtomMatcher);
        builder.addAtomMatcher(stopAtomMatcher);
        builder.addAtomMatcher(atomMatcher);

        // Add Predicate's.
        SimplePredicate simplePredicate = SimplePredicate.newBuilder()
                .setStart(APP_BREADCRUMB_REPORTED_A_MATCH_START_ID)
                .setStop(APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID)
                .build();
        Predicate predicate = Predicate.newBuilder()
                .setId(MetricsUtils.StringToId("Predicate"))
                .setSimplePredicate(simplePredicate)
                .build();
        builder.addPredicate(predicate);

        // Add GaugeMetric.
        FieldMatcher fieldMatcher = FieldMatcher.newBuilder()
                .setField(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)
                .build();
        builder.addGaugeMetric(StatsdConfigProto.GaugeMetric.newBuilder()
                .setId(MetricsUtils.GAUGE_METRIC_ID)
                .setWhat(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)
                .setCondition(predicate.getId())
                .setGaugeFieldsFilter(FieldFilter.newBuilder()
                        .setIncludeAll(false)
                        .setFields(fieldMatcher)
                        .build())
                .setDimensionsInWhat(FieldMatcher.newBuilder()
                        .setField(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)
                        .addChild(FieldMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                .build())
                        .build())
                .setBucket(StatsdConfigProto.TimeUnit.CTS)
                .build());

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Create AppBreadcrumbReported Start/Stop events.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 0);
        RunUtil.getDefault().sleep(10);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(10);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 2);
        RunUtil.getDefault().sleep(2000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 2);
        RunUtil.getDefault().sleep(10);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 0);
        RunUtil.getDefault().sleep(10);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 2);
        RunUtil.getDefault().sleep(10);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(2000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 2);
        RunUtil.getDefault().sleep(10);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);

        // Wait for the metrics to propagate to statsd.
        RunUtil.getDefault().sleep(2000);

        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        LogUtil.CLog.d("Got the following gauge metric data: " + metricReport.toString());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.GAUGE_METRIC_ID);
        assertThat(metricReport.hasGaugeMetrics()).isTrue();
        StatsLogReport.GaugeMetricDataWrapper gaugeData = metricReport.getGaugeMetrics();
        gaugeData = backfillGaugeMetricData(gaugeData);
        assertThat(gaugeData.getDataCount()).isEqualTo(1);

        int bucketCount = gaugeData.getData(0).getBucketInfoCount();
        GaugeMetricData data = gaugeData.getData(0);
        assertThat(bucketCount).isGreaterThan(2);
        MetricsUtils.assertBucketTimePresent(data.getBucketInfo(0));
        assertThat(data.getBucketInfo(0).getAtomCount()).isEqualTo(1);
        assertThat(data.getBucketInfo(0).getAtom(0).getAppBreadcrumbReported().getLabel())
                .isEqualTo(0);
        assertThat(data.getBucketInfo(0).getAtom(0).getAppBreadcrumbReported().getState())
                .isEqualTo(AppBreadcrumbReported.State.START);

        MetricsUtils.assertBucketTimePresent(data.getBucketInfo(1));
        assertThat(data.getBucketInfo(1).getAtomCount()).isEqualTo(1);

        MetricsUtils.assertBucketTimePresent(data.getBucketInfo(bucketCount - 1));
        assertThat(data.getBucketInfo(bucketCount - 1).getAtomCount()).isEqualTo(1);
        assertThat(data.getBucketInfo(bucketCount - 1).getAtom(
                0).getAppBreadcrumbReported().getLabel())
                .isEqualTo(2);
        assertThat(data.getBucketInfo(bucketCount - 1).getAtom(
                0).getAppBreadcrumbReported().getState())
                .isEqualTo(AppBreadcrumbReported.State.STOP);
    }

    public void testPulledGaugeMetricWithActivation() throws Exception {
        // Add AtomMatcher's.
        int activationAtomMatcherId = 1;
        int activationAtomMatcherLabel = 1;

        int systemUptimeMatcherId = 2;
        AtomMatcher activationAtomMatcher = MetricsUtils.appBreadcrumbMatcherWithLabel(
                activationAtomMatcherId, activationAtomMatcherLabel);
        AtomMatcher systemUptimeMatcher = AtomMatcher.newBuilder()
                .setId(systemUptimeMatcherId)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.SYSTEM_UPTIME_FIELD_NUMBER))
                .build();

        StatsdConfigProto.StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        builder.addAtomMatcher(activationAtomMatcher);
        builder.addAtomMatcher(systemUptimeMatcher);

        // Add GaugeMetric.
        builder.addGaugeMetric(StatsdConfigProto.GaugeMetric.newBuilder()
                .setId(MetricsUtils.GAUGE_METRIC_ID)
                .setWhat(systemUptimeMatcherId)
                .setGaugeFieldsFilter(
                        FieldFilter.newBuilder().setIncludeAll(true).build())
                .setBucket(StatsdConfigProto.TimeUnit.CTS)
                .build());

        // Add activation.
        builder.addMetricActivation(MetricActivation.newBuilder()
                .setMetricId(MetricsUtils.GAUGE_METRIC_ID)
                .setActivationType(ActivationType.ACTIVATE_IMMEDIATELY)
                .addEventActivation(EventActivation.newBuilder()
                        .setAtomMatcherId(activationAtomMatcherId)
                        .setTtlSeconds(5)));

        // Upload config.
        ConfigUtils.uploadConfig(getDevice(), builder);

        // Plenty of time to pull, but we should not keep the data since we are not active.
        RunUtil.getDefault().sleep(20_000);

        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        LogUtil.CLog.d("Got the following gauge metric data: " + metricReport.toString());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.GAUGE_METRIC_ID);
        assertThat(metricReport.hasGaugeMetrics()).isFalse();
    }

    public void testPulledGaugeMetricWithConditionAndActivation() throws Exception {
        final int conditionLabel = 2;
        final int activationMatcherId = 5;
        final int activationMatcherLabel = 5;
        final int whatMatcherId = 8;
        final int ttlSec = 5;

        // Add AtomMatchers.
        AtomMatcher conditionStartAtomMatcher = MetricsUtils.startAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_START_ID, conditionLabel);
        AtomMatcher conditionStopAtomMatcher = MetricsUtils.stopAtomMatcherWithLabel(
                APP_BREADCRUMB_REPORTED_A_MATCH_STOP_ID, conditionLabel);
        AtomMatcher activationMatcher = MetricsUtils.startAtomMatcherWithLabel(
                activationMatcherId, activationMatcherLabel);
        AtomMatcher whatMatcher = MetricsUtils.unspecifiedAtomMatcher(whatMatcherId);

        StatsdConfig.Builder builder =
                ConfigUtils.createConfigBuilder(MetricsUtils.DEVICE_SIDE_TEST_PACKAGE)
                        .addAtomMatcher(conditionStartAtomMatcher)
                        .addAtomMatcher(conditionStopAtomMatcher)
                        .addAtomMatcher(whatMatcher)
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

        // Add GaugeMetric.
        builder.addGaugeMetric(GaugeMetric.newBuilder()
                        .setId(MetricsUtils.GAUGE_METRIC_ID)
                        .setWhat(whatMatcher.getId())
                        .setBucket(TimeUnit.ONE_MINUTE)
                        .setCondition(predicate.getId())
                        .setGaugeFieldsFilter(FieldFilter.newBuilder()
                                .setIncludeAll(false)
                                .setFields(FieldMatcher.newBuilder()
                                        .setField(APP_BREADCRUMB_REPORTED_B_MATCH_START_ID)))
                        .setDimensionsInWhat(FieldMatcher.newBuilder().setField(whatMatcherId)))
                .addMetricActivation(MetricActivation.newBuilder()
                        .setMetricId(MetricsUtils.GAUGE_METRIC_ID)
                        .addEventActivation(EventActivation.newBuilder()
                                .setAtomMatcherId(activationMatcherId)
                                .setActivationType(ActivationType.ACTIVATE_IMMEDIATELY)
                                .setTtlSeconds(ttlSec)));

        ConfigUtils.uploadConfig(getDevice(), builder);

        // Activate the metric.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), activationMatcherLabel);
        RunUtil.getDefault().sleep(10);

        // Set the condition to true.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // This value is collected.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 10);
        RunUtil.getDefault().sleep(10);

        // Ignored; value already collected.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 20);
        RunUtil.getDefault().sleep(10);

        // Set the condition to false.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Value not updated because condition is false.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 30);
        RunUtil.getDefault().sleep(10);

        // Let the metric deactivate.
        RunUtil.getDefault().sleep(ttlSec * 1000);

        // Value not collected.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 40);
        RunUtil.getDefault().sleep(10);

        // Condition to true again.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), conditionLabel);
        RunUtil.getDefault().sleep(10);

        // Value not collected.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 50);
        RunUtil.getDefault().sleep(10);

        // Activate the metric.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), activationMatcherLabel);
        RunUtil.getDefault().sleep(10);

        // Value collected.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 60);
        RunUtil.getDefault().sleep(10);

        // Let the metric deactivate.
        RunUtil.getDefault().sleep(ttlSec * 1000);

        // Value not collected.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 70);
        RunUtil.getDefault().sleep(10);

        // Wait for the metrics to propagate to statsd.
        RunUtil.getDefault().sleep(2000);

        StatsLogReport metricReport = ReportUtils.getStatsLogReport(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        LogUtil.CLog.d("Received the following data: " + metricReport.toString());
        assertThat(metricReport.getMetricId()).isEqualTo(MetricsUtils.GAUGE_METRIC_ID);
        assertThat(metricReport.hasGaugeMetrics()).isTrue();
        assertThat(metricReport.getIsActive()).isFalse();

        StatsLogReport.GaugeMetricDataWrapper gaugeData = metricReport.getGaugeMetrics();
        gaugeData = backfillGaugeMetricData(gaugeData);
        assertThat(gaugeData.getDataCount()).isEqualTo(1);
        assertThat(gaugeData.getData(0).getBucketInfoCount()).isEqualTo(2);

        GaugeBucketInfo bucketInfo = gaugeData.getData(0).getBucketInfo(0);
        MetricsUtils.assertBucketTimePresent(bucketInfo);
        assertThat(bucketInfo.getAtomCount()).isEqualTo(1);
        assertThat(bucketInfo.getAtom(0).getAppBreadcrumbReported().getLabel()).isEqualTo(10);

        bucketInfo = gaugeData.getData(0).getBucketInfo(1);
        MetricsUtils.assertBucketTimePresent(bucketInfo);
        assertThat(bucketInfo.getAtomCount()).isEqualTo(1);
        assertThat(bucketInfo.getAtom(0).getAppBreadcrumbReported().getLabel()).isEqualTo(60);
    }

    private StatsLogReport.GaugeMetricDataWrapper backfillGaugeMetricData(
            StatsLogReport.GaugeMetricDataWrapper dataWrapper) {
        StatsLogReport.GaugeMetricDataWrapper.Builder dataWrapperBuilder = dataWrapper.toBuilder();
        List<GaugeMetricData> backfilledMetricData = new ArrayList<>();
        for (GaugeMetricData gaugeMetricData : dataWrapperBuilder.getDataList()) {
            GaugeMetricData.Builder gaugeMetricDataBuilder = gaugeMetricData.toBuilder();
            List<GaugeBucketInfo> backfilledBuckets = new ArrayList<>();
            for (GaugeBucketInfo bucketInfo : gaugeMetricData.getBucketInfoList()) {
                backfilledBuckets.add(backfillGaugeBucket(bucketInfo.toBuilder()));
            }
            gaugeMetricDataBuilder.clearBucketInfo();
            gaugeMetricDataBuilder.addAllBucketInfo(backfilledBuckets);
            backfilledMetricData.add(gaugeMetricDataBuilder.build());
        }
        dataWrapperBuilder.clearData();
        dataWrapperBuilder.addAllData(backfilledMetricData);
        return dataWrapperBuilder.build();
    }

    private GaugeBucketInfo backfillGaugeBucket(GaugeBucketInfo.Builder bucketInfoBuilder) {
        if (bucketInfoBuilder.getAtomCount() != 0) {
            return bucketInfoBuilder.build();
        }
        List<Pair<Atom, Long>> atomTimestampData = new ArrayList<>();
        for (StatsLog.AggregatedAtomInfo atomInfo : bucketInfoBuilder.getAggregatedAtomInfoList()) {
            for (long timestampNs : atomInfo.getElapsedTimestampNanosList()) {
                atomTimestampData.add(Pair.create(atomInfo.getAtom(), timestampNs));
            }
        }
        atomTimestampData.sort(Comparator.comparing(o -> o.second));
        bucketInfoBuilder.clearAggregatedAtomInfo();
        for (Pair<Atom, Long> atomTimestamp : atomTimestampData) {
            bucketInfoBuilder.addAtom(atomTimestamp.first);
            bucketInfoBuilder.addElapsedTimestampNanos(atomTimestamp.second);
        }
        return bucketInfoBuilder.build();
    }
}
