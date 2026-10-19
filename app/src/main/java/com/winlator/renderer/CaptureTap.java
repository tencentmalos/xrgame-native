package com.winlator.renderer;

import android.graphics.Bitmap;

/**
 * What an X server renderer's surface shows, for the debug capture SDK's 2D window producer
 * ({@code app.gamenative.xrgame.XrPanelCapture}): a SurfaceView's content is not part of the
 * window that View drawing reproduces. The renderer copies its frames only while the tap is on.
 */
public interface CaptureTap {
    /** On while a capture runs; turning it on renders one frame. Any thread. */
    void setCaptureTap(boolean enabled);

    /** The newest complete frame in the surface's orientation, or null before the first one. */
    Bitmap captureFrame();
}
