/*
 * Copyright (C) 2019 The Android Open Source Project
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
package android.cts.statsd.subscriber;

import static com.google.common.truth.Truth.assertThat;

import com.android.compatibility.common.util.CpuFeatures;
import com.android.internal.os.StatsdConfigProto;
import com.android.os.AtomsProto;
import com.android.os.AtomsProto.Atom;
import com.android.os.ShellConfig;
import com.android.os.statsd.ShellDataProto;
import com.android.tradefed.device.CollectingByteOutputReceiver;
import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.device.ITestDevice;
import com.android.tradefed.log.LogUtil;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.RunUtil;

import com.google.common.io.Files;
import com.google.common.truth.Expect;
import com.google.protobuf.InvalidProtocolBufferException;

import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import android.cts.statsdatom.lib.AtomTestUtils;

/**
 * Statsd shell data subscription test.
 */
@RunWith(DeviceJUnit4ClassRunner.class)
public class ShellSubscriberTest extends BaseHostJUnit4Test {
    private int sizetBytes;
    private ExecutorService mExecutor;

    @Rule
    public final Expect expect = Expect.create();

    @Before
    public void setUp() throws Exception {
        sizetBytes = getSizetBytes();
        mExecutor = Executors.newCachedThreadPool();
    }

    @After
    public void tearDown() throws Exception {
        if (mExecutor != null) {
            mExecutor.shutdownNow();
        }
    }

    private static class CountdownReceiver extends CollectingByteOutputReceiver {
        private final CountDownLatch mLatch;
        private final int mSizetBytes;
        private int mDataCount;

        CountdownReceiver(CountDownLatch latch, int dataCount, int sizetBytes) {
            mLatch = latch;
            mDataCount = dataCount;
            mSizetBytes = sizetBytes;
        }

        @Override
        public synchronized void addOutput(byte[] data, int offset, int length) {
            super.addOutput(data, offset, length);
            if (mDataCount > 0 && length > mSizetBytes) {
                mDataCount--;
                mLatch.countDown();
            }
        }
    }

    @Test
    public void testShellSubscription() throws Exception {
        if (sizetBytes < 0) {
            return;
        }

        CollectingByteOutputReceiver receiver = startSubscription();
        checkOutput(receiver);

        receiver.cancel();
    }

    // This is testShellSubscription but 5x
    @Test
    public void testShellSubscriptionReconnect() throws Exception {
        int numOfSubs = 5;
        if (sizetBytes < 0) {
            return;
        }

        for (int i = 0; i < numOfSubs; i++) {
            CollectingByteOutputReceiver receiver = startSubscription();
            checkOutput(receiver);
            receiver.cancel();
        }
    }

    // Tests that multiple clients can run at once:
    // -Runs maximum number of active subscriptions (20) at once.
    // -Maximum number of subscriptions minus 1 return:
    // --Leave 1 subscription alive to ensure the subscriber helper thread stays alive.
    // -Run maximum number of subscriptions minus 1 to reach the maximum running again.
    // -Attempt to run one more subscription, which will fail.
    @Test
    public void testShellMaxSubscriptions() throws Exception {
        // Maximum number of active subscriptions, set in ShellSubscriber.h
        final int maxSubs = 20;
        if (sizetBytes < 0) {
            return;
        }
        CollectingByteOutputReceiver[] receivers = new CollectingByteOutputReceiver[maxSubs + 1];
        Future<Boolean>[] futures = new Future[maxSubs + 1];
        ShellConfig.ShellSubscription config = createConfig();
        byte[] validConfig = makeValidConfig(config);

        // Push the shell config file to the device
        String remotePath = pushShellConfigToDevice(validConfig);

        String cmd = "cat " + remotePath + " |  cmd stats data-subscribe";

        // First subscription will receive 2 atom events.
        CountDownLatch firstSubLatch = new CountDownLatch(2);

        // The next 19 subscriptions will receive 1 atom event each and use the same latch.
        CountDownLatch latch = new CountDownLatch(maxSubs - 1);

        for (int i = 0; i < maxSubs; i++) {
            // Run data-subscribe on a thread
            if (i == 0) {
                receivers[0] = new CountdownReceiver(firstSubLatch, /*dataCount=*/2, sizetBytes);
            } else {
                receivers[i] = new CountdownReceiver(latch, /*dataCount=*/1, sizetBytes);
            }

            final CollectingByteOutputReceiver receiver = receivers[i];

            futures[i] = mExecutor.submit(() -> {
                // Execute shell command without any timeout.
                getDevice().executeShellCommand(cmd, receiver,
                        /*maxTimeToOutputShellResponse=*/0, /*timeUnit=*/null,
                        /*retryAttempts=*/0);

                // This return value is unused but it forces the Callable interface to be used
                // instead of Runnable. Callable interface has throws declaration in the
                // definition which is what we need to avoid having to handle exceptions here.
                return true;
            });
            LogUtil.CLog.d("Starting new shell subscription.");
        }

        // Sleep 2 seconds to make sure all subscription clients are initialized before
        // first pushed event
        RunUtil.getDefault().sleep(2_000);

        // Pushed event. arbitrary label = 1
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AtomsProto.AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 1);

