/*
 * Copyright (C) 2023 The Android Open Source Project
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

package android.util;

import com.android.os.AtomsProto.Atom;

import com.google.protobuf.ExtensionRegistryLite;
import com.google.protobuf.InvalidProtocolBufferException;

/** Provides utility methods for parsing StatsEvent and StatsLogItem objects in tests. */
public final class StatsEventTestUtils {
    private StatsEventTestUtils() {} // no instances.

    /**
     * Converts StatsEvent to MessageLite representation of Atom. Calls StatsEvent#release; No
     * further actions should be taken on the StatsEvent object.
     */
    public static Atom convertToAtom(StatsEvent statsEvent) throws InvalidProtocolBufferException {
        return convertToAtom(statsEvent, ExtensionRegistryLite.getEmptyRegistry());
    }

    /**
     * Converts StatsEvent to MessageLite representation of Atom for extensions. The extension can
     * be accessed with {@link Atom#getExtension}.
     *
     * <p>Calls StatsEvent#release; No further actions should be taken on the StatsEvent object.
     */
    public static Atom convertToAtom(StatsEvent statsEvent, ExtensionRegistryLite registry)
            throws InvalidProtocolBufferException {
        try {
            byte[] protoBytes =
                    AtomPayloadParser.getProtoBytes(
                            statsEvent.getBytes(), statsEvent.getNumBytes());
            return Atom.parseFrom(protoBytes, registry);
        } finally {
            statsEvent.release();
        }
    }

}
