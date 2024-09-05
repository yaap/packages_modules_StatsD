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
package android.cts.statsd.alarm;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.internal.os.StatsdConfigProto.Alarm;
import com.android.internal.os.StatsdConfigProto.IncidentdDetails;
import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.internal.os.StatsdConfigProto.Subscription;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.log.LogUtil.CLog;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.RunUtil;

import java.text.SimpleDateFormat;
import java.util.Date;

/**
 * Statsd Anomaly Detection tests.
 */
public class AlarmTests extends DeviceTestCase implements IBuildReceiver {

    private static final String TAG = "Statsd.AnomalyDetectionTests";

    private static final boolean INCIDENTD_TESTS_ENABLED = false;

    // Config constants
    private static final int ALARM_ID = 11;
    private static final int SUBSCRIPTION_ID_INCIDENTD = 41;
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
            CLog.w(TAG, TAG + " alarm tests are disabled by a flag. Change flag to true to run");
        }
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);
    }

    @Override
    protected void tearDown() throws Exception {
        super.tearDown();
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        DeviceUtils.uninstallTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
    }

    @Override
    public void setBuild(IBuildInfo buildInfo) {
        mCtsBuild = buildInfo;
    }

    public void testAlarm() throws Exception {
        StatsdConfig.Builder config = getBaseConfig();
        DeviceUtils.turnScreenOn(getDevice());
        ConfigUtils.uploadConfig(getDevice(), config);

        String markTime = MetricsUtils.getCurrentLogcatDate(getDevice());
        RunUtil.getDefault().sleep(9_000);

        if (INCIDENTD_TESTS_ENABLED) {
            assertThat(MetricsUtils.didIncidentdFireSince(getDevice(), markTime)).isTrue();
        }
    }


    private final StatsdConfig.Builder getBaseConfig() throws Exception {
        return ConfigUtils.createConfigBuilder(MetricsUtils.DEVICE_SIDE_TEST_PACKAGE)
                .addAlarm(Alarm.newBuilder()
                        .setId(ALARM_ID)
                        .setOffsetMillis(2)
                        .setPeriodMillis(5_000) // every 5 seconds.
                )
                .addSubscription(Subscription.newBuilder()
                        .setId(SUBSCRIPTION_ID_INCIDENTD)
                        .setRuleType(Subscription.RuleType.ALARM)
                        .setRuleId(ALARM_ID)
                        .setIncidentdDetails(IncidentdDetails.newBuilder()
                                .addSection(INCIDENTD_SECTION)));
    }
}
