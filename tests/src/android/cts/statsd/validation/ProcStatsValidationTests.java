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
package android.cts.statsd.validation;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsd.atom.ProcStateTestCase;
import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;
import android.service.procstats.AggregatedProcessState;

import com.android.internal.os.StatsdConfigProto.StatsdConfig;
import com.android.os.AtomsProto;
import com.android.os.AtomsProto.Atom;
import com.android.os.AtomsProto.ProcessStateAggregated;
import com.android.os.AtomsProto.ProcessStatsProto;
import com.android.os.AtomsProto.ProcessStatsStateProto;
import com.android.tradefed.device.ITestDevice;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.util.RunUtil;

import java.util.List;

/**
 * Side-by-side comparison between statsd and procstats.
 */
public class ProcStatsValidationTests extends ProcStateTestCase {

    private static final String TAG = "Statsd.ProcStatsValidationTests";

    private static final String sBackgroundServiceName = "StatsdCtsBackgroundService";

    private static final int EXTRA_WAIT_TIME_MS = 1_000; // as buffer when proc state changing.

    private static final String DUMP_PROCSTATS_CMD = "dumpsys procstats";

    private void toggleScreenAndSleep(final long duration) throws Exception {
        final long half = duration >> 1;
        RunUtil.getDefault().sleep(half);
        DeviceUtils.turnScreenOff(getDevice());
        RunUtil.getDefault().sleep(half);
        DeviceUtils.turnScreenOn(getDevice());
    }

    public void testProcessStateByPulling() throws Exception {
        startProcStatsTesting();
        clearProcStats();
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);

        // foreground service
        executeForegroundService();
        RunUtil.getDefault().sleep(SLEEP_OF_FOREGROUND_SERVICE + EXTRA_WAIT_TIME_MS);
        // background
        DeviceUtils.executeBackgroundService(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                sBackgroundServiceName, ACTION_BACKGROUND_SLEEP);
        RunUtil.getDefault().sleep(SLEEP_OF_ACTION_BACKGROUND_SLEEP + EXTRA_WAIT_TIME_MS);
        // top
        executeForegroundActivity(ACTION_SLEEP_WHILE_TOP);
        RunUtil.getDefault().sleep(SLEEP_OF_ACTION_SLEEP_WHILE_TOP + EXTRA_WAIT_TIME_MS);
        // Start extremely short-lived activity, so app goes into cache state (#1 - #3 above).
        DeviceUtils.executeBackgroundService(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                sBackgroundServiceName, ACTION_END_IMMEDIATELY);
        final int cacheTime = 2_000; // process should be in cached state for up to this long
        RunUtil.getDefault().sleep(cacheTime);
        // foreground
        // overlay should take 2 sec to appear. So this makes it 4 sec in TOP
        executeForegroundActivity(ACTION_SHOW_APPLICATION_OVERLAY);
        RunUtil.getDefault().sleep(EXTRA_WAIT_TIME_MS + 5_000);

