/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XRGame guest patch installer (xrgame_guest_patch64.dll).
 *
 * The bundled Steam client loader injects this DLL into a Windows x64 game before
 * the game's main thread runs (ColdClientLoader DllsToInjectFolder). It installs the
 * packages the launcher copied into %XRGAME_GUEST_PATCH_DIR%\packages (patch.json,
 * schema xrgame.guest-patch.v1, built by tools/xrgame/guest-patch/build.py) into their
 * target module: modules already loaded at once, later DLLs from the loader's
 * notification, before their entry point runs.
 *
 * The install follows shadPS4's BuildPlan (src/core/host_runtime/guest_patch_format.cpp)
 * and desktop loader (src/core/guest_patch_desktop.cpp): payload and a pool of stubs and
 * dispatch slots near the module, original-function trampolines, then the entry jumps.
 * The trampolines come from relocation templates computed when the package was built,
 * so no disassembler runs here. No CRT: kernel32, ntdll and bcrypt imports only.
 *
 * Files in %XRGAME_GUEST_PATCH_DIR%: packages\*.json (installed in name order),
 * status.txt (rewritten on every change), control.txt ("<seq> enable|disable [name]",
 * polled), guest-patch.log (install events and SDK log tags), sha-cache.txt. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;
typedef int s32;
typedef __WCHAR_TYPE__ wchar;
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* HANDLE;
typedef __SIZE_TYPE__ usize;

#define IMPORT __declspec(dllimport)
#define EXPORT __declspec(dllexport)
#define INVALID_HANDLE ((HANDLE)(s64)-1)

typedef struct { DWORD lo, hi; } FILETIME;
typedef struct {
    DWORD attributes;
    FILETIME created, accessed, written;
    DWORD size_high, size_low, reserved0, reserved1;
    wchar name[260];
    wchar alternate[14];
} FIND_DATA;
typedef struct {
    void* base;
    void* allocation_base;
    DWORD allocation_protect;
    u16 partition;
    u64 region_size;
    DWORD state, protect, type;
} MEMORY_INFO;
typedef struct {
    u16 architecture, reserved;
    DWORD page_size;
    void *min_address, *max_address;
    u64 processor_mask;
    DWORD processors, processor_type, granularity;
    u16 level, revision;
} SYSTEM_INFO;
typedef struct {
    DWORD attributes;
    FILETIME created, accessed, written;
    DWORD size_high, size_low;
} FILE_ATTRIBUTES; /* WIN32_FILE_ATTRIBUTE_DATA */
typedef struct { u16 length, maximum; wchar* buffer; } UNICODE_STRING;
typedef struct {
    DWORD flags;
    const UNICODE_STRING* full_name;
    const UNICODE_STRING* base_name;
    void* base;
    DWORD size;
} DLL_NOTIFICATION;
typedef void (*DLL_NOTIFICATION_FUNCTION)(DWORD reason, const DLL_NOTIFICATION* data, void* context);

IMPORT HANDLE GetProcessHeap(void);
IMPORT void* HeapAlloc(HANDLE heap, DWORD flags, usize size);
IMPORT void* HeapReAlloc(HANDLE heap, DWORD flags, void* memory, usize size);
IMPORT BOOL HeapFree(HANDLE heap, DWORD flags, void* memory);
IMPORT DWORD GetEnvironmentVariableW(const wchar* name, wchar* buffer, DWORD size);
IMPORT HANDLE CreateFileW(const wchar* name, DWORD access, DWORD share, void* security,
                          DWORD disposition, DWORD flags, HANDLE template_file);
IMPORT BOOL ReadFile(HANDLE file, void* buffer, DWORD size, DWORD* read, void* overlapped);
IMPORT BOOL WriteFile(HANDLE file, const void* buffer, DWORD size, DWORD* written, void* overlapped);
IMPORT BOOL CloseHandle(HANDLE handle);
IMPORT BOOL GetFileSizeEx(HANDLE file, s64* size);
IMPORT BOOL GetFileTime(HANDLE file, FILETIME* created, FILETIME* accessed, FILETIME* written);
IMPORT BOOL SetFilePointerEx(HANDLE file, s64 distance, s64* position, DWORD method);
IMPORT HANDLE FindFirstFileW(const wchar* pattern, FIND_DATA* data);
IMPORT BOOL FindNextFileW(HANDLE find, FIND_DATA* data);
IMPORT BOOL FindClose(HANDLE find);
IMPORT BOOL GetFileAttributesExW(const wchar* name, int level, void* information);
IMPORT void* VirtualAlloc(void* address, usize size, DWORD type, DWORD protect);
IMPORT BOOL VirtualFree(void* address, usize size, DWORD type);
IMPORT BOOL VirtualProtect(void* address, usize size, DWORD protect, DWORD* old);
IMPORT usize VirtualQuery(const void* address, MEMORY_INFO* info, usize size);
IMPORT BOOL FlushInstructionCache(HANDLE process, const void* address, usize size);
IMPORT HANDLE GetCurrentProcess(void);
IMPORT HANDLE GetModuleHandleW(const wchar* name);
IMPORT DWORD GetModuleFileNameW(HANDLE module, wchar* name, DWORD size);
IMPORT HANDLE CreateThread(void* security, usize stack, DWORD (*start)(void*), void* parameter,
                           DWORD flags, DWORD* id);
IMPORT void Sleep(DWORD milliseconds);
IMPORT BOOL QueryPerformanceCounter(s64* count);
IMPORT BOOL QueryPerformanceFrequency(s64* frequency);
IMPORT DWORD GetCurrentThreadId(void);
IMPORT DWORD GetCurrentProcessId(void);
IMPORT BOOL MoveFileExW(const wchar* from, const wchar* to, DWORD flags);
IMPORT void GetSystemInfo(SYSTEM_INFO* info);
IMPORT void AcquireSRWLockExclusive(void** lock);
IMPORT void ReleaseSRWLockExclusive(void** lock);
IMPORT void OutputDebugStringA(const char* text);
IMPORT BOOL DisableThreadLibraryCalls(HANDLE module);
IMPORT long LdrRegisterDllNotification(DWORD flags, DLL_NOTIFICATION_FUNCTION callback, void* context,
                                       void** cookie);
IMPORT long BCryptOpenAlgorithmProvider(void** algorithm, const wchar* id, const wchar* implementation,
                                        DWORD flags);
IMPORT long BCryptCreateHash(void* algorithm, void** hash, u8* object, DWORD object_size, u8* secret,
                             DWORD secret_size, DWORD flags);
IMPORT long BCryptHashData(void* hash, u8* input, DWORD size, DWORD flags);
IMPORT long BCryptFinishHash(void* hash, u8* output, DWORD size, DWORD flags);
IMPORT long BCryptDestroyHash(void* hash);

#define GENERIC_READ 0x80000000UL
#define GENERIC_WRITE 0x40000000UL
#define FILE_SHARE_ALL 7UL
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define OPEN_ALWAYS 4
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_FREE 0x10000
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define MOVEFILE_REPLACE_EXISTING 1
#define DLL_PROCESS_ATTACH 1
#define LDR_DLL_NOTIFICATION_REASON_LOADED 1

/* ---- memory and strings ---------------------------------------------------------- */

void* memcpy(void* destination, const void* source, usize size) {
    u8* d = destination;
    const u8* s = source;
    for (usize i = 0; i < size; ++i) d[i] = s[i];
    return destination;
}
void* memmove(void* destination, const void* source, usize size) {
    u8* d = destination;
    const u8* s = source;
    if (d < s) return memcpy(destination, source, size);
    for (usize i = size; i; --i) d[i - 1] = s[i - 1];
    return destination;
}
void* memset(void* destination, int value, usize size) {
    u8* d = destination;
    for (usize i = 0; i < size; ++i) d[i] = (u8)value;
    return destination;
}
int memcmp(const void* a, const void* b, usize size) {
    const u8 *x = a, *y = b;
    for (usize i = 0; i < size; ++i)
        if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
static usize length(const char* s) {
    usize n = 0;
    while (s[n]) ++n;
    return n;
}
static usize wlength(const wchar* s) {
    usize n = 0;
    while (s[n]) ++n;
    return n;
}
static int equal(const char* a, const char* b) {
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}
static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}
/* An ASCII-case-insensitive comparison of a wide module name with a UTF-8 one. */
static int same_name(const wchar* wide, usize wide_length, const char* name) {
    usize n = length(name);
    if (n != wide_length) return 0;
    for (usize i = 0; i < n; ++i)
        if (lower(wide[i]) != lower((u8)name[i])) return 0;
    return 1;
}

