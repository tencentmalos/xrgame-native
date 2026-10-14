// SPDX-License-Identifier: GPL-2.0-or-later
#include "xr_panel_feed.h"

#if XRGAME_CAPTURE

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
// After gl3.h, which defines the types it uses.
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <media/NdkImageReader.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::capture::panel {
namespace {

// One image with the thread, one waiting, and one more for acquireLatestImage's swap.
constexpr int32_t kReaderImages = 4;
// The reader's buffers are a small fixed set; anything beyond this means it reallocated.
constexpr size_t kMaxTargets = 16;
constexpr int kFenceTimeoutMs = 1000;

uint64_t NowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(now.tv_nsec);
}

using PresentationTime = EGLBoolean (*)(EGLDisplay, EGLSurface, EGLnsecsANDROID);

struct Image {
    AImage *image = nullptr;
    int fence = -1;  // HWUI's rendering into it; -1 once waited for
};

// Gives an image back to the reader. An unwaited acquire fence goes back with it, so HWUI does not
// render into the buffer again before its last rendering finished.
void Release(Image &image) {
    if (image.image) {
        AImage_deleteAsync(image.image, image.fence);
    } else if (image.fence >= 0) {
        close(image.fence);
    }
    image = {};
}

}  // namespace

struct Feed::Impl {
    AImageReader *reader = nullptr;
    ANativeWindow *window = nullptr;   // the reader's
    ANativeWindow *encoder = nullptr;  // borrowed
    uint32_t width = 0;
    uint32_t height = 0;
    std::thread thread;

    mutable std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    Image pending;
    FeedStats stats;
    uint64_t postStartNs = 0;
    std::string error;

    // The thread's GLES state.
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    PresentationTime presentationTime = nullptr;
    struct Target {
        AHardwareBuffer *buffer = nullptr;  // acquired, so the address keeps naming this buffer
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint texture = 0;
        GLuint framebuffer = 0;
    };
    std::vector<Target> targets;

    static void OnImage(void *context, AImageReader *reader);
    void Run();
    bool SetUpGl(std::string *why);
    void ClearTargets();
    void TearDownGl();
    Target *TargetFor(AHardwareBuffer *buffer);
    bool Post(Image &image, std::string *why);
};

// The reader's callback thread: keeps the newest image, gives the one it replaces back.
void Feed::Impl::OnImage(void *context, AImageReader *reader) {
    auto *self = static_cast<Impl *>(context);
    Image image;
    if (AImageReader_acquireLatestImageAsync(reader, &image.image, &image.fence) != AMEDIA_OK || !image.image) return;
    Image replaced;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        ++self->stats.rendered;
        if (self->stopping) {
            replaced = image;
        } else {
            if (self->pending.image) {
                replaced = self->pending;
                ++self->stats.dropped;
            }
            self->pending = image;
        }
    }
    self->wake.notify_one();
    Release(replaced);
}

bool Feed::Impl::SetUpGl(std::string *why) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
        *why = "no EGL display";
        return false;
    }
    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RECORDABLE_ANDROID, EGL_TRUE,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, configAttributes, &config, 1, &count) || count < 1) {
        *why = "no recordable EGL config";
        return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
        *why = "eglCreateContext failed";
        return false;
    }
    surface = eglCreateWindowSurface(display, config, encoder, nullptr);
    if (surface == EGL_NO_SURFACE) {
        *why = "cannot attach EGL to the encoder's input Surface";
        return false;
    }
    if (!eglMakeCurrent(display, surface, surface, context)) {
        *why = "eglMakeCurrent failed";
        return false;
    }
    // Asynchronous queueing: a newer frame may replace one the codec has not taken yet.
    eglSwapInterval(display, 0);
    presentationTime = reinterpret_cast<PresentationTime>(eglGetProcAddress("eglPresentationTimeANDROID"));
    return true;
}

void Feed::Impl::ClearTargets() {
    for (Target &target : targets) {
        if (target.framebuffer) glDeleteFramebuffers(1, &target.framebuffer);
        if (target.texture) glDeleteTextures(1, &target.texture);
        if (target.image != EGL_NO_IMAGE_KHR) eglDestroyImageKHR(display, target.image);
        if (target.buffer) AHardwareBuffer_release(target.buffer);
    }
    targets.clear();
}

void Feed::Impl::TearDownGl() {
    if (display == EGL_NO_DISPLAY) return;
    if (context != EGL_NO_CONTEXT && eglGetCurrentContext() == context) ClearTargets();
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
    eglReleaseThread();
    // No eglTerminate: the default display belongs to the whole process.
    surface = EGL_NO_SURFACE;
    context = EGL_NO_CONTEXT;
    display = EGL_NO_DISPLAY;
}

