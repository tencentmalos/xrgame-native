package app.gamenative.xrgame;

import java.util.concurrent.atomic.AtomicLong;

/** One progress snapshot per second per operation, even with concurrent native workers. */
public final class ProfileProgressGate {
    private final AtomicLong nextNs = new AtomicLong(Long.MIN_VALUE);
    public boolean sample(long nowNs) {
        long next = nextNs.get();
        return nowNs >= next && nextNs.compareAndSet(next, nowNs + 1_000_000_000L);
    }
}
