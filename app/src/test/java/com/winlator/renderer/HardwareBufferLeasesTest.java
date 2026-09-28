package com.winlator.renderer;

import org.junit.Test;
import java.util.concurrent.atomic.AtomicInteger;
import static org.junit.Assert.*;

public class HardwareBufferLeasesTest {
    @Test public void replacementReleasesOnlyAfterGpuProofAndExplicitNotification() {
        HardwareBufferLeases leases = new HardwareBufferLeases();
        AtomicInteger idle = new AtomicInteger();
        assertTrue(leases.replace(1, 10, 100, idle::incrementAndGet, () -> true).accepted);
        HardwareBufferLeases.Result result = leases.replace(1, 10, 101, () -> {}, () -> {
            assertEquals(0, idle.get()); return true;
        });
        assertTrue(result.accepted);
        assertEquals(0, idle.get());
        assertEquals(101, leases.get(1).buffer);
        result.notifyRetired();
        assertEquals(1, idle.get());
    }

    @Test public void failedSwapAndRetireKeepTheOriginalLease() {
        HardwareBufferLeases leases = new HardwareBufferLeases();
        AtomicInteger idle = new AtomicInteger();
        leases.replace(1, 10, 100, idle::incrementAndGet, () -> true);
        assertFalse(leases.replace(1, 10, 101, () -> {}, () -> false).accepted);
        assertFalse(leases.retire(1, () -> false).accepted);
        assertEquals(100, leases.get(1).buffer);
        assertEquals(0, idle.get());
        leases.retire(1, () -> true).notifyRetired();
        leases.retire(1, () -> { fail("already retired"); return false; }).notifyRetired();
        assertEquals(1, idle.get());
    }

    @Test public void rejectsBusyBufferAndCrossWindowAliasBeforeGpuWork() {
        HardwareBufferLeases leases = new HardwareBufferLeases();
        leases.replace(1, 10, 100, () -> {}, () -> true);
        for (long window : new long[]{1, 2}) {
            assertFalse(leases.replace(window, window * 10, 100, () -> {}, () -> {
                fail("must not touch a leased image"); return true;
            }).accepted);
        }
    }

    @Test public void resizeMustRetireOldDrawableBeforeNewLease() {
        HardwareBufferLeases leases = new HardwareBufferLeases();
        AtomicInteger idle = new AtomicInteger();
        leases.replace(1, 10, 100, idle::incrementAndGet, () -> true);
        assertFalse(leases.replace(1, 11, 101, () -> {}, () -> true).accepted);
        leases.retire(1, () -> true).notifyRetired();
        assertTrue(leases.replace(1, 11, 101, () -> {}, () -> true).accepted);
        assertEquals(1, idle.get());
    }
}