static HANDLE heap;
static void* allocate(usize size) {
    return HeapAlloc(heap, 8 /* HEAP_ZERO_MEMORY */, size ? size : 1);
}
static void release(void* memory) {
    if (memory) HeapFree(heap, 0, memory);
}

/* A growable text buffer. */
typedef struct { char* data; usize size, capacity; } Text;
static void text_append(Text* t, const char* s, usize n) {
    if (t->size + n + 1 > t->capacity) {
        usize capacity = t->capacity ? t->capacity * 2 : 4096;
        while (capacity < t->size + n + 1) capacity *= 2;
        char* data = t->data ? HeapReAlloc(heap, 8, t->data, capacity) : allocate(capacity);
        if (!data) return;
        t->data = data;
        t->capacity = capacity;
    }
    memcpy(t->data + t->size, s, n);
    t->size += n;
    t->data[t->size] = 0;
}
static void put(Text* t, const char* s) {
    text_append(t, s, length(s));
}
static void put_u64(Text* t, u64 value) {
    char digits[24];
    int n = 0;
    do digits[n++] = (char)('0' + value % 10), value /= 10; while (value);
    char out[24];
    for (int i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    text_append(t, out, n);
}
static void put_s64(Text* t, s64 value) {
    if (value < 0) put(t, "-"), value = -value;
    put_u64(t, (u64)value);
}
static void put_hex(Text* t, u64 value) {
    char out[18] = "0x";
    int n = 2;
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        int digit = (int)(value >> shift) & 15;
        if (digit || started || shift == 0) out[n++] = "0123456789abcdef"[digit], started = 1;
    }
    text_append(t, out, n);
}
/* Wide path to UTF-8 (paths here are ASCII in practice; others become '?'). */
static void put_wide(Text* t, const wchar* s, usize n) {
    for (usize i = 0; i < n; ++i) {
        char c = s[i] < 128 ? (char)s[i] : '?';
        text_append(t, &c, 1);
    }
}

/* ---- files ------------------------------------------------------------------------ */

static wchar directory[400];
static usize directory_length;

static void path_in_directory(wchar* out, const wchar* name) {
    usize n = directory_length;
    memcpy(out, directory, n * sizeof(wchar));
    out[n++] = '\\';
    for (usize i = 0; name[i] && n < 510; ++i) out[n++] = name[i];
    out[n] = 0;
}
static void write_file(const wchar* name, const char* data, usize size, int append) {
    wchar path[512];
    path_in_directory(path, name);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_ALL, 0, append ? OPEN_ALWAYS : CREATE_ALWAYS, 0x80, 0);
    if (file == INVALID_HANDLE) return;
    if (append) SetFilePointerEx(file, 0, 0, 2 /* FILE_END */);
    DWORD written;
    WriteFile(file, data, (DWORD)size, &written, 0);
    CloseHandle(file);
}
/* Whole file, NUL-terminated; *size excludes the terminator. */
static char* read_file(const wchar* path, usize limit, usize* size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_ALL, 0, OPEN_EXISTING, 0x80, 0);
    if (file == INVALID_HANDLE) return 0;
    s64 total = 0;
    char* data = 0;
    if (GetFileSizeEx(file, &total) && total >= 0 && (u64)total <= limit) {
        data = allocate((usize)total + 1);
        DWORD got = 0;
        if (data && (total == 0 || (ReadFile(file, data, (DWORD)total, &got, 0) && got == (DWORD)total))) {
            *size = (usize)total;
        } else {
            release(data);
            data = 0;
        }
    }
    CloseHandle(file);
    return data;
}

static void* log_lock;
static void log_line(const char* a, const char* b, const char* c) {
    Text t = {0};
    put(&t, "[xrgame-guest-patch] ");
    put(&t, a);
    if (b) put(&t, b);
    if (c) put(&t, c);
    put(&t, "\n");
    if (!t.data) return;
    OutputDebugStringA(t.data);
    AcquireSRWLockExclusive(&log_lock);
    write_file(L"guest-patch.log", t.data, t.size, 1);
    ReleaseSRWLockExclusive(&log_lock);
    release(t.data);
}

/* ---- SHA-256 through bcrypt -------------------------------------------------------- */

static void hex_digest(const u8 digest[32], char out[65]) {
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = "0123456789abcdef"[digest[i] >> 4];
        out[2 * i + 1] = "0123456789abcdef"[digest[i] & 15];
    }
    out[64] = 0;
}
static void* sha256_algorithm;
static int sha256_begin(void** hash) {
    if (!sha256_algorithm && BCryptOpenAlgorithmProvider(&sha256_algorithm, L"SHA256", 0, 0)) return 0;
    return BCryptCreateHash(sha256_algorithm, hash, 0, 0, 0, 0, 0) == 0;
}
static int sha256_bytes(const u8* data, usize size, char out[65]) {
    void* hash;
    u8 digest[32];
    if (!sha256_begin(&hash)) return 0;
    int ok = BCryptHashData(hash, (u8*)data, (DWORD)size, 0) == 0 && BCryptFinishHash(hash, digest, 32, 0) == 0;
    BCryptDestroyHash(hash);
    if (ok) hex_digest(digest, out);
    return ok;
}

/* SHA-256 of a module file, cached in sha-cache.txt by path, size and write time:
 * hashing a large executable under emulation costs seconds. */
static int file_sha256(const wchar* path, char out[65]) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_ALL, 0, OPEN_EXISTING, 0x80, 0);
    if (file == INVALID_HANDLE) return 0;
    s64 size = 0;
    FILETIME written = {0};
    int ok = GetFileSizeEx(file, &size) && GetFileTime(file, 0, 0, &written);
    Text key = {0};
    put_u64(&key, (u64)size);
    put(&key, " ");
    put_u64(&key, ((u64)written.hi << 32) | written.lo);
    put(&key, " ");
    put_wide(&key, path, wlength(path));
    wchar cache_path[512];
    path_in_directory(cache_path, L"sha-cache.txt");
    usize cache_size = 0;
    char* cache = ok && key.data ? read_file(cache_path, 1 << 20, &cache_size) : 0;
    int cached = 0;
    for (char* line = cache; ok && line && *line && !cached;) {
        char* end = line;
        while (*end && *end != '\n') ++end;
        /* "<sha> <size> <time> <path>" */
        if ((usize)(end - line) == 65 + key.size && line[64] == ' ' &&
            !memcmp(line + 65, key.data, key.size)) {
            memcpy(out, line, 64);
            out[64] = 0;
            cached = 1;
        }
        line = *end ? end + 1 : end;
    }
    release(cache);
    if (ok && !cached) {
        void* hash;
        ok = sha256_begin(&hash);
        u8* buffer = ok ? allocate(1 << 20) : 0;
        ok = ok && buffer;
        for (;;) {
            DWORD got = 0;
            if (!ok || !ReadFile(file, buffer, 1 << 20, &got, 0)) {
                ok = 0;
                break;
            }
            if (!got) break;
            if (BCryptHashData(hash, buffer, got, 0)) ok = 0;
        }
        u8 digest[32];
        if (ok && BCryptFinishHash(hash, digest, 32, 0) == 0) {
            hex_digest(digest, out);
            Text line = {0};
            text_append(&line, out, 64);
            put(&line, " ");
            text_append(&line, key.data, key.size);
            put(&line, "\n");
            if (line.data) write_file(L"sha-cache.txt", line.data, line.size, 1);
            release(line.data);
        } else {
            ok = 0;
        }
        if (buffer) release(buffer);
        BCryptDestroyHash(hash);
    }
    release(key.data);
    CloseHandle(file);
    return ok;
}

/* ---- JSON ------------------------------------------------------------------------- */

enum { J_NULL, J_BOOL, J_NUMBER, J_STRING, J_ARRAY, J_OBJECT };
typedef struct Json {
    int type;
    u64 number; /* unsigned integers only: packages need nothing else */
    int boolean;
    char* string;
    usize string_length;
    struct Json* items; /* array elements or object values */
    char** keys;        /* object keys */
    usize count;
} Json;
typedef struct { const char* p; const char* end; const char* error; } Parser;