        RunUtil.getDefault().sleep(60_000);
        DeviceUtils.uninstallTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        stopProcStatsTesting();
        commitProcStatsToDisk();
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);

        final String fileName = "PROCSTATSQ_PULL.pbtxt";
        StatsdConfig config = ValidationTestUtil.getConfig(fileName, mCtsBuild);
        LogUtil.CLog.d("Updating the following config:\n" + config.toString());
        ConfigUtils.uploadConfig(getDevice(), config.toBuilder());
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AtomsProto.AppBreadcrumbReported.State.START.getNumber(), 1);
        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT + 5_000);

        List<Atom> statsdData = ReportUtils.getGaugeMetricAtoms(getDevice());

        List<android.service.procstats.ProcessStatsProto> processStatsProtoList
                = getAllProcStatsProtoForStatsd();

        // We pull directly from ProcessStatsService, so not necessary to compare every field.
        // Make sure that 1. both capture statsd package 2. spot check some values are reasonable
        LogUtil.CLog.d("======================");

        String statsdPkgName = "com.android.server.cts.device.statsd";
        long rssAvgStatsd = 0;
        for (Atom d : statsdData) {
            for (ProcessStatsProto proc :
                    d.getProcStats().getProcStatsSection().getProcessStatsList()) {
                if (proc.getProcess().equals(statsdPkgName)) {
                    LogUtil.CLog.d("Got proto from statsd:");
                    LogUtil.CLog.d(proc.toString());
                    for (ProcessStatsStateProto state : proc.getStatesList()) {
                        if (state.getProcessStateAggregated()
                                == ProcessStateAggregated.PROCESS_STATE_IMPORTANT_FOREGROUND) {
                            rssAvgStatsd = state.getRss().getMeanKb();
                        }
                    }
                }
            }
        }

        long rssAvgProcstats = 0;
        for (android.service.procstats.ProcessStatsProto process : processStatsProtoList) {
            if (process.getProcess().equals(statsdPkgName)) {
                LogUtil.CLog.d("Got proto from procstats dumpsys:");
                LogUtil.CLog.d(process.toString());
                for (android.service.procstats.ProcessStatsStateProto state
                        : process.getStatesList()) {
                    if (AggregatedProcessState.AGGREGATED_PROCESS_STATE_IMPORTANT_FOREGROUND
                            == state.getProcessStateAggregated()) {
                        rssAvgProcstats = state.getRss().getMeanKb();
                        break;
                    }
                }
            }
        }

        assertThat(rssAvgStatsd).isEqualTo(rssAvgProcstats);
    }

    public void testProcStatsPkgProcStats() throws Exception {
        /**
         * Temporarily disable this test as the proc stats data being pulled into the statsd
         * doesn't include the pkg part now.
         *
         startProcStatsTesting();
         clearProcStats();
         RunUtil.getDefault().sleep(WAIT_TIME_SHORT);

         // foreground service
         executeForegroundService();
         RunUtil.getDefault().sleep(SLEEP_OF_FOREGROUND_SERVICE + EXTRA_WAIT_TIME_MS);
         // background
         executeBackgroundService(ACTION_BACKGROUND_SLEEP);
         RunUtil.getDefault().sleep(SLEEP_OF_ACTION_BACKGROUND_SLEEP + EXTRA_WAIT_TIME_MS);
         // top
         executeForegroundActivity(ACTION_SLEEP_WHILE_TOP);
         RunUtil.getDefault().sleep(SLEEP_OF_ACTION_SLEEP_WHILE_TOP + EXTRA_WAIT_TIME_MS);
         // Start extremely short-lived activity, so app goes into cache state (#1 - #3 above).
         executeBackgroundService(ACTION_END_IMMEDIATELY);
         final int cacheTime = 2_000; // process should be in cached state for up to this long
         RunUtil.getDefault().sleep(cacheTime);
         // foreground
         // overlay should take 2 sec to appear. So this makes it 4 sec in TOP
         executeForegroundActivity(ACTION_SHOW_APPLICATION_OVERLAY);
         RunUtil.getDefault().sleep(EXTRA_WAIT_TIME_MS + 5_000);

         RunUtil.getDefault().sleep(60_000);
         uninstallPackage();
         stopProcStatsTesting();
         commitProcStatsToDisk();
         RunUtil.getDefault().sleep(WAIT_TIME_SHORT);

         final String fileName = "PROCSTATSQ_PULL_PKG_PROC.pbtxt";
         StatsdConfig config = createValidationUtil().getConfig(fileName);
         LogUtil.CLog.d("Updating the following config:\n" + config.toString());
         uploadConfig(config);
         RunUtil.getDefault().sleep(WAIT_TIME_SHORT);
         setAppBreadcrumbPredicate();
         RunUtil.getDefault().sleep(WAIT_TIME_SHORT);

         List<Atom> statsdData = getGaugeMetricDataList();
         assertThat(statsdData).isNotEmpty();
         assertThat(
         statsdData.get(0).getProcStatsPkgProc().getProcStatsSection()
         .getProcessStatsList()
         ).isNotEmpty();

         // We pull directly from ProcessStatsService, so not necessary to compare every field.
         // Make sure that 1. both capture statsd package 2. spot check some values are reasonable
         LogUtil.CLog.d("======================");

         String statsdPkgName = "com.android.server.cts.device.statsd";
         long rssAvgStatsd = 0;
         long durationStatsd = 0;
         for (Atom d : statsdData) {
         for (ProcessStatsPackageProto pkg : d.getProcStatsPkgProc().getProcStatsSection()
         .getPackageStatsList()) {
         if (pkg.getPackage().equals(statsdPkgName)) {
         LogUtil.CLog.d("Got proto from statsd:");
         LogUtil.CLog.d(pkg.toString());
         for (ProcessStatsProto process : pkg.getProcessStatsList()) {
         for (ProcessStatsStateProto state : process.getStatesList()) {
         if (state.getProcessState()
         == ProcessState.PROCESS_STATE_IMPORTANT_FOREGROUND) {
         durationStatsd = state.getDurationMillis();
         rssAvgStatsd = state.getRss().getAverage();
         }
         }
         }
         }
         assertThat(pkg.getServiceStatsCount()).isEqualTo(0L);
         assertThat(pkg.getAssociationStatsCount()).isEqualTo(0L);
         }
         }

         LogUtil.CLog.d("avg rss from statsd is " + rssAvgStatsd);

         List<ProcessStatsPackageProto> processStatsPackageProtoList = getAllProcStatsProto();

         long pssAvgProcstats = 0;
         long ussAvgProcstats = 0;
         long rssAvgProcstats = 0;
         long durationProcstats = 0;
         int serviceStatsCount = 0;
         int associationStatsCount = 0;
         for (ProcessStatsPackageProto pkg : processStatsPackageProtoList) {
         if (pkg.getPackage().equals(statsdPkgName)) {
         LogUtil.CLog.d("Got proto from procstats dumpsys:");
         LogUtil.CLog.d(pkg.toString());
         for (ProcessStatsProto process : pkg.getProcessStatsList()) {
         for (ProcessStatsStateProto state : process.getStatesList()) {
         if (state.getProcessState()
         == ProcessState.PROCESS_STATE_IMPORTANT_FOREGROUND) {
         durationProcstats = state.getDurationMillis();
         pssAvgProcstats = state.getPss().getAverage();
         ussAvgProcstats = state.getUss().getAverage();
         rssAvgProcstats = state.getRss().getAverage();
         }
         }
         }
         }
         serviceStatsCount += pkg.getServiceStatsCount();
         associationStatsCount += pkg.getAssociationStatsCount();
         }
         assertThat(serviceStatsCount).isGreaterThan(0);
         assertThat(associationStatsCount).isGreaterThan(0);

         LogUtil.CLog.d("avg pss from procstats is " + pssAvgProcstats);
         assertThat(rssAvgStatsd).isEqualTo(rssAvgProcstats);
         */
    }

    private boolean isPssProfilingDisabled() throws Exception {
        ITestDevice device = getDevice();
        final String disablePssProfilingKey = "disable_app_profiler_pss_profiling";
        final String stringToCompare = " " + disablePssProfilingKey + "=true";

        final String dumpsys = device.executeShellCommand("dumpsys activity settings");
        return (dumpsys.contains(stringToCompare));
    }

    protected void clearProcStats() throws Exception {
        getDevice().executeShellCommand("dumpsys procstats --clear");
    }

    private void startProcStatsTesting() throws Exception {
        getDevice().executeShellCommand("dumpsys procstats --start-testing");
    }

    private void stopProcStatsTesting() throws Exception {
        getDevice().executeShellCommand("dumpsys procstats --stop-testing");
    }

    private void commitProcStatsToDisk() throws Exception {
        getDevice().executeShellCommand("dumpsys procstats --commit");
    }

    /*
     * Get all processes' procstats statsd data in proto
     */
    protected List<android.service.procstats.ProcessStatsProto> getAllProcStatsProtoForStatsd()
            throws Exception {
        try {
            android.service.procstats.ProcessStatsSectionProto sectionProto = MetricsUtils.getDump(
                    getDevice(),
                    android.service.procstats.ProcessStatsSectionProto.parser(),
                    String.join(" ", DUMP_PROCSTATS_CMD,
                            "--statsd"));
            List<android.service.procstats.ProcessStatsProto> processStatsProtoList
                    = sectionProto.getProcessStatsList();
            LogUtil.CLog.d("Got procstats:\n ");
            for (android.service.procstats.ProcessStatsProto processStatsProto
                    : processStatsProtoList) {
                LogUtil.CLog.d(processStatsProto.toString());
            }
            return processStatsProtoList;
        } catch (com.google.protobuf.InvalidProtocolBufferException e) {
            LogUtil.CLog.e("Failed to dump procstats proto");
            throw (e);
        }
    }
}
