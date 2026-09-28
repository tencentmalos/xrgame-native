// SPDX-License-Identifier: GPL-3.0-or-later
#include <spatial/core/utils/LiteTrace.h>
#include <jni.h>
#include <time.h>
#include <string>

using spatial::LiteTrace;
using spatial::ProfilerRing;
namespace {
std::string text(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xrgame_LitepProfiler_initialize(JNIEnv* env, jobject, jstring directory, jstring sha) {
    // Opt in to zero-frame startup capture. Disabled until an explicit debug command.
    ProfilerRing::Initialize("XRGameNative", text(env, directory), false, 1024 * 1024, true);
    timespec boot{}, mono{};
    clock_gettime(CLOCK_BOOTTIME, &boot);
    const auto trace_ns = LiteTrace::timeNs();
    clock_gettime(CLOCK_MONOTONIC, &mono);
    ProfilerRing::SetAppInfo("scope=android-host;frame_source=x11-present-request;foundation="
        XRGAME_FOUNDATION_REVISION ";catalog=" + text(env, sha) +
        ";clock_boot_ns=" + std::to_string(boot.tv_sec * 1000000000LL + boot.tv_nsec) +
        ";clock_trace_ns=" + std::to_string(trace_ns) +
        ";clock_mono_ns=" + std::to_string(mono.tv_sec * 1000000000LL + mono.tv_nsec));
    LiteTrace::trackDef(1, "XRGame startup (elapsed, not CPU time)");
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_app_gamenative_xrgame_LitepProfiler_begin(JNIEnv* env, jobject, jstring name) {
    if (!ProfilerRing::Enabled()) return nullptr;
    const jlong token[]{static_cast<jlong>(LiteTrace::regionCookieCreate()),
                        static_cast<jlong>(ProfilerRing::Generation())};
    auto array = env->NewLongArray(2);
    if (!array) return nullptr;
    env->SetLongArrayRegion(array, 0, 2, token);
    LiteTrace::regionBegin(text(env, name).c_str(), token[0], 1);
    return array;
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xrgame_LitepProfiler_end(JNIEnv* env, jobject, jlongArray array) {
    if (!array || env->GetArrayLength(array) != 2) return;
    jlong token[2];
    env->GetLongArrayRegion(array, 0, 2, token);
    if (static_cast<std::uint64_t>(token[1]) == ProfilerRing::Generation()) LiteTrace::regionEnd(token[0]);
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xrgame_LitepProfiler_bookmark(JNIEnv* env, jobject, jstring name) {
    if (ProfilerRing::Enabled()) LiteTrace::bookmark(text(env, name).c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xrgame_LitepProfiler_frame(JNIEnv*, jobject) { ProfilerRing::FrameMark(); }