static void skip_space(Parser* ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\r' || *ps->p == '\t')) ++ps->p;
}
static int parse_value(Parser* ps, Json* out, int depth);
static int fail(Parser* ps, const char* why) {
    if (!ps->error) ps->error = why;
    return 0;
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int parse_string(Parser* ps, char** out, usize* out_length) {
    ++ps->p; /* opening quote */
    const char* start = ps->p;
    usize n = 0;
    while (ps->p < ps->end && *ps->p != '"') {
        if (*ps->p == '\\') ++ps->p;
        ++ps->p;
        ++n;
    }
    if (ps->p >= ps->end) return fail(ps, "unterminated string");
    char* s = allocate(n * 3 + 1);
    if (!s) return fail(ps, "out of memory");
    usize w = 0;
    for (const char* q = start; q < ps->p; ++q) {
        if ((u8)*q < 0x20) return fail(ps, "control character in string");
        if (*q != '\\') {
            s[w++] = *q;
            continue;
        }
        ++q;
        switch (*q) {
        case '"': case '\\': case '/': s[w++] = *q; break;
        case 'b': s[w++] = '\b'; break;
        case 'f': s[w++] = '\f'; break;
        case 'n': s[w++] = '\n'; break;
        case 'r': s[w++] = '\r'; break;
        case 't': s[w++] = '\t'; break;
        case 'u': {
            if (ps->p - q < 5) return fail(ps, "bad \\u escape");
            u32 cp = 0;
            for (int i = 1; i <= 4; ++i) {
                int d = hex_digit(q[i]);
                if (d < 0) return fail(ps, "bad \\u escape");
                cp = cp * 16 + (u32)d;
            }
            q += 4;
            if (cp == 0) return fail(ps, "NUL in string");
            if (cp < 0x80) s[w++] = (char)cp;
            else if (cp < 0x800) s[w++] = (char)(0xc0 | cp >> 6), s[w++] = (char)(0x80 | (cp & 63));
            else s[w++] = (char)(0xe0 | cp >> 12), s[w++] = (char)(0x80 | ((cp >> 6) & 63)),
                 s[w++] = (char)(0x80 | (cp & 63));
            break;
        }
        default: return fail(ps, "bad escape");
        }
    }
    ++ps->p; /* closing quote */
    s[w] = 0;
    *out = s;
    *out_length = w;
    return 1;
}
static int push(Parser* ps, Json* container, Json* value, char* key) {
    if ((container->count & (container->count - 1)) == 0) { /* 0, 1, 2, 4, ...: grow */
        usize capacity = container->count ? container->count * 2 : 1;
        Json* items = allocate(capacity * sizeof(Json));
        char** keys = container->type == J_OBJECT ? allocate(capacity * sizeof(char*)) : 0;
        if (!items || (container->type == J_OBJECT && !keys)) return fail(ps, "out of memory");
        if (container->count) {
            memcpy(items, container->items, container->count * sizeof(Json));
            if (keys) memcpy(keys, container->keys, container->count * sizeof(char*));
        }
        release(container->items);
        release(container->keys);
        container->items = items;
        container->keys = keys;
    }
    if (key) {
        for (usize i = 0; i < container->count; ++i)
            if (equal(container->keys[i], key)) return fail(ps, "duplicate key");
        container->keys[container->count] = key;
    }
    container->items[container->count++] = *value;
    return 1;
}
static int parse_value(Parser* ps, Json* out, int depth) {
    memset(out, 0, sizeof(*out));
    if (depth > 24) return fail(ps, "nesting too deep");
    skip_space(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    char c = *ps->p;
    if (c == '{' || c == '[') {
        int object = c == '{';
        out->type = object ? J_OBJECT : J_ARRAY;
        ++ps->p;
        skip_space(ps);
        if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) return ++ps->p, 1;
        for (;;) {
            char* key = 0;
            usize key_length;
            skip_space(ps);
            if (object) {
                if (ps->p >= ps->end || *ps->p != '"' || !parse_string(ps, &key, &key_length))
                    return fail(ps, "expected key");
                skip_space(ps);
                if (ps->p >= ps->end || *ps->p++ != ':') return fail(ps, "expected ':'");
            }
            Json value;
            if (!parse_value(ps, &value, depth + 1) || !push(ps, out, &value, key)) return 0;
            skip_space(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ++ps->p;
                continue;
            }
            if (ps->p < ps->end && *ps->p == (object ? '}' : ']')) return ++ps->p, 1;
            return fail(ps, "expected ',' or end of container");
        }
    }
    if (c == '"') {
        out->type = J_STRING;
        return parse_string(ps, &out->string, &out->string_length);
    }
    if (c >= '0' && c <= '9') {
        out->type = J_NUMBER;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
            u64 digit = (u64)(*ps->p++ - '0');
            if (out->number > (~0ULL - digit) / 10) return fail(ps, "number overflow");
            out->number = out->number * 10 + digit;
        }
        if (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E'))
            return fail(ps, "only unsigned integers are expected");
        return 1;
    }
    if (ps->end - ps->p >= 4 && !memcmp(ps->p, "true", 4)) return ps->p += 4, out->type = J_BOOL, out->boolean = 1, 1;
    if (ps->end - ps->p >= 5 && !memcmp(ps->p, "false", 5)) return ps->p += 5, out->type = J_BOOL, 1;
    if (ps->end - ps->p >= 4 && !memcmp(ps->p, "null", 4)) return ps->p += 4, out->type = J_NULL, 1;
    return fail(ps, c == '-' ? "only unsigned integers are expected" : "unexpected character");
}
static const Json* member(const Json* object, const char* key) {
    if (!object || object->type != J_OBJECT) return 0;
    for (usize i = 0; i < object->count; ++i)
        if (equal(object->keys[i], key)) return &object->items[i];
    return 0;
}

/* ---- package ---------------------------------------------------------------------- */

#define MAX_PACKAGES 8
#define MAX_HOOKS 64
#define STUB_STRIDE 512
#define SDK_OPERATIONS 4
static const char* const SDK_IMPORTS[SDK_OPERATIONS] = {"xrg_sdk_query", "xrg_sdk_clock_ns", "xrg_sdk_counter",
                                                        "xrg_sdk_log"};

typedef struct { u32 at, end; int kind; u64 target; } Fixup; /* kind: 0 rel32, 1 abs64, 2 abs64_local */
typedef struct {
    const char *name, *replacement, *original;
    u64 offset;
    u8* expected;
    usize expected_size;
    int site, replace;
    u8* trampoline;
    usize trampoline_size, stolen;
    Fixup* fixups;
    usize fixup_count;
    /* install plan */
    u64 entry, stub, original_address, slot, replacement_address;
} Hook;
typedef struct { const char* name; u64 offset; u8 *expected, *replacement; usize size; } CodePatch;
typedef struct {
    const char* name;
    int function, writable;
    u64 offset, size, alignment;
    u8* expected;
    usize expected_size;
} Binding;
typedef struct { const char* name; u64 slot; } Import;
typedef struct { u64 id; const char* name; volatile u64 count; volatile s64 last; } Tag;
typedef struct {
    wchar file[64];
    char sha256[65];
    const char *id, *title, *module, *module_sha256, *display_name;
    u8* segment[2];
    usize segment_size[2], segment_bytes[2], segment_count, image_size;
    const Json* exports;
    u64* rebase;
    usize rebase_count;
    Import* imports;
    usize import_count;
    Hook* hooks;
    usize hook_count;
    CodePatch* patches;
    usize patch_count;
    Binding* bindings;
    usize binding_count;
    Tag counters[64], logs[64];
    usize counter_count, log_count;
    /* state */
    int attempted, installed;
    char error[256];
    u64 module_base, image, pool, code_size, pool_size;
} Package;

static Package packages[MAX_PACKAGES];
static usize package_count;
static void* state_lock;
static char selection_error[256];
static volatile u64 sdk_calls;
static u64 switches, control_sequence;
static wchar app_id[24];

static void set_error(char* error, const char* a, const char* b) {
    usize n = 0;
    for (const char* s = a; s && *s && n < 250; ++s) error[n++] = *s;
    for (const char* s = b; s && *s && n < 250; ++s) error[n++] = *s;
    error[n] = 0;
}

static int is_symbol(const char* s) {
    if (!s || !*s || length(s) > 96) return 0;
    if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return 0;
    for (; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_'))
            return 0;
    return 1;
}
static const char* text_of(const Json* j, usize limit) {
    return j && j->type == J_STRING && j->string_length && j->string_length <= limit ? j->string : 0;
}
static int number_of(const Json* j, u64* out) {
    if (!j || j->type != J_NUMBER) return 0;
    *out = j->number;
    return 1;
}
static u8* unhex(const Json* j, usize limit, usize* size) {
    if (!j || j->type != J_STRING || j->string_length % 2 || j->string_length / 2 > limit) return 0;
    usize n = j->string_length / 2;
    u8* out = allocate(n);
    for (usize i = 0; out && i < 2 * n; ++i) {
        char c = j->string[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { /* lower-case hex only */
            release(out);
            return 0;
        }
        out[i / 2] = (u8)(out[i / 2] * 16 + hex_digit(c));
    }
    *size = n;
    return out;
}
static int is_digest(const char* s) {
    if (!s || length(s) != 64) return 0;
    for (; *s; ++s)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    return 1;
}
static int inside_segment(const Package* p, u64 offset, u64 size, int executable) {
    u64 base = 0;
    for (usize i = 0; i < p->segment_count; ++i) {
        if ((i == 0) == !!executable && offset >= base && offset - base <= p->segment_size[i] &&
            size <= p->segment_size[i] - (offset - base))
            return 1;
        base += p->segment_size[i];
    }
    return 0;
}
static int is_sdk_import(const char* name) {
    for (int i = 0; i < SDK_OPERATIONS; ++i)
        if (equal(name, SDK_IMPORTS[i])) return 1;
    return 0;
}

#define REQUIRE(condition, why) \
    do { \
        if (!(condition)) { \
            set_error(p->error, why, 0); \
            return 0; \
        } \
    } while (0)

/* Port of shadPS4 Package::Load for schema xrgame.guest-patch.v1. */
static int load_package(Package* p, const Json* j) {
    const char* schema = text_of(member(j, "schema"), 64);
    const char* abi = text_of(member(j, "abi"), 32);
    u64 version = 0;
    REQUIRE(schema && equal(schema, "xrgame.guest-patch.v1") && abi && equal(abi, "x86_64-win64") &&
                number_of(member(j, "sdk_version"), &version) && version == 1,
            "unsupported package format/ABI/SDK");
    p->id = text_of(member(j, "id"), 96);
    REQUIRE(is_symbol(p->id), "invalid package id");
    p->title = text_of(member(j, "title"), 16);
    REQUIRE(p->title, "invalid title");
    p->module = text_of(member(j, "module"), 128);
    REQUIRE(p->module, "invalid module");
    for (const char* s = p->module; *s; ++s) REQUIRE(*s != '\\' && *s != '/' && *s != ':', "module must be a file name");
    p->module_sha256 = text_of(member(j, "module_sha256"), 64);
    REQUIRE(is_digest(p->module_sha256), "invalid module SHA256");
    p->display_name = text_of(member(j, "name"), 96);
    if (!p->display_name) p->display_name = p->id;

    const Json* segments = member(j, "segments");
    REQUIRE(segments && segments->type == J_ARRAY && segments->count >= 1 && segments->count <= 2,
            "invalid segment count");
    for (usize i = 0; i < segments->count; ++i) {
        const Json* s = &segments->items[i];
        u64 offset, size;
        const Json* executable = member(s, "executable");
        REQUIRE(number_of(member(s, "offset"), &offset) && number_of(member(s, "size"), &size) && executable &&
                    executable->type == J_BOOL,
                "invalid segment");
        usize bytes = 0;
        u8* data = unhex(member(s, "hex"), 1 << 20, &bytes);
        REQUIRE(data && size && offset % 4096 == 0 && size % 4096 == 0 && offset <= (1 << 20) &&
                    size <= (1 << 20) - offset && bytes <= size && offset == p->image_size &&
                    executable->boolean == (i == 0),
                "invalid segment bounds/order (RX then optional RW)");
        char digest[65];
        const char* expected = text_of(member(s, "sha256"), 64);
        REQUIRE(sha256_bytes(data, bytes, digest) && expected && equal(digest, expected),
                "payload segment SHA mismatch");
        p->segment[i] = data;
        p->segment_bytes[i] = bytes;
        p->segment_size[i] = size;
        p->image_size = offset + size;
        p->segment_count = i + 1;
    }

    p->exports = member(j, "exports");
    REQUIRE(p->exports && p->exports->type == J_OBJECT && p->exports->count <= 128, "invalid exports");
    for (usize i = 0; i < p->exports->count; ++i) {
        u64 offset;
        REQUIRE(is_symbol(p->exports->keys[i]) && number_of(&p->exports->items[i], &offset) &&
                    inside_segment(p, offset, 1, 1),
                "export outside RX");
    }

    const Json* rebase = member(j, "rebase64");
    REQUIRE(rebase && rebase->type == J_ARRAY && rebase->count <= 8192, "invalid relocations");
    p->rebase = allocate(rebase->count * sizeof(u64));
    for (usize i = 0; i < rebase->count; ++i) {
        REQUIRE(p->rebase && number_of(&rebase->items[i], &p->rebase[i]) &&
                    (inside_segment(p, p->rebase[i], 8, 1) || inside_segment(p, p->rebase[i], 8, 0)),
                "invalid relocation");
        for (usize k = 0; k < i; ++k)
            REQUIRE(p->rebase[k] + 8 <= p->rebase[i] || p->rebase[i] + 8 <= p->rebase[k], "overlapping relocations");
    }
    p->rebase_count = rebase->count;

    const Json* imports = member(j, "imports");
    REQUIRE(imports && imports->type == J_ARRAY && imports->count <= 196, "invalid imports");
    p->imports = allocate(imports->count * sizeof(Import));
    for (usize i = 0; i < imports->count; ++i) {
        Import* im = &p->imports[i];
        REQUIRE(p->imports, "out of memory");
        im->name = text_of(member(&imports->items[i], "name"), 96);
        REQUIRE(is_symbol(im->name) && number_of(member(&imports->items[i], "slot"), &im->slot) && im->slot % 8 == 0 &&
                    inside_segment(p, im->slot, 8, 1),
                "invalid import slot");
        for (usize k = 0; k < i; ++k) REQUIRE(!equal(p->imports[k].name, im->name), "duplicate import");
        for (usize k = 0; k < p->rebase_count; ++k)
            REQUIRE(p->rebase[k] + 8 <= im->slot || im->slot + 8 <= p->rebase[k], "import slot overlaps a relocation");
        for (usize k = 0; k < i; ++k)
            REQUIRE(p->imports[k].slot != im->slot, "duplicate import slot");
    }
    p->import_count = imports->count;

    const Json* hooks = member(j, "hooks");
    const Json* patches = member(j, "patches");
    usize patch_count = patches ? patches->count : 0;
    REQUIRE(hooks && hooks->type == J_ARRAY && hooks->count <= MAX_HOOKS && hooks->count + patch_count >= 1 &&
                (!patches || patches->type == J_ARRAY),
            "invalid hook count");
    p->hooks = allocate(hooks->count * sizeof(Hook));
    for (usize i = 0; i < hooks->count; ++i) {
        const Json* item = &hooks->items[i];
        Hook* h = &p->hooks[i];
        REQUIRE(p->hooks, "out of memory");
        h->name = text_of(member(item, "name"), 96);
        h->replacement = text_of(member(item, "replacement"), 96);
        h->original = text_of(member(item, "original"), 96);
        h->expected = unhex(member(item, "expected"), 256, &h->expected_size);
        const char* mode = text_of(member(item, "mode"), 32);
        REQUIRE(is_symbol(h->name) && is_symbol(h->replacement) && is_symbol(h->original) &&
                    number_of(member(item, "offset"), &h->offset) && h->expected && h->expected_size >= 5 &&
                    member(p->exports, h->replacement) && !is_sdk_import(h->original) &&
                    !equal(h->original, h->replacement),
                "invalid hook contract");
        for (usize k = 0; k < i; ++k)
            REQUIRE(!equal(p->hooks[k].name, h->name) && !equal(p->hooks[k].original, h->original),
                    "duplicate hook/original");
        if (!mode || equal(mode, "typed")) {
            h->site = 0;
        } else {
            const char* site_mode = text_of(member(item, "site_mode"), 16);
            REQUIRE(equal(mode, "site-x86_64-sse") && site_mode && (equal(site_mode, "before") || equal(site_mode, "replace")),
                    "unsupported hook mode");
            h->site = 1;
            h->replace = equal(site_mode, "replace");
            REQUIRE(!h->replace || h->expected_size <= 32, "replace site covers more than 32 bytes");
        }
        const Json* relocation = member(item, "relocation");
        const Json* fixups = member(relocation, "fixups");
        u64 stolen;
        h->trampoline = unhex(member(relocation, "code"), STUB_STRIDE - 16, &h->trampoline_size);
        REQUIRE(h->trampoline && number_of(member(relocation, "stolen"), &stolen) && stolen >= 5 &&
                    stolen <= h->expected_size && (!h->replace || stolen == h->expected_size) && fixups &&
                    fixups->type == J_ARRAY && fixups->count >= 1 && fixups->count <= 64,
                "invalid relocation template");
        h->stolen = (usize)stolen;
        h->fixups = allocate(fixups->count * sizeof(Fixup));
        for (usize k = 0; k < fixups->count; ++k) {
            const Json* f = &fixups->items[k];
            Fixup* x = &h->fixups[k];
            const char* kind = text_of(member(f, "kind"), 16);
            u64 at, end = 0;
            REQUIRE(h->fixups && kind && number_of(member(f, "at"), &at) && number_of(member(f, "target"), &x->target),
                    "invalid relocation fixup");
            x->at = (u32)at;
            if (equal(kind, "rel32")) {
                REQUIRE(number_of(member(f, "end"), &end) && at + 4 <= h->trampoline_size && end >= at + 4 &&
                            end <= h->trampoline_size,
                        "invalid rel32 fixup");
                x->kind = 0;
                x->end = (u32)end;
            } else {
                x->kind = equal(kind, "abs64") ? 1 : equal(kind, "abs64_local") ? 2 : -1;
                REQUIRE(x->kind > 0 && at + 8 <= h->trampoline_size &&
                            (x->kind == 1 || x->target < h->trampoline_size),
                        "invalid abs64 fixup");
            }
        }
        h->fixup_count = fixups->count;
    }
    p->hook_count = hooks->count;

    if (patch_count) {
        REQUIRE(patch_count <= 256, "too many code patches");
        p->patches = allocate(patch_count * sizeof(CodePatch));
        for (usize i = 0; i < patch_count; ++i) {
            const Json* item = &patches->items[i];
            CodePatch* c = &p->patches[i];
            usize replacement_size = 0;
            REQUIRE(p->patches, "out of memory");
            c->name = text_of(member(item, "name"), 96);
            c->expected = unhex(member(item, "expected"), 256, &c->size);
            c->replacement = unhex(member(item, "replacement"), 256, &replacement_size);
            REQUIRE(is_symbol(c->name) && number_of(member(item, "offset"), &c->offset) && c->expected &&
                        c->replacement && c->size && c->size == replacement_size &&
                        memcmp(c->expected, c->replacement, c->size),
                    "invalid code patch");
        }
        p->patch_count = patch_count;
    }

    const Json* bindings = member(j, "bindings");
    if (bindings) {
        REQUIRE(bindings->type == J_ARRAY && bindings->count <= 128, "too many guest bindings");
        p->bindings = allocate(bindings->count * sizeof(Binding));
        for (usize i = 0; i < bindings->count; ++i) {
            const Json* item = &bindings->items[i];
            Binding* b = &p->bindings[i];
            const char* kind = text_of(member(item, "kind"), 16);
            REQUIRE(p->bindings, "out of memory");
            b->name = text_of(member(item, "name"), 96);
            REQUIRE(is_symbol(b->name) && kind && number_of(member(item, "offset"), &b->offset) &&
                        !member(p->exports, b->name),
                    "invalid guest binding");
            if (equal(kind, "function")) {
                b->function = 1;
                b->expected = unhex(member(item, "expected"), 256, &b->expected_size);
                b->size = b->expected_size;
                b->alignment = 1;
                REQUIRE(b->expected && b->expected_size >= 5, "function binding requires a preimage");
            } else {
                const char* access = text_of(member(item, "access"), 8);
                REQUIRE(equal(kind, "data") && number_of(member(item, "size"), &b->size) &&
                            number_of(member(item, "alignment"), &b->alignment) && access &&
                            (equal(access, "r") || equal(access, "rw")) && b->size && b->size <= (1 << 20) &&
                            b->alignment && b->alignment <= 4096 && !(b->alignment & (b->alignment - 1)) &&
                            b->offset % b->alignment == 0,
                        "invalid guest data binding");
                b->writable = equal(access, "rw");
            }
        }
        p->binding_count = bindings->count;
    }

    /* Every import resolves to the SDK, a hook continuation or a binding. */
    for (usize i = 0; i < p->import_count; ++i) {
        const char* name = p->imports[i].name;
        int known = is_sdk_import(name);
        for (usize k = 0; k < p->hook_count && !known; ++k) known = equal(p->hooks[k].original, name);
        for (usize k = 0; k < p->binding_count && !known; ++k) known = equal(p->bindings[k].name, name);
        REQUIRE(known, "unknown import");
    }
    const char* kinds[2] = {"counters", "logs"};
    for (int t = 0; t < 2; ++t) {
        const Json* list = member(j, kinds[t]);
        Tag* tags = t ? p->logs : p->counters;
        usize* count = t ? &p->log_count : &p->counter_count;
        if (!list) continue;
        REQUIRE(list->type == J_ARRAY && list->count <= 64, "too many counters/log tags");
        for (usize i = 0; i < list->count; ++i) {
            REQUIRE(number_of(member(&list->items[i], "id"), &tags[i].id) && tags[i].id &&
                        is_symbol(tags[i].name = text_of(member(&list->items[i], "name"), 96)),
                    "invalid counter/log tag");
            for (usize k = 0; k < i; ++k) REQUIRE(tags[k].id != tags[i].id, "duplicate counter/log tag");
        }
        *count = list->count;
    }
    return 1;
}

/* ---- SDK services (Windows x64 calls from payload code) -------------------------------- */

static u64 sdk_query(u64 version) {
    __atomic_fetch_add(&sdk_calls, 1, __ATOMIC_RELAXED);
    return version == 1 ? 7 : 0; /* clock, counters, logs */
}
static u64 sdk_clock_ns(void) {
    static s64 frequency;
    s64 count;
    __atomic_fetch_add(&sdk_calls, 1, __ATOMIC_RELAXED);
    if (!frequency) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    return (u64)(count / frequency) * 1000000000ULL + (u64)(count % frequency) * 1000000000ULL / (u64)frequency;
}
static Tag* find_tag(Tag* tags, usize count, u64 id) {
    for (usize i = 0; i < count; ++i)
        if (tags[i].id == id) return &tags[i];
    return 0;
}
static u64 sdk_counter(usize index, u64 id, s64 value) {
    __atomic_fetch_add(&sdk_calls, 1, __ATOMIC_RELAXED);
    Tag* tag = index < package_count ? find_tag(packages[index].counters, packages[index].counter_count, id) : 0;
    if (!tag) return (u64)-22;
    __atomic_store_n(&tag->last, value, __ATOMIC_RELAXED);
    __atomic_fetch_add(&tag->count, 1, __ATOMIC_RELAXED);
    return 0;
}
static u64 sdk_log(usize index, u64 id, s64 value) {
    __atomic_fetch_add(&sdk_calls, 1, __ATOMIC_RELAXED);
    Tag* tag = index < package_count ? find_tag(packages[index].logs, packages[index].log_count, id) : 0;
    if (!tag) return (u64)-22;
    __atomic_store_n(&tag->last, value, __ATOMIC_RELAXED);
    __atomic_fetch_add(&tag->count, 1, __ATOMIC_RELAXED);
    Text t = {0};
    put(&t, "[PATCH_LOG] package=");
    put(&t, packages[index].id);
    put(&t, " tag=");
    put(&t, tag->name);
    put(&t, " value=");
    put_s64(&t, value);
    put(&t, " thread=");
    put_u64(&t, GetCurrentThreadId());
    if (t.data) log_line(t.data, 0, 0);
    release(t.data);
    return 0;
}
#define SDK_PACKAGE(n) \
    static u64 sdk_counter_##n(u64 id, s64 value) { return sdk_counter(n, id, value); } \
    static u64 sdk_log_##n(u64 id, s64 value) { return sdk_log(n, id, value); }
SDK_PACKAGE(0) SDK_PACKAGE(1) SDK_PACKAGE(2) SDK_PACKAGE(3)
SDK_PACKAGE(4) SDK_PACKAGE(5) SDK_PACKAGE(6) SDK_PACKAGE(7)
static u64 (*const sdk_counters[MAX_PACKAGES])(u64, s64) = {sdk_counter_0, sdk_counter_1, sdk_counter_2, sdk_counter_3,
                                                           sdk_counter_4, sdk_counter_5, sdk_counter_6, sdk_counter_7};
static u64 (*const sdk_logs[MAX_PACKAGES])(u64, s64) = {sdk_log_0, sdk_log_1, sdk_log_2, sdk_log_3,
                                                       sdk_log_4, sdk_log_5, sdk_log_6, sdk_log_7};

/* ---- install ---------------------------------------------------------------------- */

typedef struct {
    u8* base;
    u64 size;
    const u8* sections; /* IMAGE_SECTION_HEADER array */
    u32 section_count;
} Module;

static int module_of(void* base, Module* m) {
    u8* b = base;
    if (b[0] != 'M' || b[1] != 'Z') return 0;
    u32 pe = *(u32*)(b + 0x3c);
    if (pe > 0x1000 || memcmp(b + pe, "PE\0\0", 4) || *(u16*)(b + pe + 4) != 0x8664) return 0;
    u16 optional_size = *(u16*)(b + pe + 20);
    if (*(u16*)(b + pe + 24) != 0x20b) return 0; /* PE32+ */
    m->base = b;
    m->size = *(u32*)(b + pe + 24 + 56); /* SizeOfImage */
    m->section_count = *(u16*)(b + pe + 6);
    m->sections = b + pe + 24 + optional_size;
    return 1;
}
#define SECTION_EXECUTE 0x20000000U
#define SECTION_READ 0x40000000U
#define SECTION_WRITE 0x80000000U
/* Whether [offset, offset+size) lies in one section with all `flags`. */
static int in_section(const Module* m, u64 offset, u64 size, u32 flags) {
    for (u32 i = 0; i < m->section_count; ++i) {
        const u8* s = m->sections + 40 * i;
        u64 address = *(u32*)(s + 12), extent = *(u32*)(s + 8);
        u32 characteristics = *(u32*)(s + 36);
        if (!extent) extent = *(u32*)(s + 16);
        if ((characteristics & flags) == flags && offset >= address && size <= extent && offset - address <= extent - size)
            return 1;
    }
    return 0;
}

/* Whether one package changes module bytes the other changes or calls (a function
 * binding): the later one would find a foreign jump instead of its preimage, or call
 * code that is no longer the original (shadPS4 FindCodeOverlap). */
static int ranges_overlap(const Package* a, const Package* b) {
    for (int pass = 0; pass < 2; ++pass) {
        const Package* x = pass ? b : a;
        const Package* y = pass ? a : b;
        for (usize i = 0; i < x->hook_count + x->patch_count + x->binding_count; ++i) {
            u64 begin, end;
            if (i < x->hook_count) {
                begin = x->hooks[i].offset;
                end = begin + x->hooks[i].expected_size;
            } else if (i < x->hook_count + x->patch_count) {
                begin = x->patches[i - x->hook_count].offset;
                end = begin + x->patches[i - x->hook_count].size;
            } else {
                const Binding* bd = &x->bindings[i - x->hook_count - x->patch_count];
                if (!bd->function) continue;
                begin = bd->offset;
                end = begin + bd->size;
            }
            /* Only y's changes count: two bindings of one function do not conflict. */
            for (usize k = 0; k < y->hook_count + y->patch_count; ++k) {
                u64 b0 = k < y->hook_count ? y->hooks[k].offset : y->patches[k - y->hook_count].offset;
                u64 b1 = b0 + (k < y->hook_count ? y->hooks[k].expected_size : y->patches[k - y->hook_count].size);
                if (begin < b1 && b0 < end) return 1;
            }
        }
    }
    return 0;
}

/* A free range of `size` bytes whose every byte is within rel32 reach of [low, high). */
static u8* allocate_near(u64 low, u64 high, u64 size) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    u64 granularity = info.granularity ? info.granularity : 0x10000;
    const u64 reach = 0x7fff0000ULL;
    u64 floor = high > reach ? high - reach : granularity;
    u64 ceiling = low + reach - size;
    /* Upwards from the module first: games rarely map code right after their image. */
    for (int direction = 0; direction < 2; ++direction) {
        u64 address = direction == 0 ? (high + granularity - 1) & ~(granularity - 1) : low;
        for (int steps = 0; steps < 65536; ++steps) {
            if (direction == 0 && address > ceiling) break;
            if (direction == 1 && (address < floor + size || address <= granularity)) break;
            MEMORY_INFO m;
            u64 probe = direction == 0 ? address : address - 1;
            if (!VirtualQuery((void*)probe, &m, sizeof(m))) break;
            u64 region = (u64)m.base, end = region + m.region_size;
            if (m.state == MEM_FREE) {
                u64 candidate = direction == 0 ? (region + granularity - 1) & ~(granularity - 1)
                                               : (end - size) & ~(granularity - 1);
                if (candidate >= region && candidate + size <= end && candidate >= floor && candidate <= ceiling) {
                    void* got = VirtualAlloc((void*)candidate, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
                    if (got) return got;
                }
            }
            address = direction == 0 ? end : region;
        }
    }
    return 0;
}

static void protect(void* address, u64 size, DWORD protection) {
    DWORD old;
    VirtualProtect(address, size, protection, &old);
}
/* Writes code into the module: no thread runs it yet (the module is not initialized,
 * or the main thread has not started), and FEX drops what it compiled for the range. */
static void write_code(u64 address, const u8* bytes, usize size) {
    DWORD old;
    VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)address, bytes, size);
    VirtualProtect((void*)address, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)address, size);
}

