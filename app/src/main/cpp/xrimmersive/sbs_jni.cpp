#include "xr_windows_projection.h"
#include <jni.h>
#include <memory>
#include <unordered_map>

// All calls are made on the owning render thread with its EGL context current.
namespace {
struct SbsPresenter {
    xrimmersive::windowsvr::WindowsFrameTransport transport;
    xrimmersive::windowsvr::WindowsProjectionPresenter presenter;
};
thread_local std::unordered_map<jlong, std::unique_ptr<SbsPresenter>> presenters;
thread_local jlong nextHandle = 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_app_gamenative_ui_screen_xr_sbs_SbsNative_create(JNIEnv* env, jobject, jstring endpoint) {
    if (!endpoint || eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
    const char* path = env->GetStringUTFChars(endpoint, nullptr);
    if (!path) return 0;
    auto instance = std::make_unique<SbsPresenter>();
    const bool ready = instance->presenter.initializeSbs(eglGetCurrentDisplay());
    if (ready) instance->transport.start(path);
    env->ReleaseStringUTFChars(endpoint, path);
    if (!ready) {
        instance->presenter.shutdown();
        return 0;
    }
    const jlong handle = ++nextHandle;
    presenters.emplace(handle, std::move(instance));
    return handle;
}

extern "C" JNIEXPORT jlong JNICALL
Java_app_gamenative_ui_screen_xr_sbs_SbsNative_draw(JNIEnv*, jobject, jlong handle, jint w, jint h) {
    const auto found = presenters.find(handle);
    if (found == presenters.end() || w < 2 || h < 1) return -1;
    auto& instance = *found->second;
    instance.presenter.renderSbs(instance.transport, w, h);
    return static_cast<jlong>(instance.presenter.sbsFrameCount());
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_ui_screen_xr_sbs_SbsNative_destroy(JNIEnv*, jobject, jlong handle) {
    const auto found = presenters.find(handle);
    if (found == presenters.end()) return;
    // Drain our own GPU work before releasing imported images / transport references.
    glFinish();
    found->second->presenter.shutdown();
    found->second->transport.stop();
    presenters.erase(found);
}