        // Sleep up to 10 seconds to make sure the event is processed.
        boolean latchResult = latch.await(10, TimeUnit.SECONDS);
        expect.withMessage("latch only counted down to %s", latch.getCount())
            .that(latchResult).isTrue();

        // Validate the outputs of the last 19 subscriptions.
        for (int i = 1; i < maxSubs; i++) {
            boolean result = checkOutput(receivers[i]);
            if (!result) {
                expect.withMessage("checkOutput failed for index %s", i).fail();
            }
        }

        // Terminate last 19 subscriptions. Keep first subscription active.
        for (int i = 1; i < maxSubs; i++) {
            receivers[i].cancel();
        }

        // Allow time for last 19 subscriptions to end.
        RunUtil.getDefault().sleep(10_000);

        // Run 19 more subscriptions to hit the maximum active subscriptions again
        latch = new CountDownLatch(maxSubs - 1);
        for (int i = 1; i < maxSubs; i++) {
            // Run data-subscribe on a thread
            receivers[i] = new CountdownReceiver(latch, /*dataCount=*/1, sizetBytes);
            final CollectingByteOutputReceiver receiver = receivers[i];
            futures[i] = mExecutor.submit(() -> {
                getDevice().executeShellCommand(cmd, receiver,
                        /*maxTimeToOutputShellResponse=*/0, /*timeUnit=*/null,
                        /*retryAttempts=*/0);
                return true;
            });
            LogUtil.CLog.d("Starting new shell subscription.");
        }

        // Sleep 10 seconds to make sure all subscription clients are initialized before
        // pushed event
        RunUtil.getDefault().sleep(2_000);

        // ShellSubscriber only allows 20 subscriptions at a time. This is the 21st which will
        // be ignored
        receivers[maxSubs] = new CollectingByteOutputReceiver();
        final CollectingByteOutputReceiver receiver = receivers[maxSubs];
        futures[maxSubs] = mExecutor.submit(() -> {
            getDevice().executeShellCommand(cmd, receiver,
                    /*maxTimeToOutputShellResponse=*/0, /*timeUnit=*/null,
                    /*retryAttempts=*/0);
            return true;
        });

        // Sleep 1 seconds to ensure that the 21st subscription is rejected
        RunUtil.getDefault().sleep(1_000);

