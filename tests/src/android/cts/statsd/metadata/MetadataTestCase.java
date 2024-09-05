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

import android.cts.statsd.atom.BufferDebug;
import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.os.AtomsProto.Atom;
import com.android.os.StatsLog.StatsdStatsReport;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.device.CollectingByteOutputReceiver;
import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.MessageLite;
import com.google.protobuf.Parser;

public class MetadataTestCase extends DeviceTestCase implements IBuildReceiver {
    public static final String DUMP_METADATA_CMD = "cmd stats print-stats";

    protected IBuildInfo mCtsBuild;

    protected StatsdStatsReport getStatsdStatsReport() throws Exception {
        try {
            StatsdStatsReport report = MetricsUtils.getDump(getDevice(), StatsdStatsReport.parser(),
                    String.join(" ", DUMP_METADATA_CMD, "--proto"));
            return report;
        } catch (com.google.protobuf.InvalidProtocolBufferException e) {
            LogUtil.CLog.e("Failed to fetch and parse the statsdstats output report.");
            throw (e);
        }
    }

    protected final StatsdConfig.Builder getBaseConfig() throws Exception {
        StatsdConfig.Builder builder = ConfigUtils.createConfigBuilder(
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        ConfigUtils.addEventMetric(builder, Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER);
        return builder;
    }

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        assertThat(mCtsBuild).isNotNull();
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        DeviceUtils.installTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_APK,
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, mCtsBuild);
        RunUtil.getDefault().sleep(1000);
    }

    @Override
    protected void tearDown() throws Exception {
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        DeviceUtils.uninstallTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        super.tearDown();
    }

    @Override
    public void setBuild(IBuildInfo buildInfo) {
        mCtsBuild = buildInfo;
    }
}
