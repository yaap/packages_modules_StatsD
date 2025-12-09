package android.cts.statsd.validation;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.platform.test.flag.junit.CheckFlagsRule;
import android.platform.test.flag.junit.host.HostFlagsValueProvider;

import com.android.os.statsd.flags.Flags;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.RunUtil;

import android.platform.test.annotations.RequiresFlagsEnabled;

import org.junit.Test;
import org.junit.runner.RunWith;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;

/**
 * Tests Suite for directories used by Statsd.
 */
@RunWith(DeviceJUnit4ClassRunner.class)
public class DirectoryValidationTest extends BaseHostJUnit4Test implements IBuildReceiver {
    @Rule
    public final CheckFlagsRule mCheckFlagsRule =
            HostFlagsValueProvider.createCheckFlagsRule(this::getDevice);

    private IBuildInfo mCtsBuild;

    @Before
    public void setUp() throws Exception {
        assertThat(mCtsBuild).isNotNull();
        DeviceUtils.installTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_APK,
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, mCtsBuild);
        RunUtil.getDefault().sleep(1000);
    }

    @After
    public void tearDown() throws Exception {
        DeviceUtils.uninstallTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE);
    }

    @Override
    public void setBuild(IBuildInfo buildInfo) {
        mCtsBuild = buildInfo;
    }

    @Test
    public void testStatsActiveMetricDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsActiveMetricDirectoryExists");
    }

    @Test
    public void testStatsDataDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsDataDirectoryExists");
    }

    @Test
    public void testStatsMetadataDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsMetadataDirectoryExists");
    }

    @Test
    public void testStatsServiceDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsServiceDirectoryExists");
    }

    @Test
    public void testTrainInfoDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testTrainInfoDirectoryExists");
    }

    @Test
    @RequiresFlagsEnabled(Flags.FLAG_LOGGING_CONTROL_ENABLED)
    public void testStatsAtomsInUseDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsAtomsInUseDirectoryExists");
    }
}