        // Pushed event. arbitrary label = 1
        AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                AtomsProto.AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 1);

        // Wait up to 10 seconds to make sure the event is processed.
        latchResult = latch.await(10, TimeUnit.SECONDS);
        expect.withMessage("latch only counted down to %s", latch.getCount())
            .that(latchResult).isTrue();

        latchResult = firstSubLatch.await(10, TimeUnit.SECONDS);
        expect.withMessage("firstSubLatch only counted down to %s", firstSubLatch.getCount())
            .that(latchResult).isTrue();

        // Validate the outputs of the non-rejected subscriptions.
        for (int i = 0; i < maxSubs; i++) {
            checkOutput(receivers[i]);
            boolean result = checkOutput(receivers[i]);
            if (!result) {
                expect.withMessage("checkOutput failed for index %s", i).fail();
            }
        }

        // Ensure that the 21st subscription got rejected and has an empty output
        byte[] output = receivers[maxSubs].getOutput();
        expect.that(output).isEmpty();

        // Terminate all subscriptions.
        for (int i = 0; i <= maxSubs; i++) {
            receivers[i].cancel();
        }

        // Remove config from device if not already deleted
        getDevice().executeShellCommand("rm " + remotePath);
    }

    private int getSizetBytes() throws Exception {
        ITestDevice device = getDevice();
        if (CpuFeatures.isArm64(device)) {
            return 8;
        }
        if (CpuFeatures.isArm32(device)) {
            return 4;
        }
        return -1;
    }

    private ShellConfig.ShellSubscription createConfig() {
        return ShellConfig.ShellSubscription.newBuilder()
                .addPushed((StatsdConfigProto.SimpleAtomMatcher.newBuilder()
                        .setAtomId(Atom.APP_BREADCRUMB_REPORTED_FIELD_NUMBER))
                        .build()).build();
    }

    private byte[] makeValidConfig(ShellConfig.ShellSubscription config) {
        int length = config.toByteArray().length;
        byte[] validConfig = new byte[sizetBytes + length];
        System.arraycopy(IntToByteArrayLittleEndian(length), 0, validConfig, 0, sizetBytes);
        System.arraycopy(config.toByteArray(), 0, validConfig, sizetBytes, length);
        return validConfig;
    }

    private String pushShellConfigToDevice(byte[] validConfig) throws Exception {
        File configFile = File.createTempFile("shellconfig", ".config");
        configFile.deleteOnExit();
        Files.write(validConfig, configFile);
        String remotePath = "/data/local/tmp/" + configFile.getName();
        getDevice().pushFile(configFile, remotePath);
        return remotePath;
    }

    // Starts a subscription and stores shell output in the returned receiver.
    // Must call cancel() on the returned receiver to end the subscription.
    private CollectingByteOutputReceiver startSubscription() throws Exception {
        ShellConfig.ShellSubscription config = createConfig();
        CollectingByteOutputReceiver receiver = new CollectingByteOutputReceiver();
        LogUtil.CLog.d("Uploading the following config:\n" + config.toString());
        byte[] validConfig = makeValidConfig(config);
            // Push the shell config file to the device
            String remotePath = pushShellConfigToDevice(validConfig);

            String cmd = "cat " + remotePath + " |  cmd stats data-subscribe";
            // Run data-subscribe on a thread
            Future<Boolean> future = mExecutor.submit(() -> {
                getDevice().executeShellCommand(cmd, receiver,
                        /*maxTimeToOutputShellResponse=*/0, /*timeUnit=*/null,
                        /*retryAttempts=*/0);
                return true;
            });
            LogUtil.CLog.d("Starting new shell subscription.");

            // Sleep a second to make sure subscription is initiated
            RunUtil.getDefault().sleep(1000);

            // Pushed event. arbitrary label = 1
            AtomTestUtils.sendAppBreadcrumbReportedAtom(getDevice(),
                    AtomsProto.AppBreadcrumbReported.State.UNSPECIFIED.getNumber(), 1);

            // Sleep 2 seconds to make sure the event is processed.
            RunUtil.getDefault().sleep(2000);

            // Remove config from device if not already deleted
            getDevice().executeShellCommand("rm " + remotePath);
        return receiver;
    }

    private byte[] IntToByteArrayLittleEndian(int length) {
        ByteBuffer b = ByteBuffer.allocate(sizetBytes);
        b.order(ByteOrder.LITTLE_ENDIAN);
        b.putInt(length);
        return b.array();
    }

    // We do not know how much data will be returned, but we can check the data format.
    private boolean checkOutput(CollectingByteOutputReceiver receiver) throws Exception {
        int atomCount = 0;
        int startIndex = 0;

        byte[] output = receiver.getOutput();
        LogUtil.CLog.d("output length in checkOutput: " + output.length);
        expect.that(output.length).isGreaterThan(0);
        while (output.length > startIndex) {
            if (output.length < startIndex + sizetBytes) {
                expect.withMessage("output.length < startIndex + sizetBytes check failed").fail();
                return false;
            }
            int dataLength = readSizetFromByteArray(output, startIndex);
            if (dataLength == 0) {
                // We have received a heartbeat from statsd. This heartbeat isn't accompanied by any
                // atoms so return to top of while loop.
                startIndex += sizetBytes;
                continue;
            }
            if (output.length < startIndex + sizetBytes + dataLength) {
                expect.withMessage(
                        "output.length < startIndex + sizetBytes + dataLength check failed").fail();
                return false;
            }

            ShellDataProto.ShellData data = null;
            int dataStart = startIndex + sizetBytes;
            int dataEnd = dataStart + dataLength;
            data = ShellDataProto.ShellData.parseFrom(
                    Arrays.copyOfRange(output, dataStart, dataEnd));

            if (data.getAtomCount() == 1) {
                if (!data.getAtom(0).hasAppBreadcrumbReported()) {
                    expect.withMessage("data doesn't have AppBreadcrumbReported event").fail();
                    return false;
                }
                AtomsProto.AppBreadcrumbReported atom = data.getAtom(0).getAppBreadcrumbReported();
                if (atom.getLabel() != 1) {
                    expect
                        .withMessage("label field should be 1 but was %s", atom.getLabel()).fail();
                    return false;
                }
                if (atom.getState().getNumber() != 1) {
                    expect.withMessage(
                            "state field should be 1 but was %s",
                            atom.getState().getNumber())
                        .fail();
                    return false;
                }
            } else {
                expect.withMessage("data.getAtomCount() is not 1").fail();
                return false;
            }
            atomCount++;
            startIndex += sizetBytes + dataLength;
        }
        if (atomCount <= 0) {
            expect.withMessage("atomCount should be over 0").fail();
            return false;
        }
        return true;
    }

    // Converts the bytes in range [startIndex, startIndex + sizetBytes) from a little-endian array
    // into an integer. Even though sizetBytes could be greater than 4, we assume that the result
    // will fit within an int.
    private int readSizetFromByteArray(byte[] arr, int startIndex) {
        int value = 0;
        for (int j = 0; j < sizetBytes; j++) {
            value += ((int) arr[j + startIndex] & 0xffL) << (8 * j);
        }
        return value;
    }
}
