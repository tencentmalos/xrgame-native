package com.winlator.renderer;

/** Null except for the Meta Quest immersive path (see ImmersiveXrActivity's DirectVulkanBridge). */
public interface VulkanXrFrameBridge {
    /**
     * ahbPtr is the same raw AHardwareBuffer* (as a jlong) VulkanRenderer is handing to
     * nativeUpdateWindowContentAHB for on-screen presentation — read-only, do not release/free it.
     * Called on every real frame update for a Vulkan-rendered window (see this interface's kdoc
     * for why "scanout" in the name is no longer literally accurate).
     */
    void onScanoutBuffer(long ahbPtr, int width, int height);

    /**
     * exchangePtr is the renderer's scene target (a native xrgame::XrFrameExchange*, see
     * xr_frame_exchange.h): a ring of AHardwareBuffers with the latest completed frame. Called
     * when the target is created or resized; the session takes its own reference.
     */
    void onSceneExchange(long exchangePtr, int width, int height);
}
