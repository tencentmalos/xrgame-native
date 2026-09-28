package com.winlator.renderer;

import java.util.HashMap;
import java.util.ArrayList;
import java.util.function.BooleanSupplier;

/** One retained Present per window. Callers serialize this registry with renderer changes.
 * A successful GPU operation must retire all previous reads and prevent future reads.
 * Callbacks are returned to the caller, so they run after releasing the renderer lock.
 */
final class HardwareBufferLeases {
    static final class Lease {
        final long drawable, buffer;
        final Runnable idle;
        Lease(long drawable, long buffer, Runnable idle) {
            this.drawable = drawable; this.buffer = buffer; this.idle = idle;
        }
    }

    static final class Result {
        final boolean accepted;
        final Runnable retired;
        Result(boolean accepted, Runnable retired) { this.accepted = accepted; this.retired = retired; }
        void notifyRetired() { if (retired != null) retired.run(); }
    }

    private final HashMap<Long, Lease> windows = new HashMap<>();
    Lease get(long window) { return windows.get(window); }
    ArrayList<Long> windowIds() { return new ArrayList<>(windows.keySet()); }

    Result replace(long window, long drawable, long buffer, Runnable idle, BooleanSupplier gpuSwap) {
        Lease old = windows.get(window);
        if (buffer == 0 || (old != null && old.drawable != drawable)) return new Result(false, null);
        // Re-presenting a buffer still leased to any window cannot release the old Present.
        // Reject it rather than accumulating callbacks or allowing Guest reuse during a read.
        for (Lease lease : windows.values()) if (lease.buffer == buffer) return new Result(false, null);
        if (!gpuSwap.getAsBoolean()) return new Result(false, null);
        windows.put(window, new Lease(drawable, buffer, idle));
        return new Result(true, old == null ? null : old.idle);
    }

    Result retire(long window, BooleanSupplier gpuRetire) {
        Lease old = windows.get(window);
        if (old == null) return new Result(true, null);
        if (!gpuRetire.getAsBoolean()) return new Result(false, null);
        windows.remove(window);
        return new Result(true, old.idle);
    }
}
