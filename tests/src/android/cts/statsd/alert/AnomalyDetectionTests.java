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
package android.cts.statsd.alert;

import static com.google.common.truth.Truth.assertThat;
import static com.google.common.truth.Truth.assertWithMessage;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.internal.os.StatsdConfigProto;
import com.android.internal.os.StatsdConfigProto.Alert;
import com.android.internal.os.StatsdConfigProto.CountMetric;
import com.android.internal.os.StatsdConfigProto.DurationMetric;
import com.android.internal.os.StatsdConfigProto.FieldFilter;
import com.android.internal.os.StatsdConfigProto.FieldMatcher;
import com.android.internal.os.StatsdConfigProto.GaugeMetric;
import com.android.internal.os.StatsdConfigProto.IncidentdDetails;
import com.android.internal.os.StatsdConfigProto.PerfettoDetails;
import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.internal.os.StatsdConfigProto.Subscription;
import com.android.internal.os.StatsdConfigProto.TimeUnit;
import com.android.internal.os.StatsdConfigProto.ValueMetric;
import com.android.os.AtomsProto.AnomalyDetected;
import com.android.os.AtomsProto.AppBreadcrumbReported;
import com.android.os.AtomsProto.Atom;
import com.android.os.AtomsProto.DebugElapsedClock;
import com.android.os.StatsLog.EventMetricData;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.log.LogUtil.CLog;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.CommandStatus;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.ByteString;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Random;

import perfetto.protos.PerfettoConfig.DataSourceConfig;
import perfetto.protos.PerfettoConfig.FtraceConfig;
import perfetto.protos.PerfettoConfig.TraceConfig;

/**
 * Statsd Anomaly Detection tests.
 */
public class AnomalyDetectionTests extends DeviceTestCase implements IBuildReceiver {

    private static final String TAG = "Statsd.AnomalyDetectionTests";

    private static final boolean INCIDENTD_TESTS_ENABLED = false;
    private static final boolean PERFETTO_TESTS_ENABLED = true;

    private static final int WAIT_AFTER_BREADCRUMB_MS = 2000;

    private static final int SHELL_UID = 2000;

    // Config constants
    private static final int APP_BREADCRUMB_REPORTED_MATCH_START_ID = 1;
    private static final int APP_BREADCRUMB_REPORTED_MATCH_STOP_ID = 2;
    private static final int METRIC_ID = 8;
    private static final int ALERT_ID = 11;
    private static final int SUBSCRIPTION_ID_INCIDENTD = 41;
    private static final int SUBSCRIPTION_ID_PERFETTO = 42;
    private static final int ANOMALY_DETECT_MATCH_ID = 10;
    private static final int ANOMALY_EVENT_ID = 101;
    private static final int INCIDENTD_SECTION = -1;

