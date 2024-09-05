/*
 * Copyright (C) 2022 The Android Open Source Project
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

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.DeviceUtils;

import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.RunUtil;

public class StatsFrameworkInitializerTest extends DeviceTestCase implements IBuildReceiver {

    private IBuildInfo mCtsBuild;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        assertThat(mCtsBuild).isNotNull();
        DeviceUtils.installTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_APK,
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, mCtsBuild);
        RunUtil.getDefault().sleep(1000);
    }

    @Override
    protected void tearDown() throws Exception {
        DeviceUtils.uninstallTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
        super.tearDown();
    }

    @Override
    public void setBuild(IBuildInfo buildInfo) {
        mCtsBuild = buildInfo;
    }

    public void testStatsFrameworkInitializer_failsWhenCalledOutsideOfSystemServiceRegistry()
            throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".StatsFrameworkInitializerTests", "testRegisterServiceWrappers_expectFail");
    }

}
