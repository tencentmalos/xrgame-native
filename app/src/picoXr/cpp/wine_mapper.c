// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>

// Standalone Wine runs in the system linker's default namespace. Turnip's
// Mapper 5 backend needs the vendor HAL namespace to read AHB layout metadata.
// This adapter is preloaded only in Wine; it never loads a bundled vendor HAL.
void *dlopen(const char *path, int flags) {
    void *(*next_open)(const char *, int) = dlsym(RTLD_NEXT, "dlopen");
    if (!next_open) return NULL;

    if (path && !strcmp(path, "/vendor/lib64/hw/mapper.qti.so")) {
        void *support = next_open("/system/lib64/libvndksupport.so", RTLD_NOW | RTLD_LOCAL);
        if (support) {
            void *(*load_sphal)(const char *, int) =
                dlsym(support, "android_load_sphal_library");
            void *mapper = load_sphal ? load_sphal(path, flags) : NULL;
            dlclose(support);
            if (mapper) return mapper;
        }
    }

    // Bionic chooses the namespace from dlopen's return address. A normal
    // forwarding call changes that address and can break libui's own HAL
    // loading. Require a tail call so all other callers retain their namespace.
    __attribute__((musttail)) return next_open(path, flags);
}