static int install(Package* p, usize index, void* base, const wchar* path) {
    Module m;
    REQUIRE(module_of(base, &m), "target module is not a PE32+ x64 image");
    char sha[65];
    REQUIRE(file_sha256(path, sha), "cannot hash the module file");
    if (!equal(sha, p->module_sha256)) {
        set_error(p->error, "module SHA256 mismatch (a different build): ", sha);
        return 0;
    }
    for (usize i = 0; i < package_count; ++i)
        if (&packages[i] != p && packages[i].installed && equal(packages[i].module, p->module) &&
            ranges_overlap(&packages[i], p)) {
            set_error(p->error, "conflicts with installed package ", packages[i].id);
            return 0;
        }
    /* Preimages against the module as loaded (after the loader's relocations). */
    for (usize i = 0; i < p->hook_count; ++i) {
        Hook* h = &p->hooks[i];
        REQUIRE(in_section(&m, h->offset, h->expected_size, SECTION_EXECUTE), "hook outside executable section");
        REQUIRE(!memcmp(m.base + h->offset, h->expected, h->expected_size), "hook preimage mismatch");
        for (usize k = 0; k < i; ++k)
            REQUIRE(h->offset + h->expected_size <= p->hooks[k].offset ||
                        p->hooks[k].offset + p->hooks[k].expected_size <= h->offset,
                    "overlapping hooks");
    }
    for (usize i = 0; i < p->patch_count; ++i) {
        CodePatch* c = &p->patches[i];
        REQUIRE(in_section(&m, c->offset, c->size, SECTION_EXECUTE), "code patch outside executable section");
        REQUIRE(!memcmp(m.base + c->offset, c->expected, c->size), "code patch preimage mismatch");
        for (usize k = 0; k < p->hook_count; ++k)
            REQUIRE(c->offset + c->size <= p->hooks[k].offset || p->hooks[k].offset + p->hooks[k].expected_size <= c->offset,
                    "code patch overlaps a hook");
        for (usize k = 0; k < i; ++k)
            REQUIRE(c->offset + c->size <= p->patches[k].offset || p->patches[k].offset + p->patches[k].size <= c->offset,
                    "overlapping code patches");
    }
    for (usize i = 0; i < p->binding_count; ++i) {
        Binding* b = &p->bindings[i];
        u32 flags = b->function ? SECTION_EXECUTE : (b->writable ? SECTION_READ | SECTION_WRITE : SECTION_READ);
        REQUIRE(in_section(&m, b->offset, b->size, flags), "guest binding permission mismatch");
        if (b->function) REQUIRE(!memcmp(m.base + b->offset, b->expected, b->expected_size), "function binding preimage mismatch");
    }

    /* Payload, then stubs (RX) and one page of dispatch slots. */
    p->code_size = ((u64)p->hook_count * STUB_STRIDE + 4095) & ~4095ULL;
    p->pool_size = p->code_size + 4096;
    u64 image_size = (p->image_size + 4095) & ~4095ULL;
    u8* image = allocate_near((u64)m.base, (u64)m.base + m.size, image_size + p->pool_size);
    REQUIRE(image, "no free memory within rel32 reach of the module");
    p->module_base = (u64)m.base;
    p->image = (u64)image;
    p->pool = p->image + image_size;
    u8* stubs = image + image_size;
    u64* slots = (u64*)(stubs + p->code_size);
    memset(stubs, 0xcc, p->code_size);
    u64 base_offset = 0;
    for (usize i = 0; i < p->segment_count; ++i) {
        memcpy(image + base_offset, p->segment[i], p->segment_bytes[i]);
        base_offset += p->segment_size[i];
    }
    for (usize i = 0; i < p->rebase_count; ++i) {
        u64 value;
        memcpy(&value, image + p->rebase[i], 8);
        if (value >= p->image_size) {
            VirtualFree(image, 0, MEM_RELEASE);
            REQUIRE(0, "absolute relocation target outside payload");
        }
        value += p->image;
        memcpy(image + p->rebase[i], &value, 8);
    }
    for (usize n = 0; n < p->hook_count; ++n) {
        Hook* h = &p->hooks[n];
        h->entry = p->module_base + h->offset;
        h->stub = p->pool + n * STUB_STRIDE;
        h->original_address = h->stub + 16;
        h->slot = p->pool + p->code_size + n * 8;
        u64 export_offset = 0;
        number_of(member(p->exports, h->replacement), &export_offset);
        h->replacement_address = p->image + export_offset;
        s64 displacement = (s64)h->stub - (s64)(h->entry + 5);
        if (displacement < -0x80000000LL || displacement > 0x7fffffffLL) {
            VirtualFree(image, 0, MEM_RELEASE);
            REQUIRE(0, "entry stub outside rel32 range");
        }
        u8* stub = stubs + n * STUB_STRIDE;
        stub[0] = 0xff;
        stub[1] = 0x25;
        s32 slot_displacement = (s32)((s64)h->slot - (s64)(h->stub + 6));
        memcpy(stub + 2, &slot_displacement, 4);
        u8* trampoline = stub + 16;
        memcpy(trampoline, h->trampoline, h->trampoline_size);
        for (usize k = 0; k < h->fixup_count; ++k) {
            const Fixup* f = &h->fixups[k];
            if (f->kind == 0) {
                s64 value = (s64)(p->module_base + f->target) - (s64)(h->original_address + f->end);
                if (value < -0x80000000LL || value > 0x7fffffffLL) {
                    VirtualFree(image, 0, MEM_RELEASE);
                    REQUIRE(0, "RIP-relative target outside rel32 range of the trampoline");
                }
                s32 v = (s32)value;
                memcpy(trampoline + f->at, &v, 4);
            } else {
                u64 value = f->kind == 1 ? p->module_base + f->target : h->original_address + f->target;
                memcpy(trampoline + f->at, &value, 8);
            }
        }
        slots[n] = h->replacement_address;
    }
    /* Import slots: SDK services, hook continuations, guest bindings. */
    for (usize i = 0; i < p->import_count; ++i) {
        const char* name = p->imports[i].name;
        u64 value = 0;
        if (equal(name, SDK_IMPORTS[0])) value = (u64)&sdk_query;
        else if (equal(name, SDK_IMPORTS[1])) value = (u64)&sdk_clock_ns;
        else if (equal(name, SDK_IMPORTS[2])) value = (u64)sdk_counters[index];
        else if (equal(name, SDK_IMPORTS[3])) value = (u64)sdk_logs[index];
        for (usize k = 0; k < p->hook_count && !value; ++k)
            if (equal(p->hooks[k].original, name))
                value = p->hooks[k].replace ? p->hooks[k].entry + p->hooks[k].expected_size : p->hooks[k].original_address;
        for (usize k = 0; k < p->binding_count && !value; ++k)
            if (equal(p->bindings[k].name, name)) value = p->module_base + p->bindings[k].offset;
        u64 old;
        memcpy(&old, image + p->imports[i].slot, 8);
        if (!value || old) {
            VirtualFree(image, 0, MEM_RELEASE);
            REQUIRE(0, "unresolved or nonzero import slot");
        }
        memcpy(image + p->imports[i].slot, &value, 8);
    }
    /* Publish: payload code and stubs RX, payload data and slots RW, then the entries. */
    protect(image, p->segment_size[0], PAGE_EXECUTE_READ);
    protect(stubs, p->code_size, PAGE_EXECUTE_READ);
    FlushInstructionCache(GetCurrentProcess(), image, image_size + p->pool_size);
    for (usize n = 0; n < p->hook_count; ++n) {
        Hook* h = &p->hooks[n];
        u8 patch[256];
        memset(patch, 0x90, h->stolen);
        patch[0] = 0xe9;
        s32 displacement = (s32)((s64)h->stub - (s64)(h->entry + 5));
        memcpy(patch + 1, &displacement, 4);
        write_code(h->entry, patch, h->stolen);
    }
    for (usize i = 0; i < p->patch_count; ++i)
        write_code(p->module_base + p->patches[i].offset, p->patches[i].replacement, p->patches[i].size);
    p->installed = 1;
    return 1;
}
#undef REQUIRE

