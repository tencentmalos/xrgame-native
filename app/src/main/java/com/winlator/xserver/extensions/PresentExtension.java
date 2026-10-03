package com.winlator.xserver.extensions;

import static com.winlator.xserver.XClientRequestHandler.RESPONSE_CODE_SUCCESS;

import android.util.SparseArray;

import com.winlator.renderer.ASurfaceRenderer;
import com.winlator.renderer.GPUImage;
import com.winlator.renderer.Texture;
import com.winlator.renderer.VulkanRenderer;
import com.winlator.renderer.XServerRenderer;
import com.winlator.xconnector.XInputStream;
import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;
import com.winlator.xenvironment.components.VortekRendererComponent;
import com.winlator.xserver.Bitmask;
import com.winlator.xserver.Drawable;
import com.winlator.xserver.Pixmap;
import com.winlator.xserver.Window;
import com.winlator.xserver.XClient;
import com.winlator.xserver.XLock;
import com.winlator.xserver.XServer;
import com.winlator.xserver.errors.BadImplementation;
import com.winlator.xserver.errors.BadMatch;
import com.winlator.xserver.errors.BadPixmap;
import com.winlator.xserver.errors.BadWindow;
import com.winlator.xserver.errors.XRequestError;
import com.winlator.xserver.events.PresentCompleteNotify;
import com.winlator.xserver.events.PresentIdleNotify;

import java.io.IOException;
import app.gamenative.xrgame.XrGamePresentTrace;

public class PresentExtension implements Extension {
    private final java.util.concurrent.atomic.AtomicReference<app.gamenative.xrgame.XrGameProfiler.Region> firstPresent =
            new java.util.concurrent.atomic.AtomicReference<>(app.gamenative.xrgame.XrGameProfiler.region("x11.server_to_first_present_request"));
    public static final byte MAJOR_OPCODE = -103;
    public enum Kind { PIXMAP, MSC_NOTIFY }
    public enum Mode { COPY, FLIP, SKIP }

    private final XrGamePresentTrace presentTrace = new XrGamePresentTrace();
    private final Object copyModeLock = new Object();
    private boolean asyncCopyRequested = false, asyncCopyActive = false;
    private boolean copyPipelineRequested = false, copyPipelineActive = false;
    private int outstandingCopies = 0;
    private int requestedFrameRateLimit = 0;
    private volatile boolean closed = false;
    private final java.util.concurrent.atomic.AtomicLong asyncCompleted = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong asyncSkipped = new java.util.concurrent.atomic.AtomicLong();
    private final java.util.concurrent.atomic.AtomicLong asyncFailed = new java.util.concurrent.atomic.AtomicLong();

    public void setAsyncCopy(boolean enabled) {
        synchronized (copyModeLock) {
            asyncCopyRequested = app.gamenative.BuildConfig.XRGAME && enabled;
            applyCopySettingsWhenDrained();
        }
    }

    /** Experimental GPU-ordered draw with independently fenced copy retirement. */
    public void setCopyPipeline(boolean enabled) {
        synchronized (copyModeLock) {
            copyPipelineRequested = app.gamenative.BuildConfig.XRGAME && enabled;
            applyCopySettingsWhenDrained();
        }
    }

    // copyModeLock held: keep the current mode/cap until every accepted copy retires.
    private void applyCopySettingsWhenDrained() {
        if (outstandingCopies == 0) {
            frameRateLimit = requestedFrameRateLimit;
            asyncCopyActive = asyncCopyRequested && frameRateLimit == 0;
            copyPipelineActive = asyncCopyActive && copyPipelineRequested;
        }
    }

    public void setTraceFrames(int frames) { presentTrace.configure(frames); }

    /** Approximate concurrent host snapshot; never takes the renderer/GPU or X server locks. */
    public java.util.Map<String, Object> diagnosticSnapshot() {
        java.util.Map<String, Object> result = new java.util.LinkedHashMap<>();
        result.put("pendingIdles", pendingIdles.size());
        result.put("cpuQueuedIdles", cpuQueue.size());
        result.put("frameRateLimit", frameRateLimit);
        result.put("eagerIdleRelease", eagerIdleRelease);
        result.put("traceFramesRemaining", presentTrace.remainingFrames());
        synchronized (copyModeLock) {
            result.put("asyncCopyRequested", asyncCopyRequested);
            result.put("asyncCopyActive", asyncCopyActive);
            result.put("copyPipelineRequested", copyPipelineRequested);
            result.put("copyPipelineActive", copyPipelineActive);
            result.put("outstandingCopies", outstandingCopies);
            result.put("requestedFrameRateLimit", requestedFrameRateLimit);
        }
        result.put("asyncCompleted", asyncCompleted.get());
        result.put("asyncSkipped", asyncSkipped.get());
        result.put("asyncFailed", asyncFailed.get());
        result.put("atomicSnapshot", false);
        return result;
    }

