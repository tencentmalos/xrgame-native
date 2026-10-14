// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// How the 2D window producer (xr_capture.cpp, namespace panel) reaches the encoder. HWUI renders the
// window into an AImageReader of ours, never into the encoder's input Surface: HWUI's RenderThread
// serves every window of the process, so a codec that stops taking frames would block it in
// dequeueBuffer and freeze the whole app (and DebugBus with it). The reader is always drained: its
// listener keeps only the newest image, and a thread of ours blits that one into the encoder's
// input window through GLES. Only that thread ever waits on the encoder; stopping the encoder
// (which abandons its input Surface) releases it.

#include <android/native_window.h>

#include <cstdint>
#include <memory>
#include <string>

namespace xrimmersive::capture::panel {

struct FeedStats {
    uint64_t rendered = 0;  // images HWUI finished
    uint64_t fed = 0;       // images posted to the encoder
    uint64_t dropped = 0;   // replaced by a newer image before the thread took them
    uint64_t busyNs = 0;    // how long the current post has been running; 0 while idle
};

class Feed {
public:
    Feed();
    ~Feed();
    Feed(const Feed &) = delete;
    Feed &operator=(const Feed &) = delete;

    // `encoder` must stay valid until Stop() returns.
    bool Start(ANativeWindow *encoder, uint32_t width, uint32_t height, std::string *error);
    // The window HWUI renders into (owned by the reader, freed with this object); null if not started.
    ANativeWindow *Window() const;
    // Takes no more images. Before the encoder stops; a post blocked in the encoder ends with it.
    void RequestStop();
    // After the encoder stopped: joins the thread and frees the GLES objects. The reader stays until
    // this object goes, so HWUI may still render into its window meanwhile.
    void Stop();
    FeedStats Stats() const;
    std::string Error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrimmersive::capture::panel
