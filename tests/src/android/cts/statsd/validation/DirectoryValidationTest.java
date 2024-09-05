package android.cts.statsd.validation;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.DeviceUtils;

import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.RunUtil;

/**
 * Tests Suite for directories used by Statsd.
 */
public class DirectoryValidationTest extends DeviceTestCase implements IBuildReceiver {

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

    public void testStatsActiveMetricDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsActiveMetricDirectoryExists");
    }

    public void testStatsDataDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsDataDirectoryExists");
    }

    public void testStatsMetadataDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsMetadataDirectoryExists");
    }

    public void testStatsServiceDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testStatsServiceDirectoryExists");
    }

    public void testTrainInfoDirectoryExists() throws Exception {
        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".DirectoryTests", "testTrainInfoDirectoryExists");
    }
}
