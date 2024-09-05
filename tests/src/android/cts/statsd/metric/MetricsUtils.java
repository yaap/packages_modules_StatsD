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

import static com.google.common.truth.Truth.assertWithMessage;

import android.cts.statsd.atom.BufferDebug;

import com.android.internal.os.StatsdConfigProto;
import com.android.internal.os.StatsdConfigProto.AtomMatcher;
import com.android.internal.os.StatsdConfigProto.EventActivation;
import com.android.internal.os.StatsdConfigProto.FieldValueMatcher;
import com.android.internal.os.StatsdConfigProto.SimpleAtomMatcher;
import com.android.os.AtomsProto.Atom;
import com.android.os.AtomsProto.AppBreadcrumbReported;
import com.android.tradefed.device.CollectingByteOutputReceiver;
import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.device.ITestDevice;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.util.RunUtil;

import com.google.protobuf.InvalidProtocolBufferException;
import com.google.protobuf.Message;
import com.google.protobuf.Descriptors.Descriptor;
import com.google.protobuf.Descriptors.FieldDescriptor;
import com.google.protobuf.MessageLite;
import com.google.protobuf.Parser;

import java.text.SimpleDateFormat;
import java.util.Date;

public class MetricsUtils {
    public static final String DEVICE_SIDE_TEST_PACKAGE =
            "com.android.server.cts.device.statsd";
    public static final String DEVICE_SIDE_TEST_APK = "CtsStatsdApp.apk";
    public static final long COUNT_METRIC_ID = 3333;
    public static final long DURATION_METRIC_ID = 4444;
    public static final long GAUGE_METRIC_ID = 5555;
    public static final long VALUE_METRIC_ID = 6666;
    public static final long EVENT_METRIC_ID = 7777;

    public static AtomMatcher.Builder getAtomMatcher(int atomId) {
        AtomMatcher.Builder builder = AtomMatcher.newBuilder();
        builder.setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                .setAtomId(atomId));
        return builder;
    }

    public static AtomMatcher startAtomMatcher(int id) {
        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                .setEqInt(AppBreadcrumbReported.State.START.getNumber())))
                .build();
    }

    public static AtomMatcher startAtomMatcherWithLabel(int id, int label) {
        return appBreadcrumbMatcherWithLabelAndState(id, label, AppBreadcrumbReported.State.START);
    }

    public static AtomMatcher stopAtomMatcher(int id) {
        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                .setEqInt(AppBreadcrumbReported.State.STOP.getNumber())))
                .build();
    }

    public static AtomMatcher stopAtomMatcherWithLabel(int id, int label) {
        return appBreadcrumbMatcherWithLabelAndState(id, label, AppBreadcrumbReported.State.STOP);
    }

    public static AtomMatcher unspecifiedAtomMatcher(int id) {
        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                .setEqInt(AppBreadcrumbReported.State.UNSPECIFIED.getNumber())))
                .build();
    }

    public static AtomMatcher simpleAtomMatcher(int id) {
        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER))
                .build();
    }

    public static AtomMatcher appBreadcrumbMatcherWithLabel(int id, int label) {
        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER)
                                .setEqInt(label)))
                .build();
    }

    public static AtomMatcher appBreadcrumbMatcherWithLabelAndState(int id, int label,
            final AppBreadcrumbReported.State state) {

        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.STATE_FIELD_NUMBER)
                                .setEqInt(state.getNumber()))
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER)
                                .setEqInt(label)))
                .build();
    }

    public static AtomMatcher simpleAtomMatcher(int id, int label) {
        return AtomMatcher.newBuilder()
                .setId(id)
                .setSimpleAtomMatcher(SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER)
                        .addFieldValueMatcher(FieldValueMatcher.newBuilder()
                                .setField(AppBreadcrumbReported.LABEL_FIELD_NUMBER)
                                .setEqInt(label)
                        )
                )
                .build();
    }

    public static EventActivation.Builder createEventActivation(int ttlSecs, int matcherId,
            int cancelMatcherId) {
        return EventActivation.newBuilder()
                .setAtomMatcherId(matcherId)
                .setTtlSeconds(ttlSecs)
                .setDeactivationAtomMatcherId(cancelMatcherId);
    }

    public static long StringToId(String str) {
        return str.hashCode();
    }

    public static String getCurrentLogcatDate(ITestDevice device) throws Exception {
        // TODO: Do something more robust than this for getting logcat markers.
        long timestampMs = device.getDeviceDate();
        return new SimpleDateFormat("MM-dd HH:mm:ss.SSS")
                .format(new Date(timestampMs));
    }

    public static void assertBucketTimePresent(Message bucketInfo) {
        Descriptor descriptor = bucketInfo.getDescriptorForType();
        boolean found = false;
        FieldDescriptor bucketNum = descriptor.findFieldByName("bucket_num");
        FieldDescriptor startMillis = descriptor.findFieldByName("start_bucket_elapsed_millis");
        FieldDescriptor endMillis = descriptor.findFieldByName("end_bucket_elapsed_millis");
        if (bucketNum != null && bucketInfo.hasField(bucketNum)) {
            found = true;
        } else if (startMillis != null && bucketInfo.hasField(startMillis) &&
                endMillis != null && bucketInfo.hasField(endMillis)) {
            found = true;
        }
        assertWithMessage(
                "Bucket info did not have either bucket num or start and end elapsed millis"
        ).that(found).isTrue();
    }

    public static boolean didIncidentdFireSince(ITestDevice device, String date) throws Exception {
        final String INCIDENTD_TAG = "incidentd";
        final String INCIDENTD_STARTED_STRING = "reportIncident";
        // TODO: Do something more robust than this in case of delayed logging.
        RunUtil.getDefault().sleep(1000);
        String log = getLogcatSince(device, date, String.format(
                "-s %s -e %s", INCIDENTD_TAG, INCIDENTD_STARTED_STRING));
        return log.contains(INCIDENTD_STARTED_STRING);
    }

    public static String getLogcatSince(ITestDevice device, String date, String logcatParams)
            throws Exception {
        return device.executeShellCommand(String.format(
                "logcat -v threadtime -t '%s' -d %s", date, logcatParams));
    }

    /**
     * Call onto the device with an adb shell command and get the results of
     * that as a proto of the given type.
     *
     * @param parser  A protobuf parser object. e.g. MyProto.parser()
     * @param command The adb shell command to run. e.g. "dumpsys fingerprint --proto"
     * @throws DeviceNotAvailableException    If there was a problem communicating with
     *                                        the test device.
     * @throws InvalidProtocolBufferException If there was an error parsing
     *                                        the proto. Note that a 0 length buffer is not
     *                                        necessarily an error.
     */
    public static <T extends MessageLite> T getDump(ITestDevice device, Parser<T> parser,
            String command)
            throws DeviceNotAvailableException, InvalidProtocolBufferException {
        final CollectingByteOutputReceiver receiver = new CollectingByteOutputReceiver();
        device.executeShellCommand(command, receiver);
        if (false) {
            LogUtil.CLog.d("Command output while parsing " + parser.getClass().getCanonicalName()
                    + " for command: " + command + "\n"
                    + BufferDebug.debugString(receiver.getOutput(), -1));
        }
        try {
            return parser.parseFrom(receiver.getOutput());
        } catch (Exception ex) {
            LogUtil.CLog.d(
                    "Error parsing " + parser.getClass().getCanonicalName() + " for command: "
                            + command
                            + BufferDebug.debugString(receiver.getOutput(), 16384));
            throw ex;
        }
    }
}