/* ---- status and control ------------------------------------------------------------- */

static void write_status_locked(void) {
    Text t = {0};
    put(&t, "pid: ");
    put_u64(&t, GetCurrentProcessId());
    put(&t, "\napp: ");
    put_wide(&t, app_id, wlength(app_id));
    put(&t, "\npackages: ");
    put_u64(&t, package_count);
    usize installed = 0;
    for (usize i = 0; i < package_count; ++i) installed += packages[i].installed;
    put(&t, "\ninstalled: ");
    put_u64(&t, installed);
    put(&t, "\nswitches: ");
    put_u64(&t, switches);
    put(&t, "\ncontrol_sequence: ");
    put_u64(&t, control_sequence);
    put(&t, "\nsdk_calls: ");
    put_u64(&t, __atomic_load_n(&sdk_calls, __ATOMIC_RELAXED));
    put(&t, "\n");
    if (selection_error[0]) put(&t, "error: "), put(&t, selection_error), put(&t, "\n");
    for (usize i = 0; i < package_count; ++i) {
        Package* p = &packages[i];
        put(&t, "package: ");
        put(&t, p->id ? p->id : "<unloaded>");
        put(&t, "\nfile: ");
        put_wide(&t, p->file, wlength(p->file));
        put(&t, "\nsha256: ");
        put(&t, p->sha256);
        put(&t, "\nname: ");
        put(&t, p->display_name ? p->display_name : "");
        put(&t, "\nmodule: ");
        put(&t, p->module ? p->module : "");
        put(&t, "\ninstalled: ");
        put_u64(&t, (u64)p->installed);
        put(&t, "\n");
        if (p->error[0]) put(&t, "error: "), put(&t, p->error), put(&t, "\n");
        if (p->id && !p->attempted) put(&t, "waiting_for_module: "), put(&t, p->module), put(&t, "\n");
        if (p->installed) {
            put(&t, "payload: "), put_hex(&t, p->image), put(&t, " pool: "), put_hex(&t, p->pool);
            put(&t, " module_base: "), put_hex(&t, p->module_base), put(&t, "\n");
        }
        for (usize n = 0; p->installed && n < p->hook_count; ++n) {
            Hook* h = &p->hooks[n];
            u64 slot = __atomic_load_n((u64*)h->slot, __ATOMIC_RELAXED);
            put(&t, h->site ? "site: " : "hook: ");
            put(&t, h->name);
            if (h->site) put(&t, h->replace ? " mode=replace" : " mode=before");
            put(&t, " entry="), put_hex(&t, h->entry);
            put(&t, " handler="), put_hex(&t, h->replacement_address);
            put(&t, " original="), put_hex(&t, h->original_address);
            put(&t, " stolen="), put_u64(&t, h->stolen);
            put(&t, " enabled="), put_u64(&t, slot == h->replacement_address);
            put(&t, "\n");
        }
        for (usize n = 0; p->installed && n < p->patch_count; ++n) {
            put(&t, "code_patch: "), put(&t, p->patches[n].name);
            put(&t, " address="), put_hex(&t, p->module_base + p->patches[n].offset);
            put(&t, " size="), put_u64(&t, p->patches[n].size), put(&t, "\n");
        }
        for (int kind = 0; kind < 2; ++kind) {
            Tag* tags = kind ? p->logs : p->counters;
            usize count = kind ? p->log_count : p->counter_count;
            for (usize n = 0; n < count; ++n) {
                put(&t, kind ? "log_tag: " : "counter: ");
                put_u64(&t, tags[n].id);
                put(&t, " "), put(&t, tags[n].name);
                put(&t, " samples="), put_u64(&t, __atomic_load_n(&tags[n].count, __ATOMIC_RELAXED));
                put(&t, " last="), put_s64(&t, __atomic_load_n(&tags[n].last, __ATOMIC_RELAXED));
                put(&t, "\n");
            }
        }
    }
    if (!t.data) return;
    wchar temporary[512], final_path[512];
    path_in_directory(temporary, L"status.tmp");
    path_in_directory(final_path, L"status.txt");
    write_file(L"status.tmp", t.data, t.size, 0);
    MoveFileExW(temporary, final_path, MOVEFILE_REPLACE_EXISTING);
    release(t.data);
}

