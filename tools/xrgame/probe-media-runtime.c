/* Build with NDK clang -O2 -Wall -Wextra -Werror and -ldl.
 * Run under the app UID with its LD_LIBRARY_PATH and GST_PLUGIN_PATH.
 * Loads only the supplied libraries and checks Wine's non-GL media elements.
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    const char *elements[] = {"decodebin", "typefind", "qtdemux", "capssetter",
        "videoconvert", "deinterlace", "videoflip", "audioconvert", "audioresample",
        "avdec_h264", "avdec_aac"};
    int failed = 0;
    for (int i = 1; i < argc; ++i) {
        void *handle = dlopen(argv[i], RTLD_NOW | RTLD_GLOBAL);
        printf("library %s: %s\n", argv[i], handle ? "loaded" : dlerror());
        failed |= !handle;
    }
    void *gst = dlopen("libgstreamer-1.0.so", RTLD_NOW | RTLD_GLOBAL);
    if (!gst) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    int (*init)(int *, char ***, void **) = dlsym(gst, "gst_init_check");
    void *(*find)(const char *) = dlsym(gst, "gst_element_factory_find");
    void (*unref)(void *) = dlsym(gst, "gst_object_unref");
    if (!init || !find || !unref) return 1;
    void *error = NULL;
    if (!init(NULL, NULL, &error)) { fprintf(stderr, "gst_init_check failed\n"); return 1; }
    for (unsigned int i = 0; i < sizeof(elements) / sizeof(elements[0]); ++i) {
        void *factory = find(elements[i]);
        printf("element %s: %s\n", elements[i], factory ? "present" : "MISSING");
        if (factory) unref(factory);
        else failed = 1;
    }
    return failed;
}
