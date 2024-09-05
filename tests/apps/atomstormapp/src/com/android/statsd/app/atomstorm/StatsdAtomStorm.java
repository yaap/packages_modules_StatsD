/*
 * Copyright (C) 2023 Google LLC.
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

package com.android.statsd.app.atomstorm;

import android.util.StatsLog;

import androidx.test.filters.MediumTest;
import androidx.test.runner.AndroidJUnit4;

import org.junit.Test;
import org.junit.runner.RunWith;

@MediumTest
@RunWith(AndroidJUnit4.class)
public class StatsdAtomStorm {
    private static final int EventStormAtomsCount = 50000;
    private static final int RelaxedLoggingAtomsCount = 10;
    private static final int RecommendedLoggingIntervalMs = 10;

    /** Tests socket overflow. */
    @Test
    public void testLogManyAtomsBackToBack() throws Exception {
        // logging back to back many atoms to force socket overflow
        logAtomsBackToBack(EventStormAtomsCount, 0);

        // Due to the nature of stress test there is some unpredictability aspect, repeating
        // natural atom logging flow several times to have higher guaranty of atom delivery
        // including recommended delay between logging atoms
        for (int i = 0; i < RelaxedLoggingAtomsCount; i++) {
            Thread.sleep(RecommendedLoggingIntervalMs);
            // give chance for libstatssocket send loss stats to statsd triggering
            // successful logging
            logAtomsBackToBack(1, RecommendedLoggingIntervalMs);
        }
    }

    private void logAtomsBackToBack(int iterations, int loggingDelay) throws Exception {
        // single atom logging takes ~2us excluding JNI interactions
        for (int i = 0; i < iterations; i++) {
            StatsLog.logStart(i);
            if (loggingDelay > 0) {
                Thread.sleep(loggingDelay);
            }
            StatsLog.logStop(i);
        }
    }
}