/* "<seq> enable|disable [package|hook|package/hook]": no name switches every hook. */
static void apply_control(const char* line) {
    u64 sequence = 0;
    while (*line >= '0' && *line <= '9') sequence = sequence * 10 + (u64)(*line++ - '0');
    if (sequence <= control_sequence || *line != ' ') return;
    control_sequence = sequence;
    ++line;
    int enable;
    if (!memcmp(line, "enable", 6)) enable = 1, line += 6;
    else if (!memcmp(line, "disable", 7)) enable = 0, line += 7;
    else return;
    while (*line == ' ') ++line;
    char name[200];
    usize n = 0;
    while (*line && *line != '\r' && *line != '\n' && *line != ' ' && n < sizeof(name) - 1) name[n++] = *line++;
    name[n] = 0;
    char shown[200];
    memcpy(shown, name, n + 1);
    char *package_name = 0, *hook_name = 0;
    for (usize i = 0; i < n; ++i)
        if (name[i] == '/') name[i] = 0, package_name = name, hook_name = name + i + 1;
    if (n && !package_name) {
        int is_package = 0;
        for (usize i = 0; i < package_count; ++i)
            if (packages[i].id && equal(packages[i].id, name)) is_package = 1;
        if (is_package) package_name = name;
        else hook_name = name;
    }
    int found = 0;
    for (usize i = 0; i < package_count; ++i) {
        Package* p = &packages[i];
        if (!p->installed || (package_name && !equal(p->id, package_name))) continue;
        for (usize k = 0; k < p->hook_count; ++k) {
            Hook* h = &p->hooks[k];
            if (hook_name && !equal(h->name, hook_name)) continue;
            /* An aligned 8-byte store: a thread at the stub jumps either way, never torn. */
            __atomic_store_n((u64*)h->slot, enable ? h->replacement_address : h->original_address, __ATOMIC_RELEASE);
            found = 1;
        }
    }
    if (found) ++switches;
    log_line(found ? "control applied: " : "control matched nothing: ", enable ? "enable " : "disable ", shown);
}