    private final SparseArray<Event> events = new SparseArray<>();
    private SyncExtension syncExtension;
    private byte firstEventId = 0;
    private byte firstErrorId = 0;

    // Target FPS for the back-pressure limiter. 0 disables limiting (notifies fire
    // immediately). Set from XServerScreen when the user toggles the FPS cap.
    private volatile int frameRateLimit = 0;

    // Mailbox semantics for the pacing scheduler: when a new present supersedes a
    // still-pending pixmap on the same window, release the superseded one
    // immediately instead of holding it to the schedule. Armed only while
    // bionic-fg is active — the layer owns base pacing then, and it presents at
    // multiplier x the cap, so releases must track presents or its generated
    // frames starve the swapchain.
    private volatile boolean eagerIdleRelease = false;
    private int supersededDrops = 0;

    public void setEagerIdleRelease(boolean eager) {
        this.eagerIdleRelease = eager;
    }

    private static class PendingIdle {
        Window window; Pixmap pixmap; int serial; int idleFence;
        XrGamePresentTrace.Frame trace;
        long targetNs;
        int  vsyncSkips;    // vsyncs left to skip before firing (for fps < refresh)
        PendingIdle(Window w, Pixmap p, int s, int f, long t, int sk, XrGamePresentTrace.Frame trace) {
            this.trace = trace;
            window = w; pixmap = p; serial = s; idleFence = f; targetNs = t; vsyncSkips = sk;
        }
    }

    private final java.util.concurrent.ConcurrentHashMap<Integer, PendingIdle> pendingIdles =
            new java.util.concurrent.ConcurrentHashMap<>();

    private volatile android.view.Choreographer choreographer = null;
    private volatile boolean choreographerChecked = false;
    private final Object choreographerLock = new Object();

    private volatile Thread cpuPacerThread = null;
    private final java.util.concurrent.PriorityBlockingQueue<PendingIdle> cpuQueue =
            new java.util.concurrent.PriorityBlockingQueue<>(11,
                    java.util.Comparator.comparingLong(p -> p.targetNs));

    private static final long FIRE_EARLY_NS = 700_000L; // 0.7 ms

    public void setFrameRateLimit(int limit) {
        synchronized (copyModeLock) {
            requestedFrameRateLimit = Math.max(0, limit);
            applyCopySettingsWhenDrained();
        }
    }

    public void close() {
        closed = true;
        app.gamenative.xrgame.XrGameProfiler.Region pending = firstPresent.getAndSet(null);
        if (pending != null) {
            pending.close();
            app.gamenative.xrgame.XrGameProfiler.mark("x11.closed_without_present_request");
        }
        synchronized (choreographerLock) {
            if (cpuPacerThread != null) {
                cpuPacerThread.interrupt();
                cpuPacerThread = null;
            }
        }
    }

    private android.view.Choreographer tryGetChoreographer(VulkanRenderer renderer) {
        if (choreographerChecked) return choreographer;
        synchronized (choreographerLock) {
            if (choreographerChecked) return choreographer;
            choreographerChecked = true;
            try {
                if (renderer != null && renderer.xServerView != null) {
                    choreographer = android.view.Choreographer.getInstance();
                }
            } catch (Exception ignored) {
                android.util.Log.w("PresentExtension", "Choreographer unavailable, using CPU pacer");
            }
            if (choreographer == null) {
                startCpuPacer();
            }
            return choreographer;
        }
    }

