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
package android.cts.statsd.uidmap;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.compatibility.common.tradefed.build.CompatibilityBuildHelper;
import com.android.internal.os.StatsdConfigProto;
import com.android.os.AtomsProto;
import com.android.os.StatsLog.ConfigMetricsReportList;
import com.android.os.StatsLog.ConfigMetricsReport;
import com.android.os.StatsLog.UidMapping;
import com.android.os.StatsLog.UidMapping.PackageInfoSnapshot;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.ExtensionRegistry;

public class UidMapTests extends DeviceTestCase implements IBuildReceiver {

    private static final long DEVICE_SIDE_TEST_PKG_HASH =
            Long.parseUnsignedLong("15694052924544098582");

    private IBuildInfo mCtsBuild;

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

    // Tests that every report has at least one snapshot.
    public void testUidSnapshotIncluded() throws Exception {
        // There should be at least the test app installed during the test setup.
        ConfigUtils.uploadConfigForPushedAtom(getDevice(),
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                AtomsProto.Atom.UID_PROCESS_STATE_CHANGED_FIELD_NUMBER);

        ConfigMetricsReportList reports = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(reports.getReportsCount()).isGreaterThan(0);

        for (ConfigMetricsReport report : reports.getReportsList()) {
            UidMapping uidmap = report.getUidMap();
            assertThat(uidmap.getSnapshotsCount()).isGreaterThan(0);
            for (PackageInfoSnapshot snapshot : uidmap.getSnapshotsList()) {
                // There must be at least one element in each snapshot (at least one package is
                // installed).
                assertThat(snapshot.getPackageInfoCount()).isGreaterThan(0);
            }
        }
    }

    private boolean hasMatchingChange(UidMapping uidmap, int uid, boolean expectDeletion) {
        LogUtil.CLog.d("The uid we are looking for is " + uid);
        for (UidMapping.Change change : uidmap.getChangesList()) {
            if (change.getAppHash() == DEVICE_SIDE_TEST_PKG_HASH && change.getUid() == uid) {
                if (change.getDeletion() == expectDeletion) {
                    return true;
                }
            }
        }
        return false;
    }

    // Tests that delta event included during app installation.
    public void testChangeFromInstallation() throws Exception {
        getDevice().uninstallPackage(MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        ConfigUtils.uploadConfigForPushedAtom(getDevice(),
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                AtomsProto.Atom.UID_PROCESS_STATE_CHANGED_FIELD_NUMBER);
        // Install the package after the config is sent to statsd. The uid map is not guaranteed to
        // be updated if there's no config in statsd.
        CompatibilityBuildHelper buildHelper = new CompatibilityBuildHelper(mCtsBuild);
        final String result = getDevice().installPackage(
                buildHelper.getTestFile(MetricsUtils.DEVICE_SIDE_TEST_APK), false, true);

        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);

        ConfigMetricsReportList reports = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(reports.getReportsCount()).isGreaterThan(0);

        boolean found = false;
        int uid = DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        for (ConfigMetricsReport report : reports.getReportsList()) {
            LogUtil.CLog.d("Got the following report: \n" + report.toString());
            if (hasMatchingChange(report.getUidMap(), uid, false)) {
                found = true;
            }
        }
        assertThat(found).isTrue();
    }

    // We check that a re-installation gives a change event (similar to an app upgrade).
    public void testChangeFromReinstall() throws Exception {
        CompatibilityBuildHelper buildHelper = new CompatibilityBuildHelper(mCtsBuild);
        getDevice().installPackage(buildHelper.getTestFile(MetricsUtils.DEVICE_SIDE_TEST_APK),
                false, true);
        ConfigUtils.uploadConfigForPushedAtom(getDevice(),
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                AtomsProto.Atom.UID_PROCESS_STATE_CHANGED_FIELD_NUMBER);
        // Now enable re-installation.
        getDevice().installPackage(buildHelper.getTestFile(MetricsUtils.DEVICE_SIDE_TEST_APK), true,
                true);

        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);

        ConfigMetricsReportList reports = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(reports.getReportsCount()).isGreaterThan(0);

        boolean found = false;
        int uid = DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        for (ConfigMetricsReport report : reports.getReportsList()) {
            LogUtil.CLog.d("Got the following report: \n" + report.toString());
            if (hasMatchingChange(report.getUidMap(), uid, false)) {
                found = true;
            }
        }
        assertThat(found).isTrue();
    }

    public void testChangeFromUninstall() throws Exception {
        CompatibilityBuildHelper buildHelper = new CompatibilityBuildHelper(mCtsBuild);
        getDevice().installPackage(buildHelper.getTestFile(MetricsUtils.DEVICE_SIDE_TEST_APK), true,
                true);

        ConfigUtils.uploadConfigForPushedAtom(getDevice(),
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                AtomsProto.Atom.UID_PROCESS_STATE_CHANGED_FIELD_NUMBER);
        int uid = DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        getDevice().uninstallPackage(MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);

        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_LONG);

        ConfigMetricsReportList reports = ReportUtils.getReportList(getDevice(),
                ExtensionRegistry.getEmptyRegistry());
        assertThat(reports.getReportsCount()).isGreaterThan(0);

        boolean found = false;
        for (ConfigMetricsReport report : reports.getReportsList()) {
            LogUtil.CLog.d("Got the following report: \n" + report.toString());
            if (hasMatchingChange(report.getUidMap(), uid, true)) {
                found = true;
            }
        }
        assertThat(found).isTrue();
    }
}
