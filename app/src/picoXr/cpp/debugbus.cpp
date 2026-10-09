// SPDX-License-Identifier: GPL-3.0-or-later
// Android host adapter. Foundation owns command dispatch; the Service owns transport/lifetime.
#include <spatial/debugbus/DebugCommandRegistry.h>
#include <spatial/debugbus/profiler/ProfilerRingCommands.h>
#include <jni.h>
#include <link.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include "profiler_core.h"

namespace {
constexpr size_t max_response = 128 * 1024;
std::string text(JNIEnv* env, jstring value) {
    if (!value) throw std::runtime_error("null string");
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) throw std::runtime_error("string allocation");
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}
std::string quote(const char* value) {
    std::string result = "\"";
    for (const unsigned char* c = reinterpret_cast<const unsigned char*>(value); *c; ++c) {
        if (*c == '\"' || *c == '\\') result += '\\';
        if (*c < 32 || *c > 126) result += '?';
        else result += static_cast<char>(*c);
    }
    return result + '\"';
}
std::string build_id(const dl_phdr_info& info) {
    constexpr char hex[] = "0123456789abcdef";
    for (size_t p = 0; p < info.dlpi_phnum; ++p) {
        const auto& ph = info.dlpi_phdr[p];
        if (ph.p_type != PT_NOTE || ph.p_memsz > 1024 * 1024) continue;
        bool readable = false;
        for (size_t n = 0; n < info.dlpi_phnum; ++n) {
            const auto& load = info.dlpi_phdr[n];
            if (load.p_type == PT_LOAD && (load.p_flags & PF_R) && ph.p_vaddr >= load.p_vaddr &&
                ph.p_vaddr - load.p_vaddr <= load.p_memsz &&
                ph.p_memsz <= load.p_memsz - (ph.p_vaddr - load.p_vaddr)) readable = true;
        }
        if (!readable) continue;
        const auto* bytes = reinterpret_cast<const unsigned char*>(info.dlpi_addr + ph.p_vaddr);
        size_t offset = 0;
        while (offset + sizeof(ElfW(Nhdr)) <= ph.p_memsz) {
            ElfW(Nhdr) note;
            std::memcpy(&note, bytes + offset, sizeof(note));
            const size_t name = offset + sizeof(note);
            const size_t desc = name + ((size_t{note.n_namesz} + 3) & ~size_t{3});
            const size_t end = desc + ((size_t{note.n_descsz} + 3) & ~size_t{3});
            if (end > ph.p_memsz) break;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(bytes + name, "GNU\0", 4) == 0 && note.n_descsz <= 64) {
                std::string id;
                for (size_t n = 0; n < note.n_descsz; ++n) {
                    id += hex[bytes[desc+n] >> 4]; id += hex[bytes[desc+n] & 15];
                }
                return id;
            }
            offset = end;
        }
    }
    return {};
}
std::string modules(const std::string& filter) {
    struct State { std::ostringstream out; size_t count = 0; bool truncated = false; std::string filter; } state;
    state.filter = filter;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* opaque) {
        auto& s = *static_cast<State*>(opaque);
        const char* base = std::strrchr(info->dlpi_name, '/');
        if (!s.filter.empty() && s.filter != (base ? base + 1 : info->dlpi_name)) return 0;
        if (s.count == 256) { s.truncated = true; return 1; }
        if (s.count++) s.out << ',';
        s.out << "{\"name\":" << quote(base ? base + 1 : info->dlpi_name)
              << ",\"base\":\"0x" << std::hex << info->dlpi_addr << std::dec
              << "\",\"buildId\":\"" << build_id(*info) << "\"}";
        return 0;
    }, &state);
    return "{\"schema\":1,\"scope\":\"android-host-elf\",\"modules\":[" + state.out.str() +
           "],\"truncated\":" + (state.truncated ? "true}" : "false}");
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_app_gamenative_xrgame_DebugBusService_execute(JNIEnv* env, jobject provider,
                                                  jstring request, jobjectArray arguments) {
    try {
        auto name = text(env, request);
        const auto count = env->GetArrayLength(arguments);
        if (name.size() > 64 || count > 4) return env->NewStringUTF("{\"error\":\"request_limit\"}\n");
        std::vector<std::string> args;
        for (jsize i = 0; i < count; ++i) {
            auto item = static_cast<jstring>(env->GetObjectArrayElement(arguments, i));
            auto arg = text(env, item);
            env->DeleteLocalRef(item);
            if (arg.size() > 64) return env->NewStringUTF("{\"error\":\"argument_limit\"}\n");
            args.push_back(std::move(arg));
        }
        const auto cls = env->GetObjectClass(provider);
        const auto query = env->GetMethodID(cls, "query", "(Ljava/lang/String;[Ljava/lang/String;)Ljava/lang/String;");
        if (!query) return nullptr; // Preserve the JNI exception.
        // Request-scoped captures cannot outlive the JNI call. No global JNI refs, registry
        // rebinding race or Foundation worker threads are needed for Android dumpsys.
        spatial::debugbus::DebugCommandRegistry registry;
        spatial::debugbus::profiler::RegisterProfilerRingCommands(registry);
        registry.Register("instrumentation", "Host probes: instrumentation [off|coarse|detail]", xrgameProfileCommand);
        registry.Register("bridge", "Foundation revision and transport", [](const auto& a) {
            if (!a.empty()) return std::string("{\"error\":\"unexpected_arguments\"}");
            return std::string("{\"schema\":1,\"foundation\":\"") + XRGAME_FOUNDATION_REVISION +
                "\",\"transport\":\"android-dumpsys\",\"scope\":\"host\",\"pid\":" +
                std::to_string(getpid()) + "}";
        });
        registry.Register("modules", "Loaded host ELF Build IDs; modules [exact-basename]", [](const auto& a) {
            return a.size() <= 1 ? modules(a.empty() ? "" : a[0]) : "{\"error\":\"unexpected_arguments\"}";
        });
        for (const auto* command : {"status", "runtime", "processes", "present", "api_capture", "vr_tuning", "vr_upscale", "vr_grip"}) {
            registry.Register(command, command == std::string_view("present") ?
                "Host Present state; present trace <0..3600>" : command == std::string_view("api_capture") ?
                "GFXReconstruct capture; api_capture [status|start|stop] [container]" :
                command == std::string_view("vr_tuning") ?
                "Windows VR pacing; vr_tuning [pacing=off|auto|half] [start=<us>] [predict=0|1]" :
                command == std::string_view("vr_upscale") ?
                "Windows VR reconstruction; vr_upscale [filter=off|fsr1|sgsr] [sharp=0..100] [fov=off|fixed|eye] "
                "[level=low|balanced|high] [out=50..100] [debug=0|1]" :
                command == std::string_view("vr_grip") ?
                "Windows VR controller grip correction; vr_grip [pitch=<deg>] [yaw=<deg>] [roll=<deg>] "
                "[x=<mm>] [y=<mm>] [z=<mm>] [reset=1]" :
                "Host snapshot (JSON schema 1)",
                [&, command](const auto&) {
                    auto key = env->NewStringUTF(command);
                    auto result = static_cast<jstring>(env->CallObjectMethod(provider, query, key, arguments));
                    env->DeleteLocalRef(key);
                    if (env->ExceptionCheck()) return std::string{};
                    auto response = text(env, result);
                    env->DeleteLocalRef(result);
                    return response;
                });
        }
        auto result = registry.Handle(name, args);
        env->DeleteLocalRef(cls);
        if (env->ExceptionCheck()) return nullptr;
        if (result.size() > max_response) result = "{\"error\":\"response_limit\"}";
        return env->NewStringUTF((result + '\n').c_str());
    } catch (const std::exception&) {
        if (env->ExceptionCheck()) return nullptr;
        return env->NewStringUTF("{\"error\":\"native_dispatch_failed\"}\n");
    }
}