    private void startCpuPacer() {
        // Called under choreographerLock, which also serializes close().
        if (closed || cpuPacerThread != null) return;
        cpuPacerThread = new Thread(() -> {
            while (!Thread.interrupted()) {
                PendingIdle p = cpuQueue.peek();
                if (p == null) {
                    // unpark's permit survives an offer between peek and park.
                    java.util.concurrent.locks.LockSupport.park(this);
                    continue;
                }
                long now = System.nanoTime();
                if (now >= p.targetNs) {
                    if (cpuQueue.remove(p)) {
                        sendIdleNotify(p.window, p.pixmap, p.serial, p.idleFence, p.trace);
                    }
                } else {
                    long diff = p.targetNs - now;
                    if (diff > 2_000_000L)
                        java.util.concurrent.locks.LockSupport.parkNanos(1_000_000L);
                    else
                        Thread.yield();
                }
            }
        }, "PresentPacer-CPU");
        cpuPacerThread.setDaemon(true);
        cpuPacerThread.setPriority(Thread.MAX_PRIORITY);
        cpuPacerThread.start();
    }

    private void enqueueCpuIdle(PendingIdle idle) {
        cpuQueue.offer(idle);
        // Also interrupt a timed wait when an earlier deadline is inserted.
        java.util.concurrent.locks.LockSupport.unpark(cpuPacerThread);
    }

    private volatile boolean choreographerPosted = false;
    private final android.view.Choreographer.FrameCallback vsyncCallback = frameTimeNs -> {
        choreographerPosted = false;
        boolean anyRemaining = false;
        for (java.util.Iterator<java.util.Map.Entry<Integer, PendingIdle>> it =
             pendingIdles.entrySet().iterator(); it.hasNext(); ) {
            PendingIdle p = it.next().getValue();
            if (frameTimeNs >= p.targetNs) {
                if (p.vsyncSkips > 0) {
                    p.vsyncSkips--;
                    anyRemaining = true;
                } else if (pendingIdles.remove(p.window.id, p)) {
                    sendIdleNotify(p.window, p.pixmap, p.serial, p.idleFence, p.trace);
                }
            } else {
                anyRemaining = true;
            }
        }
        if (anyRemaining) postChoreographerCallback();
    };

    private void postChoreographerCallback() {
        if (choreographer == null || choreographerPosted) return;
        choreographerPosted = true;
        choreographer.postFrameCallback(vsyncCallback);
    }

    private static class WindowTiming { long nextIdleNs = 0; }
    private final java.util.concurrent.ConcurrentHashMap<Integer, WindowTiming> windowTimings =
            new java.util.concurrent.ConcurrentHashMap<>();