static DWORD control_thread(void* parameter) {
    (void)parameter;
    wchar path[512];
    path_in_directory(path, L"control.txt");
    u64 last_calls = ~0ULL;
    FILE_ATTRIBUTES seen = {0};
    for (;;) {
        /* Attributes only (a stat): opening the file every poll costs an ioctl that
         * Android's SELinux denies and audits for app data files. */
        FILE_ATTRIBUTES now = {0};
        usize size = 0;
        char* control = 0;
        if (GetFileAttributesExW(path, 0 /* GetFileExInfoStandard */, &now) &&
            (now.written.lo != seen.written.lo || now.written.hi != seen.written.hi || now.size_low != seen.size_low)) {
            seen = now;
            control = read_file(path, 1 << 16, &size);
        }
        AcquireSRWLockExclusive(&state_lock);
        u64 before = switches + control_sequence;
        for (char* line = control; line && *line;) {
            apply_control(line);
            while (*line && *line != '\n') ++line;
            if (*line) ++line;
        }
        u64 calls = __atomic_load_n(&sdk_calls, __ATOMIC_RELAXED);
        if (before != switches + control_sequence || calls != last_calls) write_status_locked();
        last_calls = calls;
        ReleaseSRWLockExclusive(&state_lock);
        release(control);
        Sleep(250);
    }
    return 0;
}

