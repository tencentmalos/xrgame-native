#pragma once

#include "xr_windows_transport.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>

namespace xrimmersive::windowsvr {

class WindowsProjectionPresenter {
public:
    bool initialize(XrSession session, int64_t format, uint32_t width, uint32_t height, EGLDisplay display);
    bool render(WindowsFrameTransport &transport, XrSpace space, XrCompositionLayerProjection *layer);
    bool initializeSbs(EGLDisplay display);
    bool renderSbs(WindowsFrameTransport &transport, uint32_t width, uint32_t height);
    uint64_t sbsFrameCount() const { return sbsFrameCount_; }
    // Whether the last successful render() resubmitted the previously released image.
    bool lastRenderReused() const { return lastRenderReused_; }
    // FRAME_SYNC serial of the game frame the last render() drew for the first time, or -1.
    int64_t lastFreshSnap() const { return lastFreshSnap_; }
    // The runtime rejected a frame that reused the released image: always redraw from now on.
    void disableReuse() { reuseReleased_ = false; }
    void shutdown();

private:
    bool ensureProgram();
    bool createSwapchain(uint32_t width, uint32_t height);
    bool matchSourceSize(const EyeFrame &source);
    EGLImageKHR createImageFromHardwareBuffer(AHardwareBuffer *buffer);
    EGLImageKHR createImageFromDmabuf(const EyeFrame &frame);
    bool waitForAcquireFence(int fenceFd);
    int createReleaseFence();
    bool uploadLinearDmabufToTexture(uint32_t eye, int imageIndex, const EyeFrame &frame,
                                    GLuint &texture, uint64_t &cachedRegistration);
    bool importEyeBuffer(WindowsFrameTransport &transport, uint32_t eye, EyeFrame &frame, bool &fresh,
                         bool poll = true);
    void drawSource(uint32_t eye, const EyeFrame &source);
    void releaseFresh(WindowsFrameTransport &transport, const std::array<EyeFrame, 2> &frames,
                      const std::array<bool, 2> &fresh);
    void drawEye(uint32_t eye, const EyeFrame &source, uint32_t imageIndex);
    void discardFresh(WindowsFrameTransport &transport, const std::array<EyeFrame, 2> &frames,
                      const std::array<bool, 2> &fresh);

    XrSession session_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    int64_t format_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t maxWidth_ = 0;
    uint32_t maxHeight_ = 0;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    std::vector<XrSwapchainImageOpenGLESKHR> images_;
    std::array<XrCompositionLayerProjectionView, 2> views_{};
    std::array<std::array<EGLImageKHR, WindowsFrameTransport::kMaxImages>, 2> eglImages_{};
    std::array<std::array<GLuint, WindowsFrameTransport::kMaxImages>, 2> textures_{};
    std::array<std::array<uint64_t, WindowsFrameTransport::kMaxImages>, 2> registrations_{};
    std::array<std::array<bool, WindowsFrameTransport::kMaxImages>, 2> cpuFallback_{};
    std::array<std::array<void *, WindowsFrameTransport::kMaxImages>, 2> cpuMappings_{};
    std::array<std::array<size_t, WindowsFrameTransport::kMaxImages>, 2> cpuMappingLengths_{};
    std::array<std::array<uint64_t, WindowsFrameTransport::kMaxImages>, 2> cpuMappingRegistrations_{};
    std::array<std::array<int, WindowsFrameTransport::kMaxImages>, 2> cpuTextureWidths_{};
    std::array<std::array<int, WindowsFrameTransport::kMaxImages>, 2> cpuTextureHeights_{};
    std::array<uint64_t, 2> renderedSerials_{0, 0};
    bool releasedImageValid_ = false;
    bool reuseReleased_ = true;
    bool lastRenderReused_ = false;
    int64_t lastFreshSnap_ = -1;
    EGLSyncKHR acquireSync_ = EGL_NO_SYNC_KHR;
    GLuint framebuffer_ = 0;
    GLuint program_ = 0;
    GLuint vertexBuffer_ = 0;
    GLuint vertexArray_ = 0;
    GLint uvTransformLocation_ = -1;
    GLuint sbsTexture_ = 0;
    bool sbsReady_ = false;
    uint64_t sbsFrameCount_ = 0;
};

}