    private void scheduleIdleNotify(Window window, Pixmap pixmap, int serial,
                                    int idleFence, int targetFps, VulkanRenderer renderer) {
        scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, renderer, null);
    }

    private void scheduleIdleNotify(Window window, Pixmap pixmap, int serial,
                                    int idleFence, int targetFps, VulkanRenderer renderer,
                                    XrGamePresentTrace.Frame trace) {
        if (targetFps <= 0) {
            sendIdleNotify(window, pixmap, serial, idleFence, trace);
            return;
        }

        final long frameNs = 1_000_000_000L / targetFps;
        long now = System.nanoTime();

        WindowTiming wt = windowTimings.computeIfAbsent(window.id, k -> new WindowTiming());
        if (wt.nextIdleNs <= now - frameNs) {
            wt.nextIdleNs = now + frameNs;
        } else {
            wt.nextIdleNs += frameNs;
        }
        long fireTime = wt.nextIdleNs - FIRE_EARLY_NS;

        android.view.Choreographer ch = tryGetChoreographer(renderer);
        if (ch != null) {
            PendingIdle superseded = pendingIdles.put(window.id,
                    new PendingIdle(window, pixmap, serial, idleFence, fireTime, 0, trace));
            if (superseded != null) {
                if (eagerIdleRelease || app.gamenative.BuildConfig.XRGAME) {
                    sendIdleNotify(superseded.window, superseded.pixmap,
                            superseded.serial, superseded.idleFence, superseded.trace);
                } else if (supersededDrops++ < 8) {
                    android.util.Log.w("PresentExtension", "pending idle superseded and dropped"
                            + " for window 0x" + Integer.toHexString(window.id)
                            + " serial " + superseded.serial);
                }
            }
            postChoreographerCallback();
        } else {
            if (eagerIdleRelease) {
                for (PendingIdle q : cpuQueue) {
                    if (q.window == window && cpuQueue.remove(q)) {
                        sendIdleNotify(q.window, q.pixmap, q.serial, q.idleFence, q.trace);
                    }
                }
            }
            enqueueCpuIdle(new PendingIdle(window, pixmap, serial, idleFence, fireTime, 0, trace));
        }
    }

    private static abstract class ClientOpcodes {
        static final byte QUERY_VERSION = 0;
        static final byte PRESENT_PIXMAP = 1;
        static final byte SELECT_INPUT = 3;
    }

    private static class Event {
        Window window;
        XClient client;
        int id;
        Bitmask mask;
    }

    @Override
    public String getName() { return "Present"; }

    @Override
    public byte getMajorOpcode() { return MAJOR_OPCODE; }

    @Override
    public int getNumEvents() { return 2; }

    @Override
    public int getNumErrors() { return 0; }

    @Override
    public void setFirstEventId(byte id) { this.firstEventId = id; }

    @Override
    public void setFirstErrorId(byte id) { this.firstErrorId = id; }

    @Override
    public byte getFirstEventId() { return firstEventId; }

    @Override
    public byte getFirstErrorId() { return firstErrorId; }

    private void sendIdleNotify(Window window, Pixmap pixmap, int serial, int idleFence) {
        sendIdleNotify(window, pixmap, serial, idleFence, null);
    }

    private void sendIdleNotify(Window window, Pixmap pixmap, int serial, int idleFence,
                                XrGamePresentTrace.Frame trace) {
        if (trace != null) trace.event("idle", "");
        if (idleFence != 0 && syncExtension != null) syncExtension.setTriggered(idleFence);
        synchronized (events) {
            for (int i = 0; i < events.size(); i++) {
                Event e = events.valueAt(i);
                if (e.window == window && e.mask.isSet(PresentIdleNotify.getEventMask())) {
                    e.client.sendEvent(new PresentIdleNotify(e.id, window, pixmap, serial, idleFence));
                }
            }
        }
    }

    private void sendCompleteNotify(Window window, int serial, Kind kind, Mode mode, long ust, long msc) {
        synchronized (events) {
            for (int i = 0; i < events.size(); i++) {
                Event e = events.valueAt(i);
                if (e.window == window && e.mask.isSet(PresentCompleteNotify.getEventMask())) {
                    e.client.sendEvent(new PresentCompleteNotify(e.id, window, serial, kind, mode, ust, msc));
                }
            }
        }
    }

    private static void queryVersion(XClient client, XInputStream inputStream, XOutputStream outputStream) throws IOException, XRequestError {
        inputStream.skip(8);

        try (XStreamLock lock = outputStream.lock()) {
            outputStream.writeByte(RESPONSE_CODE_SUCCESS);
            outputStream.writeByte((byte)0);
            outputStream.writeShort(client.getSequenceNumber());
            outputStream.writeInt(0);
            outputStream.writeInt(1);
            outputStream.writeInt(0);
            outputStream.writePad(16);
        }
    }

    private void presentPixmap(XClient client, XInputStream inputStream, XOutputStream outputStream) throws IOException, XRequestError {
        int windowId = inputStream.readInt();
        int pixmapId = inputStream.readInt();
        int serial = inputStream.readInt();
        inputStream.skip(8);
        short xOff = inputStream.readShort();
        short yOff = inputStream.readShort();
        inputStream.skip(4); // target CRTC
        int waitFence = inputStream.readInt();
        int idleFence = inputStream.readInt();
        inputStream.skip(client.getRemainingRequestLength());

        final Window window = client.xServer.windowManager.getWindow(windowId);
        if (window == null) throw new BadWindow(windowId);

        final Pixmap pixmap = client.xServer.pixmapManager.getPixmap(pixmapId);
        if (pixmap == null) throw new BadPixmap(pixmapId);

        app.gamenative.xrgame.XrGameProfiler.present();
        if (firstPresent.get() != null) {
            app.gamenative.xrgame.XrGameProfiler.Region pending = firstPresent.getAndSet(null);
            if (pending != null) {
                pending.close();
                app.gamenative.xrgame.XrGameProfiler.mark("x11.first_present_request");
            }
        }
        Drawable content = window.getContent();
        int contentDepth = content.visual.depth;
        int pixmapDepth = pixmap.drawable.visual.depth;
        boolean depthCompat = (contentDepth == pixmapDepth) ||
                ((contentDepth == 24 || contentDepth == 32) && (pixmapDepth == 24 || pixmapDepth == 32));
        if (!depthCompat) throw new BadMatch();

        final XServerRenderer xr = client.xServer.getRenderer();
        final VulkanRenderer vr = (xr instanceof VulkanRenderer) ? (VulkanRenderer) xr : null;
        final ASurfaceRenderer asr = (xr instanceof ASurfaceRenderer) ? (ASurfaceRenderer) xr : null;
        final int targetFps = this.frameRateLimit;

        long ust = System.nanoTime() / 1000;
        long msc = ust / (targetFps > 0 ? (1_000_000L / targetFps) : (1_000_000L / 60));

        if (!client.xServer.isFlatPresentationEnabled()) {
            sendCompleteNotify(window, serial, Kind.PIXMAP, Mode.SKIP, ust, msc);
            if (targetFps > 0) scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, vr);
            else sendIdleNotify(window, pixmap, serial, idleFence);
            return;
        }

        synchronized (content.renderLock) {
            if (app.gamenative.BuildConfig.XRGAME && pixmap.drawable.getTexture() instanceof GPUImage && vr != null) {
                // Release the guest image only after the host GPU has finished reading it.
                XrGamePresentTrace.Frame trace = presentTrace.begin(windowId, pixmapId, serial, waitFence, idleFence);
                final boolean queuedCopy;
                final boolean pipelineCopy;
                synchronized (copyModeLock) {
                    // Switching modes waits for accepted copies to retire. Unsupported
                    // request shapes retain the existing synchronous path when drained.
                    applyCopySettingsWhenDrained();
                    queuedCopy = asyncCopyActive && waitFence == 0 && idleFence == 0 && frameRateLimit == 0
                            && xOff == 0 && yOff == 0 && !vr.canSampleHardwareBuffers();
                    if (!queuedCopy && outstandingCopies != 0) throw new com.winlator.xserver.errors.BadAlloc();
                    if (queuedCopy) ++outstandingCopies;
                    pipelineCopy = copyPipelineActive;
                }
                if (queuedCopy) {
                    final short sequence = client.getSequenceNumber();
                    final byte opcode = client.getRequestData();
                    boolean accepted = vr.queueHardwarePixmap(window, (GPUImage)pixmap.drawable.getTexture(),
                            trace == null ? 0 : trace.id, pipelineCopy, result -> {
                                try {
                                    if (result < 0) {
                                        asyncFailed.incrementAndGet();
                                        if (trace != null) trace.event("copy_failed", " mode=async");
                                        android.util.Log.e("PresentExtension", "Asynchronous copy failed; no Idle/Complete sent");
                                        if (!closed) try {
                                            new com.winlator.xserver.errors.BadAlloc().sendError(client, MAJOR_OPCODE, sequence, opcode);
                                        } catch (IOException ignored) {}
                                        return;
                                    }
                                    if (result == 0) asyncCompleted.incrementAndGet();
                                    else asyncSkipped.incrementAndGet();
                                    if (closed) return;
                                    long doneUst = System.nanoTime() / 1000;
                                    sendCompleteNotify(window, serial, Kind.PIXMAP, result == 0 ? Mode.COPY : Mode.SKIP,
                                            doneUst, doneUst / 16667);
                                    if (trace != null) trace.event("complete", " mode=async result=" + result);
                                    sendIdleNotify(window, pixmap, serial, idleFence, trace);
                                } finally {
                                    synchronized (copyModeLock) {
                                        --outstandingCopies;
                                        applyCopySettingsWhenDrained();
                                    }
                                }
                            });
                    if (!accepted) {
                        synchronized (copyModeLock) {
                            --outstandingCopies;
                            applyCopySettingsWhenDrained();
                        }
                        if (trace != null) trace.event("copy_rejected", " mode=async");
                        throw new com.winlator.xserver.errors.BadAlloc();
                    }
                    if (trace != null) trace.event("enqueue_return", " mode=async");
                    return;
                }
                if (vr.canSampleHardwareBuffers() && waitFence == 0 && xOff == 0 && yOff == 0
                        && window.attributes.isMapped()) {
                    if (!vr.sampleHardwarePixmap(window, (GPUImage)pixmap.drawable.getTexture(),
                            trace == null ? 0 : trace.id,
                            () -> {
                                long acceptedUst = System.nanoTime() / 1000;
                                sendCompleteNotify(window, serial, Kind.PIXMAP, Mode.FLIP, acceptedUst, acceptedUst / 16667);
                                if (trace != null) trace.event("complete", " mode=sample");
                            },
                            () -> scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, vr, trace))) {
                        if (trace != null) trace.event("sample_failed", "");
                        throw new com.winlator.xserver.errors.BadAlloc();
                    }
                    // Idle belongs to the future GPU retirement, not this acceptance.
                    return;
                }
                if (!vr.copyHardwarePixmap(window, (GPUImage)pixmap.drawable.getTexture(), trace == null ? 0 : trace.id)) {
                    if (trace != null) trace.event("copy_failed", "");
                    throw new com.winlator.xserver.errors.BadAlloc();
                }
                long completedUst = System.nanoTime() / 1000;
                sendCompleteNotify(window, serial, Kind.PIXMAP, Mode.COPY, completedUst, completedUst / 16667);
                if (trace != null) trace.event("complete", " mode=copy");
                scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, vr, trace);
                return;
            }
            if (asr != null) {
                content.setTexture(pixmap.drawable.getTexture());
                sendCompleteNotify(window, serial, Kind.PIXMAP, Mode.FLIP, ust, msc);
                if (window.attributes.isMapped()) {
                    asr.onUpdateWindowContent(window);
                }
                if (targetFps > 0) scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, vr);
                else sendIdleNotify(window, pixmap, serial, idleFence);
            } else if (vr != null && window.attributes.isMapped()) {
                sendCompleteNotify(window, serial, Kind.PIXMAP, Mode.COPY, ust, msc);
                vr.onUpdateWindowContentDirect(window, pixmap.drawable, xOff, yOff);
                if (targetFps > 0) scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, vr);
                else sendIdleNotify(window, pixmap, serial, idleFence);
            } else {
                // GL Renderer
                content.copyArea((short)0, (short)0, xOff, yOff,
                        pixmap.drawable.width, pixmap.drawable.height, pixmap.drawable);
                sendCompleteNotify(window, serial, Kind.PIXMAP, Mode.COPY, ust, msc);
                if (targetFps > 0) scheduleIdleNotify(window, pixmap, serial, idleFence, targetFps, vr);
                else sendIdleNotify(window, pixmap, serial, idleFence);
            }
        }
    }

    private void selectInput(XClient client, XInputStream inputStream, XOutputStream outputStream) throws IOException, XRequestError {
        int eventId = inputStream.readInt();
        int windowId = inputStream.readInt();
        Bitmask mask = new Bitmask(inputStream.readInt());

        Window window = client.xServer.windowManager.getWindow(windowId);
        if (window == null) throw new BadWindow(windowId);

        if (!app.gamenative.BuildConfig.XRGAME && GPUImage.isSupported() && !mask.isEmpty()) {
            Drawable content = window.getContent();
            final Texture oldTexture = content.getTexture();
            if (oldTexture != null && !(oldTexture instanceof GPUImage)) {
                XServerRenderer r = client.xServer.getRenderer();
                if (r != null)
                    r.getRendererView().queueEvent(() -> VortekRendererComponent.destroyTexture(oldTexture));
            }
            if (!(content.getTexture() instanceof GPUImage))
                content.setTexture(new GPUImage(content.width, content.height));
        }

        synchronized (events) {
            Event event = events.get(eventId);
            if (event != null) {
                if (event.window != window || event.client != client) throw new BadMatch();
                if (!mask.isEmpty()) event.mask = mask;
                else events.remove(eventId);
            } else {
                event = new Event();
                event.id = eventId;
                event.window = window;
                event.client = client;
                event.mask = mask;
                events.put(eventId, event);
            }
        }
    }

    @Override
    public void handleRequest(XClient client, XInputStream inputStream, XOutputStream outputStream) throws IOException, XRequestError {
        int opcode = client.getRequestData();
        if (syncExtension == null) syncExtension = client.xServer.getExtension(SyncExtension.MAJOR_OPCODE);

        switch (opcode) {
            case ClientOpcodes.QUERY_VERSION:
                queryVersion(client, inputStream, outputStream);
                break;
            case ClientOpcodes.PRESENT_PIXMAP:
                try (XLock lock = client.xServer.lock(XServer.Lockable.WINDOW_MANAGER)) {
                    presentPixmap(client, inputStream, outputStream);
                }
                break;
            case ClientOpcodes.SELECT_INPUT:
                try (XLock lock = client.xServer.lock(XServer.Lockable.WINDOW_MANAGER)) {
                    selectInput(client, inputStream, outputStream);
                }
                break;
            default:
                throw new BadImplementation();
        }
    }
}
