/*
 * Copyright (C) 2024 The Android Open Source Project
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

package android.cts.statsd.atom;

import static com.google.common.truth.Truth.assertThat;

import android.cts.statsd.metric.MetricsUtils;
import android.cts.statsdatom.lib.AtomTestUtils;
import android.cts.statsdatom.lib.ConfigUtils;
import android.cts.statsdatom.lib.DeviceUtils;
import android.cts.statsdatom.lib.ReportUtils;

import com.android.os.AtomsProto.Atom;
import com.android.os.AttributionNode;
import com.android.os.StatsLog.EventMetricData;
import com.android.os.AtomsProto.TestAtomReported;
import com.android.os.statsd.StatsdExtensionAtoms;
import com.android.os.statsd.StatsdExtensionAtoms.TestExtensionAtomReported;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.log.LogUtil.CLog;
import com.android.tradefed.build.IBuildInfo;
import com.android.tradefed.testtype.DeviceTestCase;
import com.android.tradefed.testtype.IBuildReceiver;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.ExtensionRegistry;

import java.util.List;
import java.util.concurrent.Flow.Subscription;

public class AtomParsingTests extends DeviceTestCase implements IBuildReceiver {

    private IBuildInfo mCtsBuild;

    @Override
    protected void setUp() throws Exception {
        super.setUp();
        assertThat(mCtsBuild).isNotNull();
        ConfigUtils.removeConfig(getDevice());
        ReportUtils.clearReports(getDevice());
        DeviceUtils.installTestApp(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_APK,
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, mCtsBuild);
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

    public void testWriteExtensionTestAtom() throws Exception {
        final int atomTag = StatsdExtensionAtoms.TEST_EXTENSION_ATOM_REPORTED_FIELD_NUMBER;
        ConfigUtils.uploadConfigForPushedAtomWithUid(getDevice(),
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, atomTag, /*useUidAttributionChain=*/true);

        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".AtomTests", "testWriteExtensionTestAtom");

        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        // Sorted list of events in order in which they occurred.

        ExtensionRegistry registry = ExtensionRegistry.newInstance();
        StatsdExtensionAtoms.registerAllExtensions(registry);

        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice(), registry);
        assertThat(data).hasSize(4);

        TestExtensionAtomReported atom = data.get(0).getAtom().getExtension(
                StatsdExtensionAtoms.testExtensionAtomReported);
        List<AttributionNode> attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(2);
        assertThat(attrChain.get(0).getUid()).isEqualTo(1234);
        assertThat(attrChain.get(0).getTag()).isEqualTo("tag1");
        assertThat(attrChain.get(1).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(1).getTag()).isEqualTo("tag2");

        assertThat(atom.getIntField()).isEqualTo(42);
        assertThat(atom.getLongField()).isEqualTo(Long.MAX_VALUE);
        assertThat(atom.getFloatField()).isEqualTo(3.14f);
        assertThat(atom.getStringField()).isEqualTo("This is a basic test!");
        assertThat(atom.getBooleanField()).isFalse();
        assertThat(atom.getState().getNumber()).isEqualTo(TestExtensionAtomReported.State.ON_VALUE);
        assertThat(atom.getBytesField().getLongFieldList())
                .containsExactly(1L, 2L, 3L).inOrder();

        assertThat(atom.getRepeatedIntFieldList()).containsExactly(3, 6).inOrder();
        assertThat(atom.getRepeatedLongFieldList()).containsExactly(1000L, 1002L).inOrder();
        assertThat(atom.getRepeatedFloatFieldList()).containsExactly(0.3f, 0.09f).inOrder();
        assertThat(atom.getRepeatedStringFieldList()).containsExactly("str1", "str2").inOrder();
        assertThat(atom.getRepeatedBooleanFieldList()).containsExactly(true, false).inOrder();
        assertThat(atom.getRepeatedEnumFieldList())
                .containsExactly(TestExtensionAtomReported.State.OFF,
                    TestExtensionAtomReported.State.ON).inOrder();

        atom = data.get(1).getAtom().getExtension(
            StatsdExtensionAtoms.testExtensionAtomReported);
        attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(2);
        assertThat(attrChain.get(0).getUid()).isEqualTo(9999);
        assertThat(attrChain.get(0).getTag()).isEqualTo("tag9999");
        assertThat(attrChain.get(1).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(1).getTag()).isEmpty();

        assertThat(atom.getIntField()).isEqualTo(100);
        assertThat(atom.getLongField()).isEqualTo(Long.MIN_VALUE);
        assertThat(atom.getFloatField()).isEqualTo(-2.5f);
        assertThat(atom.getStringField()).isEqualTo("Test null uid");
        assertThat(atom.getBooleanField()).isTrue();
        assertThat(atom.getState().getNumber()).isEqualTo(
                TestExtensionAtomReported.State.UNKNOWN_VALUE);
        assertThat(atom.getBytesField().getLongFieldList())
                .containsExactly(1L, 2L, 3L).inOrder();
        assertThat(atom.getRepeatedIntFieldList()).containsExactly(3, 6).inOrder();
        assertThat(atom.getRepeatedLongFieldList()).containsExactly(1000L, 1002L).inOrder();
        assertThat(atom.getRepeatedFloatFieldList()).containsExactly(0.3f, 0.09f).inOrder();
        assertThat(atom.getRepeatedStringFieldList()).containsExactly("str1", "str2").inOrder();
        assertThat(atom.getRepeatedBooleanFieldList()).containsExactly(true, false).inOrder();
        assertThat(atom.getRepeatedEnumFieldList())
                .containsExactly(TestExtensionAtomReported.State.OFF,
                        TestExtensionAtomReported.State.ON)
                .inOrder();

        atom = data.get(2).getAtom().getExtension(
            StatsdExtensionAtoms.testExtensionAtomReported);
        attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(1);
        assertThat(attrChain.get(0).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(0).getTag()).isEqualTo("tag1");

        assertThat(atom.getIntField()).isEqualTo(-256);
        assertThat(atom.getLongField()).isEqualTo(-1234567890L);
        assertThat(atom.getFloatField()).isEqualTo(42.01f);
        assertThat(atom.getStringField()).isEqualTo("Test non chained");
        assertThat(atom.getBooleanField()).isTrue();
        assertThat(atom.getState().getNumber()).isEqualTo(
                TestExtensionAtomReported.State.OFF_VALUE);
        assertThat(atom.getBytesField().getLongFieldList())
                .containsExactly(1L, 2L, 3L).inOrder();
        assertThat(atom.getRepeatedIntFieldList()).isEmpty();
        assertThat(atom.getRepeatedLongFieldList()).isEmpty();
        assertThat(atom.getRepeatedFloatFieldList()).isEmpty();
        assertThat(atom.getRepeatedStringFieldList()).isEmpty();
        assertThat(atom.getRepeatedBooleanFieldList()).isEmpty();
        assertThat(atom.getRepeatedEnumFieldList()).isEmpty();

        atom = data.get(3).getAtom().getExtension(
            StatsdExtensionAtoms.testExtensionAtomReported);
        attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(1);
        assertThat(attrChain.get(0).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(0).getTag()).isEmpty();

        assertThat(atom.getIntField()).isEqualTo(0);
        assertThat(atom.getLongField()).isEqualTo(0L);
        assertThat(atom.getFloatField()).isEqualTo(0f);
        assertThat(atom.getStringField()).isEmpty();
        assertThat(atom.getBooleanField()).isTrue();
        assertThat(atom.getState().getNumber()).isEqualTo(
                TestExtensionAtomReported.State.OFF_VALUE);
        assertThat(atom.getBytesField().getLongFieldList()).isEmpty();
        assertThat(atom.getRepeatedIntFieldList()).isEmpty();
        assertThat(atom.getRepeatedLongFieldList()).isEmpty();
        assertThat(atom.getRepeatedFloatFieldList()).isEmpty();
        assertThat(atom.getRepeatedStringFieldList()).isEmpty();
        assertThat(atom.getRepeatedBooleanFieldList()).isEmpty();
        assertThat(atom.getRepeatedEnumFieldList()).isEmpty();
    }

    public void testWriteRawTestAtom() throws Exception {
        final int atomTag = Atom.TEST_ATOM_REPORTED_FIELD_NUMBER;
        ConfigUtils.uploadConfigForPushedAtomWithUid(getDevice(),
                MetricsUtils.DEVICE_SIDE_TEST_PACKAGE, atomTag, /*useUidAttributionChain=*/true);

        DeviceUtils.runDeviceTests(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE,
                ".AtomTests", "testWriteRawTestAtom");

        RunUtil.getDefault().sleep(AtomTestUtils.WAIT_TIME_SHORT);
        // Sorted list of events in order in which they occurred.
        List<EventMetricData> data = ReportUtils.getEventMetricDataList(getDevice());
        assertThat(data).hasSize(4);

        TestAtomReported atom = data.get(0).getAtom().getTestAtomReported();
        List<AttributionNode> attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(2);
        assertThat(attrChain.get(0).getUid()).isEqualTo(1234);
        assertThat(attrChain.get(0).getTag()).isEqualTo("tag1");
        assertThat(attrChain.get(1).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(1).getTag()).isEqualTo("tag2");

        assertThat(atom.getIntField()).isEqualTo(42);
        assertThat(atom.getLongField()).isEqualTo(Long.MAX_VALUE);
        assertThat(atom.getFloatField()).isEqualTo(3.14f);
        assertThat(atom.getStringField()).isEqualTo("This is a basic test!");
        assertThat(atom.getBooleanField()).isFalse();
        assertThat(atom.getState().getNumber()).isEqualTo(TestAtomReported.State.ON_VALUE);
        assertThat(atom.getBytesField().getExperimentIdList())
                .containsExactly(1L, 2L, 3L).inOrder();

        assertThat(atom.getRepeatedIntFieldList()).containsExactly(3, 6).inOrder();
        assertThat(atom.getRepeatedLongFieldList()).containsExactly(1000L, 1002L).inOrder();
        assertThat(atom.getRepeatedFloatFieldList()).containsExactly(0.3f, 0.09f).inOrder();
        assertThat(atom.getRepeatedStringFieldList()).containsExactly("str1", "str2").inOrder();
        assertThat(atom.getRepeatedBooleanFieldList()).containsExactly(true, false).inOrder();
        assertThat(atom.getRepeatedEnumFieldList())
                .containsExactly(TestAtomReported.State.OFF, TestAtomReported.State.ON)
                .inOrder();

        atom = data.get(1).getAtom().getTestAtomReported();
        attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(2);
        assertThat(attrChain.get(0).getUid()).isEqualTo(9999);
        assertThat(attrChain.get(0).getTag()).isEqualTo("tag9999");
        assertThat(attrChain.get(1).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(1).getTag()).isEmpty();

        assertThat(atom.getIntField()).isEqualTo(100);
        assertThat(atom.getLongField()).isEqualTo(Long.MIN_VALUE);
        assertThat(atom.getFloatField()).isEqualTo(-2.5f);
        assertThat(atom.getStringField()).isEqualTo("Test null uid");
        assertThat(atom.getBooleanField()).isTrue();
        assertThat(atom.getState().getNumber()).isEqualTo(TestAtomReported.State.UNKNOWN_VALUE);
        assertThat(atom.getBytesField().getExperimentIdList())
                .containsExactly(1L, 2L, 3L).inOrder();

        assertThat(atom.getRepeatedIntFieldList()).containsExactly(3, 6).inOrder();
        assertThat(atom.getRepeatedLongFieldList()).containsExactly(1000L, 1002L).inOrder();
        assertThat(atom.getRepeatedFloatFieldList()).containsExactly(0.3f, 0.09f).inOrder();
        assertThat(atom.getRepeatedStringFieldList()).containsExactly("str1", "str2").inOrder();
        assertThat(atom.getRepeatedBooleanFieldList()).containsExactly(true, false).inOrder();
        assertThat(atom.getRepeatedEnumFieldList())
                .containsExactly(TestAtomReported.State.OFF, TestAtomReported.State.ON)
                .inOrder();

        atom = data.get(2).getAtom().getTestAtomReported();
        attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(1);
        assertThat(attrChain.get(0).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(0).getTag()).isEqualTo("tag1");

        assertThat(atom.getIntField()).isEqualTo(-256);
        assertThat(atom.getLongField()).isEqualTo(-1234567890L);
        assertThat(atom.getFloatField()).isEqualTo(42.01f);
        assertThat(atom.getStringField()).isEqualTo("Test non chained");
        assertThat(atom.getBooleanField()).isTrue();
        assertThat(atom.getState().getNumber()).isEqualTo(TestAtomReported.State.OFF_VALUE);
        assertThat(atom.getBytesField().getExperimentIdList())
                .containsExactly(1L, 2L, 3L).inOrder();

        assertThat(atom.getRepeatedIntFieldList()).isEmpty();
        assertThat(atom.getRepeatedLongFieldList()).isEmpty();
        assertThat(atom.getRepeatedFloatFieldList()).isEmpty();
        assertThat(atom.getRepeatedStringFieldList()).isEmpty();
        assertThat(atom.getRepeatedBooleanFieldList()).isEmpty();
        assertThat(atom.getRepeatedEnumFieldList()).isEmpty();

        atom = data.get(3).getAtom().getTestAtomReported();
        attrChain = atom.getAttributionNodeList();
        assertThat(attrChain).hasSize(1);
        assertThat(attrChain.get(0).getUid()).isEqualTo(
                DeviceUtils.getAppUid(getDevice(), MetricsUtils.DEVICE_SIDE_TEST_PACKAGE));
        assertThat(attrChain.get(0).getTag()).isEmpty();

        assertThat(atom.getIntField()).isEqualTo(0);
        assertThat(atom.getLongField()).isEqualTo(0L);
        assertThat(atom.getFloatField()).isEqualTo(0f);
        assertThat(atom.getStringField()).isEmpty();
        assertThat(atom.getBooleanField()).isTrue();
        assertThat(atom.getState().getNumber()).isEqualTo(TestAtomReported.State.OFF_VALUE);
        assertThat(atom.getBytesField().getExperimentIdList()).isEmpty();

        assertThat(atom.getRepeatedIntFieldList()).isEmpty();
        assertThat(atom.getRepeatedLongFieldList()).isEmpty();
        assertThat(atom.getRepeatedFloatFieldList()).isEmpty();
        assertThat(atom.getRepeatedStringFieldList()).isEmpty();
        assertThat(atom.getRepeatedBooleanFieldList()).isEmpty();
        assertThat(atom.getRepeatedEnumFieldList()).isEmpty();
    }
}