    private IBuildInfo mCtsBuild;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        assertThat(mCtsBuild).isNotNull();
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        DeviceUtils.installTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_APK,
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, mCtsBuild);
        if (!INCIDENTD_TESTS_ENABLED) {
            CLog.w(TAG, TAG + " anomaly tests are disabled by a flag. Change flag to true to run");
        }

        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);
    }

    @Override
    public void setBuild(IBuildInfo buildInfo) {
        mCtsBuild = buildInfo;
    }

    @Override
    protected void tearDown() throws Exception {
        super.tearDown();
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        DeviceUtils.uninstallTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        if (PERFETTO_TESTS_ENABLED) {
            // Deadline to finish trace collection
            final long deadLine = System.currentTimeMillis() + 10000;
            while (isSystemTracingEnabled()) {
                if (System.currentTimeMillis() > deadLine) {
                    CLog.w("/sys/kernel/debug/tracing/tracing_on is still 1 after 10 secs : "
                            + isSystemTracingEnabled());
                    break;
                }
                CLog.d("Waiting to finish collecting traces. ");
                RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
            }
        }
    }

    // Tests that anomaly detection for count works.
    // Also tests that anomaly detection works when spanning multiple buckets.
    public void testCountAnomalyDetection() throws Exception {
        StatsdConfig.Builder config = getBaseConfig(10, 20, 2 /* threshold: > 2 counts */)
                .addCountMetric(CountMetric.newBuilder()
                        .setId(METRIC_ID)
                        .setWhat(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                        .setBucket(TimeUnit.CTS) // 1 second
                        // Slice by label
                        .setDimensionsInWhat(FieldMatcher.newBuilder()
                                .setField(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                                .addChild(FieldMatcher.newBuilder()
                                        .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER)
                                )
                        )
                );
        ConfigUtils.uploadConfig(getDevice(), config);

        String markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        // count(label=6) -> 1 (not an anomaly, since not "greater than 2")
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 6);
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isFalse();
        }

        // count(label=6) -> 2 (not an anomaly, since not "greater than 2")
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 6);
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isFalse();
        }

        // count(label=12) -> 1 (not an anomaly, since not "greater than 2")
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 12);
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isFalse();
        }

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                6); // count(label=6) -> 3 (anomaly, since "greater than 2"!)
        RunUtil.getDefault().sleep(WAIT_AFTER_BREADCRUMB_MS);

        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertWithMessage("Expected anomaly").that(data).hasSize(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isTrue();
        }
    }

    // Tests that anomaly detection for duration works.
    // Also tests that refractory periods in anomaly detection work.
    public void testDurationAnomalyDetection() throws Exception {
        final int APP_BREADCRUMB_REPORTED_IS_ON_PREDICATE = 1423;
        StatsdConfig.Builder config =
                getBaseConfig(17, 17, 10_000_000_000L  /*threshold: > 10 seconds in nanoseconds*/)
                        .addDurationMetric(DurationMetric.newBuilder()
                                .setId(METRIC_ID)
                                .setWhat(APP_BREADCRUMB_REPORTED_IS_ON_PREDICATE) // predicate below
                                .setAggregationType(DurationMetric.AggregationType.SUM)
                                .setBucket(TimeUnit.CTS) // 1 second
                        )
                        .addPredicate(StatsdConfigProto.Predicate.newBuilder()
                                .setId(APP_BREADCRUMB_REPORTED_IS_ON_PREDICATE)
                                .setSimplePredicate(StatsdConfigProto.SimplePredicate.newBuilder()
                                        .setStart(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                                        .setStop(APP_BREADCRUMB_REPORTED_MATCH_STOP_ID)
                                )
                        );
        ConfigUtils.uploadConfig(getDevice(), config);

        // Since timing is crucial and checking logcat for incidentd is slow, we don't test for it.

        // Test that alarm doesn't fire early.
        String markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(6_000);  // Recorded duration at end: 6s
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        RunUtil.getDefault().sleep(4_000);  // Recorded duration at end: 6s
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();

        // Test that alarm does fire when it is supposed to (after 4s, plus up to 5s alarm delay).
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(9_000);  // Recorded duration at end: 13s
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 2);
        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertWithMessage("Expected anomaly").that(data).hasSize(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);

        // Now test that the refractory period is obeyed.
        markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(3_000);  // Recorded duration at end: 13s
        // NB: the previous getEventMetricDataList also removes the report, so size is back to 0.
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();

        // Test that detection works again after refractory period finishes.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        RunUtil.getDefault().sleep(8_000);  // Recorded duration at end: 9s
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(15_000);  // Recorded duration at end: 15s
        // We can do an incidentd test now that all the timing issues are done.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 2);
        data = ReportUtils.getEventMetricDataList(getDevice());
        assertWithMessage("Expected anomaly").that(data).hasSize(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isTrue();
        }

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
    }

    // Tests that anomaly detection for duration works even when the alarm fires too late.
    public void testDurationAnomalyDetectionForLateAlarms() throws Exception {
        final int APP_BREADCRUMB_REPORTED_IS_ON_PREDICATE = 1423;
        StatsdConfig.Builder config =
                getBaseConfig(50, 0, 6_000_000_000L /* threshold: > 6 seconds in nanoseconds */)
                        .addDurationMetric(DurationMetric.newBuilder()
                                .setId(METRIC_ID)
                                .setWhat(
                                        APP_BREADCRUMB_REPORTED_IS_ON_PREDICATE) // Predicate below.
                                .setAggregationType(DurationMetric.AggregationType.SUM)
                                .setBucket(TimeUnit.CTS) // 1 second
                        )
                        .addPredicate(StatsdConfigProto.Predicate.newBuilder()
                                .setId(APP_BREADCRUMB_REPORTED_IS_ON_PREDICATE)
                                .setSimplePredicate(StatsdConfigProto.SimplePredicate.newBuilder()
                                        .setStart(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                                        .setStop(APP_BREADCRUMB_REPORTED_MATCH_STOP_ID)
                                )
                        );
        ConfigUtils.uploadConfig(getDevice(), config);

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(5_000);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        RunUtil.getDefault().sleep(2_000);
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();

        // Test that alarm does fire when it is supposed to.
        // The anomaly occurs in 1s, but alarms won't fire that quickly.
        // It is likely that the alarm will only fire after this period is already over, but the
        // anomaly should nonetheless be detected when the event stops.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(1_200);
        // Anomaly should be detected here if the alarm didn't fire yet.
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.STOP.getNumber(), 1);
        RunUtil.getDefault().sleep(200);
        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        if (data.size() == 2) {
            // Although we expect that the alarm won't fire, we certainly cannot demand that.
            CLog.w(TAG, "The anomaly was detected twice. Presumably the alarm did manage to fire.");
        }
        assertThat(data.size()).isAnyOf(1, 2);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);
    }

    // Tests that anomaly detection for value works.
    public void testValueAnomalyDetection() throws Exception {
        StatsdConfig.Builder config = getBaseConfig(4, 0, 6 /* threshold: value > 6 */)
                .addValueMetric(ValueMetric.newBuilder()
                        .setId(METRIC_ID)
                        .setWhat(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                        .setBucket(TimeUnit.ONE_MINUTE)
                        // Get the label field's value:
                        .setValueField(FieldMatcher.newBuilder()
                                .setField(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                                .addChild(FieldMatcher.newBuilder()
                                        .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER))
                        )

                );
        ConfigUtils.uploadConfig(getDevice(), config);

        String markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                6); // value = 6, which is NOT > trigger
        RunUtil.getDefault().sleep(WAIT_AFTER_BREADCRUMB_MS);
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isFalse();
        }

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                14); // value = 14 > trigger
        RunUtil.getDefault().sleep(WAIT_AFTER_BREADCRUMB_MS);

        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertWithMessage("Expected anomaly").that(data).hasSize(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isTrue();
        }
    }

    // Test that anomaly detection integrates with perfetto properly.
    public void testPerfetto() throws Exception {
        String chars = getDevice().getProperty("ro.build.characteristics");
        if (chars.contains("watch")) {
            return;
        }

        if (PERFETTO_TESTS_ENABLED) resetPerfettoGuardrails();

        StatsdConfig.Builder config = getBaseConfig(4, 0, 6 /* threshold: value > 6 */)
                .addSubscription(Subscription.newBuilder()
                        .setId(SUBSCRIPTION_ID_PERFETTO)
                        .setRuleType(Subscription.RuleType.ALERT)
                        .setRuleId(ALERT_ID)
                        .setPerfettoDetails(PerfettoDetails.newBuilder()
                                .setTraceConfig(getPerfettoConfig()))
                )
                .addValueMetric(ValueMetric.newBuilder()
                        .setId(METRIC_ID)
                        .setWhat(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                        .setBucket(TimeUnit.ONE_MINUTE)
                        // Get the label field's value:
                        .setValueField(FieldMatcher.newBuilder()
                                .setField(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                                .addChild(FieldMatcher.newBuilder()
                                        .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER))
                        )

                );
        ConfigUtils.uploadConfig(getDevice(), config);

        String markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                6); // value = 6, which is NOT > trigger
        RunUtil.getDefault().sleep(WAIT_AFTER_BREADCRUMB_MS);
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();
        if (PERFETTO_TESTS_ENABLED) assertThat(isSystemTracingEnabled()).isFalse();

        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                14); // value = 14 > trigger
        RunUtil.getDefault().sleep(WAIT_AFTER_BREADCRUMB_MS);

        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertWithMessage("Expected anomaly").that(data).hasSize(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);

        // Pool a few times to allow for statsd <-> traced <-> traced_probes communication to
        // happen.
        if (PERFETTO_TESTS_ENABLED) {
            boolean tracingEnabled = false;
            for (int i = 0; i < 5; i++) {
                if (isSystemTracingEnabled()) {
                    tracingEnabled = true;
                    break;
                }
                RunUtil.getDefault().sleep(1000);
            }
            assertThat(tracingEnabled).isTrue();
        }
    }

    // Tests that anomaly detection for gauge works.
    public void testGaugeAnomalyDetection() throws Exception {
        StatsdConfig.Builder config = getBaseConfig(1, 20, 6 /* threshold: value > 6 */)
                .addGaugeMetric(GaugeMetric.newBuilder()
                        .setId(METRIC_ID)
                        .setWhat(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                        .setBucket(TimeUnit.CTS)
                        // Get the label field's value into the gauge:
                        .setGaugeFieldsFilter(
                                FieldFilter.newBuilder().setFields(FieldMatcher.newBuilder()
                                        .setField(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                                        .addChild(FieldMatcher.newBuilder()
                                                .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER))
                                )
                        )
                );
        ConfigUtils.uploadConfig(getDevice(), config);

        String markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                6); // gauge = 6, which is NOT > trigger
        RunUtil.getDefault().sleep(
                Math.max(WAIT_AFTER_BREADCRUMB_MS, 1_100)); // Must be >1s to push next bucket.
        assertWithMessage("Premature anomaly").that(
                ReportUtils.getEventMetricDataList(getDevice())).isEmpty();
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isFalse();
        }

        // We waited for >1s above, so we are now in the next bucket (which is essential).
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AppBreadcrumbReported.State.START.getNumber(),
                14); // gauge = 14 > trigger
        RunUtil.getDefault().sleep(WAIT_AFTER_BREADCRUMB_MS);

        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertWithMessage("Expected anomaly").that(data).hasSize(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);
        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isTrue();
        }
    }

    // Test that anomaly detection for pulled metrics work.
    public void testPulledAnomalyDetection() throws Exception {
        final int ATOM_ID = Atom.DEBUG_ELAPSED_CLOCK_FIELD_NUMBER; // A pulled atom
        final int VALUE_FIELD =
                DebugElapsedClock.ELAPSED_CLOCK_MILLIS_FIELD_NUMBER; // Something that will be > 0.
        final int ATOM_MATCHER_ID = 300;

        StatsdConfig.Builder config = getBaseConfig(10, 20, 0 /* threshold: value > 0 */)
                .addAllowedLogSource("AID_SYSTEM")
                // Track the ATOM_ID pulled atom
                .addAtomMatcher(StatsdConfigProto.AtomMatcher.newBuilder()
                        .setId(ATOM_MATCHER_ID)
                        .setSimpleAtomMatcher(StatsdConfigProto.SimpleAtomMatcher.newBuilder()
                                .setAtomId(ATOM_ID)))
                .addGaugeMetric(GaugeMetric.newBuilder()
                        .setId(METRIC_ID)
                        .setWhat(ATOM_MATCHER_ID)
                        .setBucket(TimeUnit.CTS)
                        .setSamplingType(GaugeMetric.SamplingType.RANDOM_ONE_SAMPLE)
                        // Track the VALUE_FIELD (anomaly detection requires exactly one field here)
                        .setGaugeFieldsFilter(
                                FieldFilter.newBuilder().setFields(FieldMatcher.newBuilder()
                                        .setField(ATOM_ID)
                                        .addChild(FieldMatcher.newBuilder().setField(VALUE_FIELD))
                                )
                        )
                );
        ConfigUtils.uploadConfig(getDevice(), config);

        RunUtil.getDefault().sleep(
                6_000); // Wait long enough to ensure AlarmManager signals >= 1 pull

        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertThat(data.size()).isEqualTo(1);
        assertThat(data.get(0).getAtom().getAnomalyDetected().getAlertId()).isEqualTo(ALERT_ID);
    }


    private final StatsdConfig.Builder getBaseConfig(int numBuckets,
            int refractorySecs,
            long triggerIfSumGt) throws Exception {
        return ConfigUtils.createConfigBuilder(MetricsUtils.DEVICE_SIDE_TEST_PACKAGE)
                // Items of relevance for detecting the anomaly:
                .addAtomMatcher(StatsdConfigProto.AtomMatcher.newBuilder()
                        .setId(APP_BREADCRUMB_REPORTED_MATCH_START_ID)
                        .setSimpleAtomMatcher(StatsdConfigProto.SimpleAtomMatcher.newBuilder()
                                .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER) // Event
                                // only when the uid is this app's uid.
                                .addFieldValueMatcher(ConfigUtils
                                        .createFvm(AppBreadcrumbReported.UID_FIELD_NUMBER)
                                        .setEqInt(SHELL_UID))
                                .addFieldValueMatcher(ConfigUtils
                                        .createFvm(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                        .setEqInt(AppBreadcrumbReported.State.START.getNumber()))))
                .addAtomMatcher(StatsdConfigProto.AtomMatcher.newBuilder()
                        .setId(APP_BREADCRUMB_REPORTED_MATCH_STOP_ID)
                        .setSimpleAtomMatcher(StatsdConfigProto.SimpleAtomMatcher.newBuilder()
                                .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                                // Event only when the uid is this app's uid.
                                .addFieldValueMatcher(ConfigUtils
                                        .createFvm(AppBreadcrumbReported.UID_FIELD_NUMBER)
                                        .setEqInt(SHELL_UID))
                                .addFieldValueMatcher(ConfigUtils
                                        .createFvm(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                        .setEqInt(AppBreadcrumbReported.State.STOP.getNumber()))))
                .addAlert(Alert.newBuilder()
                        .setId(ALERT_ID)
                        .setMetricId(METRIC_ID) // The metric itself must yet be added by the test.
                        .setNumBuckets(numBuckets)
                        .setRefractoryPeriodSecs(refractorySecs)
                        .setTriggerIfSumGt(triggerIfSumGt))
                .addSubscription(Subscription.newBuilder()
                        .setId(SUBSCRIPTION_ID_INCIDENTD)
                        .setRuleType(Subscription.RuleType.ALERT)
                        .setRuleId(ALERT_ID)
                        .setIncidentdDetails(IncidentdDetails.newBuilder().addSection(
                                INCIDENTD_SECTION)))
                // We want to trigger anomalies on METRIC_ID, but don't want the actual data.
                .addNoReportMetric(METRIC_ID)

                // Items of relevance to reporting the anomaly (we do want this data):
                .addAtomMatcher(StatsdConfigProto.AtomMatcher.newBuilder()
                        .setId(ANOMALY_DETECT_MATCH_ID)
                        .setSimpleAtomMatcher(StatsdConfigProto.SimpleAtomMatcher.newBuilder()
                                .setAtomId(Atom.ANOMALY_DETECTED_FIELD_NUMBER)
                                .addFieldValueMatcher(ConfigUtils
                                        .createFvm(AnomalyDetected.CONFIG_UID_FIELD_NUMBER)
                                        .setEqInt(SHELL_UID))
                                .addFieldValueMatcher(ConfigUtils
                                        .createFvm(AnomalyDetected.CONFIG_ID_FIELD_NUMBER)
                                        .setEqInt(ConfigUtils.CONFIG_ID))))
                .addEventMetric(StatsdConfigProto.EventMetric.newBuilder()
                        .setId(ANOMALY_EVENT_ID)
                        .setWhat(ANOMALY_DETECT_MATCH_ID));
    }

    /**
     * Determines whether perfetto enabled the kernel ftrace tracer.
     */
    protected boolean isSystemTracingEnabled() throws Exception {
        final String traceFsPath = "/sys/kernel/tracing/tracing_on";
        String tracing_on = probe(traceFsPath);
        if (tracing_on.startsWith("0")) return false;
        if (tracing_on.startsWith("1")) return true;

        // fallback to debugfs
        LogUtil.CLog.d("Unexpected state for %s = %s. Falling back to debugfs", traceFsPath,
                tracing_on);

        final String debugFsPath = "/sys/kernel/debug/tracing/tracing_on";
        tracing_on = probe(debugFsPath);
        if (tracing_on.startsWith("0")) return false;
        if (tracing_on.startsWith("1")) return true;
        throw new Exception(String.format("Unexpected state for %s = %s", traceFsPath, tracing_on));
    }

    private String probe(String path) throws Exception {
        return getDevice().executeShellCommand("if [ -e " + path + " ] ; then"
                + " cat " + path + " ; else echo -1 ; fi");
    }

    /**
     * Resets the state of the Perfetto guardrails. This avoids that the test fails if it's
     * run too close of for too many times and hits the upload limit.
     */
    private void resetPerfettoGuardrails() throws Exception {
        final String cmd = "perfetto --reset-guardrails";
        CommandResult cr = getDevice().executeShellV2Command(cmd);
        if (cr.getStatus() != CommandStatus.SUCCESS) {
            throw new Exception(
                    String.format("Error while executing %s: %s %s", cmd, cr.getStdout(),
                            cr.getStderr()));
        }
    }

    /**
     * Returns a protobuf-encoded perfetto config that enables the kernel
     * ftrace tracer with sched_switch for 10 seconds.
     */
    private ByteString getPerfettoConfig() {
        TraceConfig.Builder builder = TraceConfig.newBuilder();

        TraceConfig.BufferConfig buffer = TraceConfig.BufferConfig
                .newBuilder()
                .setSizeKb(128)
                .build();
        builder.addBuffers(buffer);

        FtraceConfig ftraceConfig = FtraceConfig.newBuilder()
                .addFtraceEvents("sched/sched_switch")
                .build();
        DataSourceConfig dataSourceConfig = DataSourceConfig.newBuilder()
                .setName("linux.ftrace")
                .setTargetBuffer(0)
                .setFtraceConfig(ftraceConfig)
                .build();
        TraceConfig.DataSource dataSource = TraceConfig.DataSource
                .newBuilder()
                .setConfig(dataSourceConfig)
                .build();
        builder.addDataSources(dataSource);

        builder.setDurationMs(10000);
        builder.setAllowUserBuildTracing(true);

        TraceConfig.IncidentReportConfig incident = TraceConfig.IncidentReportConfig
                .newBuilder()
                .setSkipIncidentd(true)
                .build();
        builder.setIncidentReportConfig(incident);

        // To avoid being hit with guardrails firing in multiple test runs back
        // to back, we set a unique session key for each config.
        Random random = new Random();
        StringBuilder sessionNameBuilder = new StringBuilder("statsd-cts-");
        sessionNameBuilder.append(random.nextInt() & Integer.MAX_VALUE);
        builder.setUniqueSessionName(sessionNameBuilder.toString());

        return builder.build().toByteString();
    }
}