/* ---- module events ------------------------------------------------------------------ */

static void try_install(void* base, const wchar* full_path, const wchar* base_name, usize base_name_length) {
    int changed = 0;
    AcquireSRWLockExclusive(&state_lock);
    for (usize i = 0; i < package_count; ++i) {
        Package* p = &packages[i];
        if (!p->id || p->attempted || !same_name(base_name, base_name_length, p->module)) continue;
        p->attempted = 1;
        changed = 1;
        if (install(p, i, base, full_path)) {
            log_line("installed ", p->id, 0);
        } else {
            log_line("not installed, the game runs without it: ", p->id, 0);
            log_line("  reason: ", p->error, 0);
        }
    }
    if (changed) write_status_locked();
    ReleaseSRWLockExclusive(&state_lock);
}

static void notification(DWORD reason, const DLL_NOTIFICATION* data, void* context) {
    (void)context;
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !data || !data->full_name || !data->base_name) return;
    wchar path[520];
    usize n = data->full_name->length / sizeof(wchar);
    if (n >= 520) return;
    memcpy(path, data->full_name->buffer, n * sizeof(wchar));
    path[n] = 0;
    try_install(data->base, path, data->base_name->buffer, data->base_name->length / sizeof(wchar));
}

static void install_loaded(void) {
    for (usize i = 0; i < package_count; ++i) {
        Package* p = &packages[i];
        if (!p->id || p->attempted) continue;
        wchar name[130];
        usize n = 0;
        for (const char* s = p->module; *s && n < 129; ++s) name[n++] = (wchar)(u8)*s;
        name[n] = 0;
        HANDLE module = GetModuleHandleW(name);
        if (!module) continue;
        wchar path[520];
        DWORD got = GetModuleFileNameW(module, path, 520);
        if (!got || got >= 520) continue;
        try_install(module, path, name, n);
    }
}

/* Packages in name order from the launcher's copy, at most MAX_PACKAGES. */
static void read_packages(void) {
    wchar pattern[512];
    path_in_directory(pattern, L"packages\\*.json");
    /* Heap, not stack: no stack probes in this DLL, so frames stay well below a page. */
    FIND_DATA* found = allocate(sizeof(FIND_DATA));
    wchar(*names)[64] = allocate(sizeof(wchar[MAX_PACKAGES][64]));
    usize count = 0;
    HANDLE find = found && names ? FindFirstFileW(pattern, found) : INVALID_HANDLE;
    if (find != INVALID_HANDLE) {
        do {
            usize n = wlength(found->name);
            if (n >= 64 || (found->attributes & 0x10)) continue;
            if (count == MAX_PACKAGES) {
                set_error(selection_error, "more than 8 packages; the rest are left out", 0);
                break;
            }
            memcpy(names[count++], found->name, (n + 1) * sizeof(wchar));
        } while (FindNextFileW(find, found));
        FindClose(find);
    }
    release(found);
    for (usize i = 1; i < count; ++i) /* insertion sort by name */
        for (usize k = i; k > 0; --k) {
            usize a = 0;
            while (names[k - 1][a] && names[k - 1][a] == names[k][a]) ++a;
            if (names[k - 1][a] <= names[k][a]) break;
            wchar swap[64];
            memcpy(swap, names[k], sizeof(swap));
            memcpy(names[k], names[k - 1], sizeof(swap));
            memcpy(names[k - 1], swap, sizeof(swap));
        }
    for (usize i = 0; i < count; ++i) {
        Package* p = &packages[package_count++];
        memcpy(p->file, names[i], sizeof(p->file));
        wchar path[512], relative[80] = L"packages\\";
        usize n = 9;
        for (usize k = 0; names[i][k] && n < 79; ++k) relative[n++] = names[i][k];
        relative[n] = 0;
        path_in_directory(path, relative);
        usize size = 0;
        char* source = read_file(path, 4 << 20, &size);
        if (!source) {
            set_error(p->error, "cannot read package (missing or larger than 4 MiB)", 0);
            p->attempted = 1;
            continue;
        }
        sha256_bytes((u8*)source, size, p->sha256);
        Parser ps = {source, source + size, 0};
        Json* root = allocate(sizeof(Json));
        int ok = root && parse_value(&ps, root, 0);
        if (ok) {
            skip_space(&ps);
            if (ps.p != ps.end) ok = 0, ps.error = "trailing data";
        }
        if (!ok) {
            set_error(p->error, "invalid JSON: ", ps.error ? ps.error : "out of memory");
            p->id = 0;
            p->attempted = 1;
            continue;
        }
        if (!load_package(p, root)) {
            p->id = 0;
            p->attempted = 1;
            continue;
        }
        /* The launcher copies only this game's packages; check anyway. */
        usize id_length = wlength(app_id);
        if (!same_name(app_id, id_length, p->title)) {
            set_error(p->error, "package is for another game: ", p->title);
            p->id = 0;
            p->attempted = 1;
            continue;
        }
        for (usize k = 0; k + 1 < package_count; ++k)
            if (packages[k].id && equal(packages[k].id, p->id)) {
                set_error(p->error, "same package id as an earlier one", 0);
                p->id = 0;
                p->attempted = 1;
            }
    }
    release(names);
}

EXPORT BOOL DllMain(HANDLE module, DWORD reason, void* reserved) {
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return 1;
    DisableThreadLibraryCalls(module);
    heap = GetProcessHeap();
    directory_length = GetEnvironmentVariableW(L"XRGAME_GUEST_PATCH_DIR", directory, 400);
    if (!directory_length || directory_length >= 400) return 1; /* not configured: stay inert */
    DWORD id_length = GetEnvironmentVariableW(L"XRGAME_GUEST_PATCH_APP", app_id, 24);
    if (!id_length || id_length >= 24) app_id[0] = 0;
    log_line("installer loaded", 0, 0);
    AcquireSRWLockExclusive(&state_lock);
    read_packages();
    write_status_locked();
    ReleaseSRWLockExclusive(&state_lock);
    install_loaded();
    void* cookie;
    if (LdrRegisterDllNotification(0, notification, 0, &cookie)) log_line("cannot watch DLL loads", 0, 0);
    HANDLE thread = CreateThread(0, 0, control_thread, 0, 0, 0);
    if (thread) CloseHandle(thread);
    return 1;
}
