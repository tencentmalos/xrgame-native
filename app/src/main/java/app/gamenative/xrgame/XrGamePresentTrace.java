package app.gamenative.xrgame;

import android.util.Log;

/** Bounded, opt-in host timeline. Times use CLOCK_MONOTONIC, not GPU time. */
public final class XrGamePresentTrace {
    private final long generation = System.nanoTime();
    private int remaining;

    public synchronized void configure(int frames) {
        remaining = Math.max(0, Math.min(frames, 3600));
    }

    public synchronized int remainingFrames() { return remaining; }

    public synchronized Frame begin(int window, int pixmap, int serial, int waitFence, int idleFence) {
        if (remaining == 0) return null;
        --remaining;
        Frame frame = new Frame(generation, System.nanoTime(), window, pixmap, serial);
        frame.event("receive", " wait_fence=" + Integer.toUnsignedString(waitFence)
                + " idle_fence=" + Integer.toUnsignedString(idleFence));
        return frame;
    }

    public static final class Frame {
        public final long id;
        private final long generation;
        private final int window, pixmap, serial;

        private Frame(long generation, long id, int window, int pixmap, int serial) {
            this.generation = generation;
            this.id = id;
            this.window = window;
            this.pixmap = pixmap;
            this.serial = serial;
        }

        public void event(String event, String detail) {
            Log.d("XRGamePresentTrace", "event=" + event + " generation=" + generation
                    + " frame=" + id + " mono_ns=" + System.nanoTime()
                    + " window=" + Integer.toUnsignedString(window)
                    + " pixmap=" + Integer.toUnsignedString(pixmap)
                    + " serial=" + Integer.toUnsignedString(serial) + detail);
        }
    }
}
