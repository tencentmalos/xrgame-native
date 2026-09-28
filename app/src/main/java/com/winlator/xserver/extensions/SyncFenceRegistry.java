package com.winlator.xserver.extensions;

import com.winlator.xserver.errors.BadFence;
import com.winlator.xserver.errors.BadIdChoice;
import com.winlator.xserver.errors.BadMatch;
import java.util.HashMap;

/** Waiters retain the fence object so destroying/reusing an XID cannot change their generation. */
final class SyncFenceRegistry {
    private static final class Fence {
        boolean triggered, destroyed;
        Fence(boolean triggered) { this.triggered = triggered; }
    }
    private final HashMap<Integer, Fence> fences = new HashMap<>();

    synchronized void create(int id, boolean triggered) throws BadIdChoice {
        if (fences.containsKey(id)) throw new BadIdChoice(id);
        fences.put(id, new Fence(triggered));
    }

    synchronized void trigger(int id, boolean allowMissing) throws BadFence {
        Fence fence = fences.get(id);
        if (fence == null) {
            if (!allowMissing) throw new BadFence(id);
            return;
        }
        fence.triggered = true;
        notifyAll();
    }

    synchronized void reset(int id) throws BadFence, BadMatch {
        Fence fence = fences.get(id);
        if (fence == null) throw new BadFence(id);
        if (!fence.triggered) throw new BadMatch();
        fence.triggered = false;
    }

    synchronized void destroy(int id) throws BadFence {
        Fence fence = fences.remove(id);
        if (fence == null) throw new BadFence(id);
        fence.destroyed = true;
        notifyAll();
    }

    synchronized void awaitAny(int[] ids) throws BadFence, InterruptedException {
        Fence[] waiting = new Fence[ids.length];
        for (int i = 0; i < ids.length; ++i) {
            waiting[i] = fences.get(ids[i]);
            if (waiting[i] == null) throw new BadFence(ids[i]);
        }
        while (true) {
            for (Fence fence : waiting) {
                if (fence.triggered || fence.destroyed) return;
            }
            // Releases the monitor: another client or Present Idle must be able to trigger.
            wait();
        }
    }
}