Feed::Impl::Target *Feed::Impl::TargetFor(AHardwareBuffer *buffer) {
    for (Target &target : targets) {
        if (target.buffer == buffer) return &target;
    }
    if (targets.size() >= kMaxTargets) ClearTargets();
    Target target;
    EGLClientBuffer client = eglGetNativeClientBufferANDROID(buffer);
    if (!client) return nullptr;
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    target.image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, client, attributes);
    if (target.image == EGL_NO_IMAGE_KHR) return nullptr;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(target.image));
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    AHardwareBuffer_acquire(buffer);
    target.buffer = buffer;
    targets.push_back(target);
    if (complete) return &targets.back();
    ClearTargets();
    return nullptr;
}

bool Feed::Impl::Post(Image &image, std::string *why) {
    if (image.fence >= 0) {
        pollfd fence{image.fence, POLLIN, 0};
        if (poll(&fence, 1, kFenceTimeoutMs) <= 0) {
            *why = "HWUI's frame did not finish";
            return false;
        }
        close(image.fence);
        image.fence = -1;
    }
    AHardwareBuffer *buffer = nullptr;
    if (AImage_getHardwareBuffer(image.image, &buffer) != AMEDIA_OK || !buffer) {
        *why = "the window image has no hardware buffer";
        return false;
    }
    Target *target = TargetFor(buffer);
    if (!target) {
        *why = "cannot import the window image into GLES";
        return false;
    }
    const GLint w = static_cast<GLint>(width);
    const GLint h = static_cast<GLint>(height);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, target->framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    // The image's first row is its top; the window surface's first GL row is its bottom.
    glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // The image goes back to the reader after this; the blit must have read it.
    glFinish();
    if (presentationTime) presentationTime(display, surface, static_cast<EGLnsecsANDROID>(NowNs()));
    if (!eglSwapBuffers(display, surface)) {
        char text[64];
        snprintf(text, sizeof(text), "eglSwapBuffers into the encoder failed (0x%x)", eglGetError());
        *why = text;
        return false;
    }
    return true;
}

void Feed::Impl::Run() {
    pthread_setname_np(pthread_self(), "XRGame:PanelEnc");
    std::string why;
    const bool ready = SetUpGl(&why);
    if (!ready) {
        LOGE("capture: 2D window feed: %s", why.c_str());
        std::lock_guard<std::mutex> lock(mutex);
        error = why;
    }
    while (true) {
        Image image;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this] { return stopping || pending.image != nullptr; });
            if (stopping) break;
            image = pending;
            pending = {};
            postStartNs = NowNs();
        }
        const bool posted = ready && Post(image, &why);
        Release(image);
        std::lock_guard<std::mutex> lock(mutex);
        postStartNs = 0;
        if (posted) {
            ++stats.fed;
        } else if (ready && !stopping && error.empty()) {
            // Stopping abandons the encoder's Surface under a running post: not an error then.
            error = why;
            LOGE("capture: 2D window feed: %s", why.c_str());
        }
    }
    TearDownGl();
}

Feed::Feed() : impl_(std::make_unique<Impl>()) {}

Feed::~Feed() {
    Stop();
    if (impl_->reader) {
        AImageReader_setImageListener(impl_->reader, nullptr);
        AImageReader_delete(impl_->reader);  // and the images still acquired, and its window
    }
}

bool Feed::Start(ANativeWindow *encoder, uint32_t width, uint32_t height, std::string *error) {
    Impl &p = *impl_;
    if (p.reader || !encoder || width == 0 || height == 0) {
        *error = "2D window feed already started or without an encoder";
        return false;
    }
    if (AImageReader_newWithUsage(static_cast<int32_t>(width), static_cast<int32_t>(height), AIMAGE_FORMAT_RGBA_8888,
                                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT,
                                  kReaderImages, &p.reader) != AMEDIA_OK) {
        p.reader = nullptr;
        *error = "cannot create the 2D window image reader";
        return false;
    }
    AImageReader_ImageListener listener{&p, &Impl::OnImage};
    AImageReader_getWindow(p.reader, &p.window);
    p.encoder = encoder;
    p.width = width;
    p.height = height;
    p.stopping = false;
    p.stats = {};
    p.error.clear();
    AImageReader_setImageListener(p.reader, &listener);
    p.thread = std::thread([&p] { p.Run(); });
    return true;
}

ANativeWindow *Feed::Window() const { return impl_->window; }

void Feed::RequestStop() {
    Image pending;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
        pending = impl_->pending;
        impl_->pending = {};
    }
    impl_->wake.notify_all();
    Release(pending);
}

void Feed::Stop() {
    RequestStop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

FeedStats Feed::Stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    FeedStats stats = impl_->stats;
    stats.busyNs = impl_->postStartNs != 0 ? NowNs() - impl_->postStartNs : 0;
    return stats;
}

std::string Feed::Error() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->error;
}

}  // namespace xrimmersive::capture::panel

#endif  // XRGAME_CAPTURE
