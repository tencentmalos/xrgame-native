#pragma once

// The X server's scene for an immersive session: a ring of AHardwareBuffers the Vulkan renderer
// composites into, shared by libvulkan_renderer (the producer) and libxrimmersive (the session).
// The renderer publishes a frame once its GPU work completed and never waits for the session: it
// writes a slot the session does not hold and that is not the latest frame. The session holds the
// slots it reads until its own GPU work on them has completed. Both sides only use the atomics and
// the destroy callback, so the struct may cross library boundaries.

#include <android/hardware_buffer.h>

#include <atomic>
#include <cstdint>

namespace xrgame {

struct XrFrameExchange {
    static constexpr uint32_t kVersion = 1;
    // The session holds the frame it shows, the one before (frame generation) and slots its GPU
    // work still reads; one more is the latest frame, so five leave one to write in most cases.
    static constexpr uint32_t kSlots = 5;

    uint32_t version = kVersion;
    uint32_t width = 0;
    uint32_t height = 0;
    AHardwareBuffer *buffers[kSlots] = {};
    // The latest completed frame as serial << 8 | slot; 0 before the first one.
    std::atomic<uint64_t> latest{0};
    // Per slot: the count of game content updates the frame composited (frames that differ only
    // by the cursor share it), and when its GPU work completed (CLOCK_MONOTONIC nanoseconds).
    std::atomic<uint64_t> content[kSlots] = {};
    std::atomic<int64_t> completedNs[kSlots] = {};
    // Slots the session reads, one bit each.
    std::atomic<uint32_t> held{0};
    std::atomic<uint32_t> refs{1};
    void (*destroy)(XrFrameExchange *) = nullptr;

    static uint64_t Pack(uint64_t serial, uint32_t slot) { return serial << 8 | slot; }
    static uint64_t Serial(uint64_t packed) { return packed >> 8; }
    static uint32_t Slot(uint64_t packed) { return static_cast<uint32_t>(packed & 0xff); }
};

inline void Retain(XrFrameExchange *exchange) { exchange->refs.fetch_add(1, std::memory_order_relaxed); }

inline void Release(XrFrameExchange *exchange) {
    if (exchange->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) exchange->destroy(exchange);
}

}  // namespace xrgame
