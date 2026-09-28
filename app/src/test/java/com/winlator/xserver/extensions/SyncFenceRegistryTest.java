package com.winlator.xserver.extensions;

import com.winlator.xserver.errors.BadFence;
import org.junit.Test;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import static org.junit.Assert.*;

public class SyncFenceRegistryTest {
    private Thread startWaiter(SyncFenceRegistry fences, FutureTask<Void> task) throws Exception {
        Thread thread = new Thread(task);
        thread.setDaemon(true);
        thread.start();
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (thread.getState() != Thread.State.WAITING && thread.isAlive() && System.nanoTime() < deadline)
            Thread.yield();
        assertEquals("Await must sleep without holding the trigger monitor", Thread.State.WAITING, thread.getState());
        return thread;
    }

    @Test(timeout = 5000) public void anotherThreadCanTriggerPendingWait() throws Exception {
        SyncFenceRegistry fences = new SyncFenceRegistry();
        fences.create(1, false);
        fences.create(2, false);
        FutureTask<Void> task = new FutureTask<>(() -> { fences.awaitAny(new int[]{1, 2}); return null; });
        Thread waiter = startWaiter(fences, task);
        try { fences.trigger(2, false); task.get(2, TimeUnit.SECONDS); }
        finally { waiter.interrupt(); }
    }

    @Test(timeout = 5000) public void destructionReleasesOldWaitEvenWhenIdIsReused() throws Exception {
        SyncFenceRegistry fences = new SyncFenceRegistry();
        fences.create(1, false);
        FutureTask<Void> task = new FutureTask<>(() -> { fences.awaitAny(new int[]{1}); return null; });
        Thread waiter = startWaiter(fences, task);
        try {
            synchronized (fences) { fences.destroy(1); fences.create(1, false); }
            task.get(2, TimeUnit.SECONDS);
        } finally { waiter.interrupt(); }
    }

    @Test(timeout = 5000) public void validatesEntireListBeforeAcceptingTriggeredFence() throws Exception {
        SyncFenceRegistry fences = new SyncFenceRegistry();
        fences.create(1, true);
        assertThrows(BadFence.class, () -> fences.awaitAny(new int[]{1, 2}));
        fences.awaitAny(new int[]{1});
    }

    @Test(timeout = 5000) public void shutdownCanInterruptWait() throws Exception {
        SyncFenceRegistry fences = new SyncFenceRegistry();
        fences.create(1, false);
        FutureTask<Void> task = new FutureTask<>(() -> {
            assertThrows(InterruptedException.class, () -> fences.awaitAny(new int[]{1}));
            return null;
        });
        Thread waiter = startWaiter(fences, task);
        waiter.interrupt();
        task.get(2, TimeUnit.SECONDS);
    }
}
