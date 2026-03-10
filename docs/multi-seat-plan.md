# Multi-Seating Implementation Plan for Apollo

## Executive Summary

This plan details the phased implementation of multi-seating for Apollo, enabling multiple concurrent users to stream independently from a single host machine. Each user gets their own isolated desktop environment with independent video capture, audio capture, input routing, and optional virtual display. The design follows a Windows-first approach while maintaining cross-platform compatibility and full backward compatibility with single-user mode.

---

## Table of Contents

1. [Architectural Overview](#1-architectural-overview)
2. [Identified Blockers](#2-identified-blockers)
3. [Phase 1: Seat Abstraction Layer](#3-phase-1-seat-abstraction-layer)
4. [Phase 2: Per-Seat Video Capture](#4-phase-2-per-seat-video-capture)
5. [Phase 3: Per-Seat Audio Capture](#5-phase-3-per-seat-audio-capture)
6. [Phase 4: Per-Seat Input Isolation](#6-phase-4-per-seat-input-isolation)
7. [Phase 5: Per-Seat Virtual Display & Desktop Environment](#7-phase-5-per-seat-virtual-display--desktop-environment)
8. [Phase 6: Per-Seat Process Management](#8-phase-6-per-seat-process-management)
9. [Phase 7: Windows Session Isolation (TermWrap-like)](#9-phase-7-windows-session-isolation-termwrap-like)
10. [Phase 8: Configuration & Web UI](#10-phase-8-configuration--web-ui)
11. [Phase 9: Multi-Seat Networking](#11-phase-9-multi-seat-networking)
12. [Cross-Cutting Concerns](#12-cross-cutting-concerns)
13. [Key Questions Answered](#13-key-questions-answered)
14. [File Change Summary](#14-file-change-summary)

---

## 1. Architectural Overview

### Current Architecture (Single-Seat)

The current flow is:
```
Client RTSP -> make_launch_session() [nvhttp.cpp]
            -> session::alloc() [stream.cpp:2148]
            -> session::start() [stream.cpp:2089]
                -> videoThread() -> video::capture(mail, config, channel_data)
                                     -> capture_async() using GLOBAL capture_thread_async
                                        -> shared platf::display_t targeting GLOBAL config::video.output_name
                -> audioThread() -> audio::capture(mail, config, channel_data)
                                     -> GLOBAL control_shared -> WASAPI system default
                -> input::alloc(mail) [already per-session]
```

Key globals that enforce single-seat behavior:
- `broadcast` (stream.cpp:488) - single set of UDP sockets
- `capture_thread_async` (video.cpp:483) - one display capture thread
- `chosen_encoder` (video.cpp:1054) - one encoder for all sessions
- `control_shared` (audio.cpp:254) - one audio context
- `config::video.output_name` (config.h:89) - one display target
- `proc::proc` (process.h:195) - one running application

### Target Architecture (Multi-Seat)

```
Client RTSP -> make_launch_session() [nvhttp.cpp]
            -> seat_manager::acquire_seat(launch_session) [NEW]
                -> creates/finds seat_t with:
                   - dedicated virtual display (or assigned physical display)
                   - dedicated audio endpoint
                   - dedicated input target
            -> session::alloc(config, launch_session, seat) [MODIFIED]
            -> session::start(session, addr)
                -> videoThread() -> video::capture(mail, config, channel_data, seat.display_name)
                                     -> per-seat capture_thread keyed by display_name
                -> audioThread() -> audio::capture(mail, config, channel_data, seat.audio_endpoint)
                                     -> per-seat audio context keyed by endpoint
                -> input routed to seat.input_target
```

### Core Design Principle: Seat as Resource Bundle

A "seat" is a bundle of desktop resources assigned to one streaming session:

```cpp
struct seat_t {
    std::string id;                    // Unique seat identifier
    std::string display_name;          // Target display for video capture
    std::string audio_sink_id;         // Target audio endpoint for capture
    std::optional<GUID> vdisplay_guid; // Virtual display GUID (Windows)
    input_target_t input_target;       // Where to route input events
    std::weak_ptr<session_t> session;  // Currently bound session (if any)
    seat_state_e state;                // AVAILABLE, BOUND, RELEASING
};
```

---

## 2. Identified Blockers

Each blocker is addressed in a specific phase:

| # | Blocker | Location | Addressed In |
|---|---------|----------|-------------|
| 1 | `broadcast` global (single socket set) | stream.cpp:488 | Phase 9 |
| 2 | `capture_thread_async` global | video.cpp:483 | Phase 2 |
| 3 | `chosen_encoder` global pointer | video.cpp:1054 | Phase 2 |
| 4 | `config::video.output_name` global | config.h:89, video.cpp:1087+ | Phase 2 |
| 5 | `control_shared` global audio context | audio.cpp:254 | Phase 3 |
| 6 | `proc::proc` singleton | process.h:195 | Phase 6 |
| 7 | No Windows session isolation | N/A | Phase 7 |
| 8 | No seat orchestration layer | N/A | Phase 1 |

---

## 3. Phase 1: Seat Abstraction Layer

**Goal**: Introduce the `seat_t` concept and `seat_manager` without changing any existing behavior. Single-user mode continues to work identically through a "default seat."

### New Files to Create

#### `src/seat.h`
```cpp
#pragma once

#include <string>
#include <optional>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <guiddef.h>
#endif

namespace seat {

    enum class state_e {
        AVAILABLE,   // Seat resources are allocated but not bound to a session
        BOUND,       // Seat is actively streaming
        RELEASING,   // Seat is being torn down
    };

    // Represents an input routing target for a seat
    struct input_target_t {
        std::string display_name;  // For SendInput targeting (Windows)
        // Future: HWND, session ID, etc.
    };

    struct seat_t {
        std::string id;
        std::string display_name;          // Video capture target
        std::string audio_sink_id;         // Audio capture target (empty = system default)
        input_target_t input_target;

#ifdef _WIN32
        std::optional<GUID> vdisplay_guid; // Virtual display GUID
#endif

        state_e state = state_e::AVAILABLE;

        // The session currently bound to this seat (weak to avoid circular ref)
        std::weak_ptr<void> bound_session;
    };

    using seat_ptr = std::shared_ptr<seat_t>;

    // Creates a default seat that mirrors current single-user behavior
    seat_ptr make_default_seat();

    // Seat manager: allocates, tracks, and releases seats
    class manager_t {
    public:
        // Acquire a seat for a new session. Returns nullptr if no seat available.
        seat_ptr acquire(/* launch_session params */);

        // Release a seat when a session ends
        void release(const seat_ptr &seat);

        // Get all active seats
        std::vector<seat_ptr> active_seats() const;

        // Check if multi-seating is enabled
        bool multi_seat_enabled() const;

    private:
        mutable std::mutex _mutex;
        std::vector<seat_ptr> _seats;
        bool _multi_seat = false;  // Driven by config
    };

    // Global seat manager instance
    extern manager_t manager;

}  // namespace seat
```

#### `src/seat.cpp`
Implementation of seat manager. In single-seat mode, `acquire()` always returns the same default seat. In multi-seat mode, it allocates from a pool or creates new virtual displays.

### Files to Modify

#### `src/stream.h` - Add seat reference to session
- Add `#include "seat.h"`
- Add `seat::seat_ptr seat` field to forward-declared `session_t`

#### `src/stream.cpp` - Thread through seat to session
- In `session_t` struct (line 350): add `seat::seat_ptr seat;`
- In `session::alloc()` (line 2148): accept `seat::seat_ptr` parameter, store in session
- In `session::start()` (line 2089): pass seat through to video/audio threads
- When session stops: call `seat::manager.release(session->seat)`

#### `src/rtsp.h` / `src/nvhttp.cpp` - Seat acquisition at session creation
- Before calling `session::alloc()`, call `seat::manager.acquire()`
- Pass the resulting seat through the session creation pipeline

### Backward Compatibility
- When `seat::manager.multi_seat_enabled()` returns false, `acquire()` returns a singleton default seat that uses `config::video.output_name` and system default audio
- All existing behavior is preserved; the seat is just passed through without changing any capture logic yet

### Testing Strategy
- Verify single-user streaming still works identically
- Verify seat_t is created and destroyed with session lifecycle
- Unit test seat_manager allocation and release

---

## 4. Phase 2: Per-Seat Video Capture

**Goal**: Make video capture target a specific display per seat instead of reading from the global `config::video.output_name`.

### Key Insight
The `capture_thread_async` global (video.cpp:483) uses a `safe::make_shared` pattern where a single capture thread is shared across all sessions targeting the same display. For multi-seating, we need one capture thread **per unique display target**. The existing sharing pattern is actually correct for multiple sessions on the same display -- we just need to key it by display name.

### Files to Modify

#### `src/video.h` - Add display_name to capture signature
```cpp
// MODIFIED: Add display_name parameter
void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data,
    const std::string &display_name = ""  // Empty = use config default (backward compat)
);
```

#### `src/video.cpp` - Per-display capture thread pool

**Step 1: Replace global `capture_thread_async` with a keyed map**

At line 483, replace:
```cpp
// OLD: Single global capture thread
auto capture_thread_async = safe::make_shared<capture_thread_async_ctx_t>(...);
```

With a thread-safe map of display_name -> capture_thread:
```cpp
// NEW: Per-display capture thread pool
static std::mutex capture_threads_mutex;
static std::unordered_map<std::string,
    safe::shared_t<capture_thread_async_ctx_t>> capture_threads;

static safe::shared_t<capture_thread_async_ctx_t>&
get_capture_thread_for_display(const std::string &display_name) {
    std::lock_guard lock(capture_threads_mutex);
    auto it = capture_threads.find(display_name);
    if (it == capture_threads.end()) {
        auto [inserted, _] = capture_threads.emplace(
            display_name,
            safe::make_shared<capture_thread_async_ctx_t>(
                start_capture_async, end_capture_async));
        return inserted->second;
    }
    return it->second;
}
```

**Step 2: Parameterize `refresh_displays()` (lines 1085-1139)**

Currently reads `config::video.output_name` at lines 1087, 1112. Change to accept `display_name` as parameter:
```cpp
// MODIFIED: Accept display_name parameter instead of reading global
void refresh_displays(
    platf::mem_type_e dev_type,
    ...,
    const std::string &display_name  // NEW parameter
) {
    const auto output_name { display_device::map_output_name(display_name) };
    // ... rest of function uses output_name parameter instead of global
}
```

All 5 call sites using `config::video.output_name` (lines 1087, 1112, 2525, 2830) must be updated to take the display name from the seat/config rather than the global.

**Step 3: Modify `capture_async()` (lines 2354-2436)**

Replace `capture_thread_async.ref()` at line 2367 with:
```cpp
auto &capture_thread = get_capture_thread_for_display(display_name);
auto ref = capture_thread.ref();
```

**Step 4: Modify `capture()` entry point (lines 2438-2466)**

Add `display_name` parameter, pass through to `capture_async()` / `capture_sync()`.

**Step 5: Handle `chosen_encoder` (line 1054)**

The encoder selection (`probe_encoders()` at line 2700+) is hardware-dependent, not display-dependent. All seats on the same GPU use the same encoder. For Phase 2, keep `chosen_encoder` as a global but ensure it is probed before any seat starts. In a future phase (multi-GPU), this could become per-GPU.

#### `src/stream.cpp` - Pass seat display_name to video::capture

At line 1920, change:
```cpp
// OLD
video::capture(session->mail, session->config.monitor, session);

// NEW
video::capture(
    session->mail,
    session->config.monitor,
    session,
    session->seat ? session->seat->display_name : ""
);
```

### Backward Compatibility
- Empty `display_name` falls back to `config::video.output_name` (existing behavior)
- Single-seat default seat has `display_name = ""` which triggers the fallback

---

## 5. Phase 3: Per-Seat Audio Capture

**Goal**: Allow each seat to capture audio from a specific audio endpoint rather than the system default.

### Key Insight
The global `control_shared` (audio.cpp:254) manages a single WASAPI audio context. For multi-seating, each seat needs its own audio endpoint. On Windows, virtual audio cables or per-display audio sinks (HDMI/DP audio from virtual displays) can provide isolated endpoints.

### Files to Modify

#### `src/audio.h` - Add endpoint to capture signature
```cpp
// MODIFIED: Add audio_sink_id parameter
void capture(
    safe::mail_t mail,
    config_t config,
    void *channel_data,
    const std::string &audio_sink_id = ""  // Empty = system default
);

// NEW: Per-endpoint audio context management
audio_ctx_ref_t get_audio_ctx_ref(const std::string &endpoint_id = "");
```

#### `src/audio.cpp` - Per-endpoint audio context pool

Replace the global `control_shared` (line 254) with a keyed map:
```cpp
// OLD
static auto control_shared {safe::make_shared<audio_ctx_t>(
    start_audio_control, stop_audio_control)};

// NEW: Per-endpoint audio context pool
static std::mutex audio_contexts_mutex;
static std::unordered_map<std::string,
    safe::shared_t<audio_ctx_t>> audio_contexts;

static safe::shared_t<audio_ctx_t>&
get_audio_context_for_endpoint(const std::string &endpoint_id) {
    std::lock_guard lock(audio_contexts_mutex);
    auto key = endpoint_id.empty() ? "__default__" : endpoint_id;
    auto it = audio_contexts.find(key);
    if (it == audio_contexts.end()) {
        auto [inserted, _] = audio_contexts.emplace(
            key,
            safe::make_shared<audio_ctx_t>(
                start_audio_control, stop_audio_control));
        return inserted->second;
    }
    return it->second;
}
```

The `start_audio_control` callback must be parameterized to target the specific audio endpoint rather than the system default. This requires modifying `platf::audio_control_t` to accept an endpoint ID.

#### `src/platform/common.h` - Parameterize audio control creation
Add an `endpoint_id` parameter to the audio control factory, defaulting to empty (system default).

#### `src/platform/windows/audio.cpp` - WASAPI endpoint targeting
The Windows WASAPI implementation must be modified to:
1. Accept a specific audio endpoint ID
2. Initialize the audio capture client on that specific endpoint
3. Support enumerating available audio endpoints for the seat manager

#### `src/stream.cpp` - Pass seat audio endpoint
At line 1941, change:
```cpp
// OLD
audio::capture(session->mail, session->config.audio, session);

// NEW
audio::capture(
    session->mail,
    session->config.audio,
    session,
    session->seat ? session->seat->audio_sink_id : ""
);
```

### Audio Endpoint Strategy (Windows)
1. **Virtual display audio**: SudoVDA virtual displays can expose HDMI/DP audio endpoints. Each virtual display gets its own audio sink automatically.
2. **Virtual audio cable**: Software like VB-Audio or Voicemeeter can create virtual endpoints.
3. **System default fallback**: If no dedicated endpoint, fall back to system default (single-seat behavior).

---

## 6. Phase 4: Per-Seat Input Isolation

**Goal**: Route input events from each session to the correct desktop/display target.

### Current State
Input is already per-session (`input::alloc(session.mail)` at stream.cpp:2090). However, the actual input injection (SendInput on Windows, uinput on Linux) targets the foreground desktop globally.

### Files to Modify

#### `src/input.h` - Add seat context to input allocation
```cpp
// MODIFIED: Accept seat context for input targeting
std::shared_ptr<input_t> alloc(safe::mail_t mail, const seat::seat_ptr &seat = nullptr);
```

#### `src/input.cpp` / `src/platform/windows/input.cpp`
- Store the seat's `input_target` in the input context
- On Windows, use `SetThreadDesktop()` or target the specific virtual display coordinates
- Route mouse events to the virtual display's coordinate space
- Route keyboard events to the foreground window of the seat's desktop

#### `src/stream.cpp` - Pass seat to input allocation
At line 2090:
```cpp
// OLD
session->input = input::alloc(session.mail);

// NEW
session->input = input::alloc(session.mail, session->seat);
```

### Input Targeting Strategy (Windows)
1. **Coordinate remapping**: Virtual displays have specific coordinates in the Windows desktop space. Remap Moonlight's relative coordinates to the virtual display's absolute position.
2. **Focus management**: Each seat's virtual display is the "foreground" for that session. Input events are injected relative to that display.
3. **Gamepad isolation**: Each session already has its own gamepad emulation via ViGEm. No changes needed for gamepad isolation.

---

## 7. Phase 5: Per-Seat Virtual Display & Desktop Environment

**Goal**: Automatically create and manage virtual displays for each seat using the existing SudoVDA integration.

### Current State
Virtual display support already exists in Apollo:
- `VDISPLAY::createVirtualDisplay()` in `src/platform/windows/virtual_display.h`
- `launch_session_t` already has `virtual_display` flag and `display_guid`
- `proc_t` supports virtual display creation per app

### Files to Modify

#### `src/seat.cpp` - Virtual display lifecycle in seat manager
When a seat is acquired in multi-seat mode:
1. Call `VDISPLAY::createVirtualDisplay(client_uid, client_name, width, height, fps, guid)`
2. Wait for the display to appear via `VDISPLAY::matchDisplay()`
3. Set `seat.display_name` to the new virtual display's device name
4. Set `seat.vdisplay_guid` for cleanup
5. Optionally call `VDISPLAY::changeDisplaySettings2()` for resolution/refresh

When a seat is released:
1. Call `VDISPLAY::removeVirtualDisplay(seat.vdisplay_guid)`
2. Remove from capture thread pool

#### `src/platform/windows/virtual_display.cpp` - Enhance for multi-seat
- Add function to query audio endpoint associated with a virtual display
- Add function to get virtual display coordinate bounds for input targeting
- Ensure multiple simultaneous virtual displays are supported

#### `src/process.cpp` - Virtual display per seat (not per app)
Currently, virtual display creation is tied to app launch in `proc_t::execute()`. For multi-seating, the virtual display is owned by the **seat**, not the **app**. The app runs within the seat's display.

### Display Configuration
Each seat's virtual display:
- Has a unique GUID
- Has resolution matching the client's requested resolution
- Has refresh rate matching the client's requested FPS
- Is placed at a non-overlapping position in the virtual desktop (or isolated at origin per existing logic)

---

## 8. Phase 6: Per-Seat Process Management

**Goal**: Allow each seat to run its own application independently.

### Current State
`proc::proc` is a global singleton (process.h:195). It manages exactly one running application. Multi-seating requires concurrent app execution.

### Approach: Seat-Scoped Process Context

#### `src/process.h` - Make proc_t per-seat
Instead of one global `proc::proc`, the seat manager holds per-seat process contexts:

```cpp
// NEW: Add to seat_t
struct seat_t {
    // ... existing fields ...
    std::unique_ptr<proc::proc_t> process;  // Per-seat app process
};
```

The global `proc::proc` remains for backward compatibility in single-seat mode. In multi-seat mode, each seat's `process` field is used instead.

#### `src/process.cpp` - Scope operations to seat
- `execute()`: Launch app within the seat's virtual display context
- `terminate()`: Only terminate the seat's app, not all apps
- `running()`: Report status per seat

#### `src/stream.cpp` - Use seat's process context
At lines 2121-2123, the session start currently does:
```cpp
if (++running_sessions == 1) {
    platf::streaming_will_start();
    proc::proc.resume();
}
```
In multi-seat mode, this must resume the seat-specific process instead:
```cpp
if (session->seat && session->seat->process) {
    session->seat->process->resume();
} else {
    proc::proc.resume();  // Single-seat fallback
}
```

---

## 9. Phase 7: Windows Session Isolation (TermWrap-like)

**Goal**: Create isolated Windows desktop sessions per seat for full process/window isolation. This is the most complex and invasive phase.

### Background: DuoStream/Duo Approach
DuoStream uses "TermWrap" to create multiple Windows Terminal Services sessions on a single machine, each with its own desktop, explorer shell, and GPU context. This requires:
1. RDP session creation (or equivalent session bootstrapping)
2. Per-session desktop with its own window station
3. GPU-accelerated rendering in each session

### Proposed Approach for Apollo

**Option A: Virtual Desktop (Window Station) Isolation** (Recommended for Phase 7)
- Use `CreateDesktop()` / `CreateWindowStation()` Win32 APIs
- Each seat gets its own Window Station + Desktop
- Processes launched in a seat run on that Desktop
- Less invasive than full Terminal Services sessions
- Does not require RDP licensing or TermSrv modifications

**Option B: Terminal Services Sessions** (Full DuoStream parity, future)
- Create actual TS sessions per seat
- Full process/window isolation
- Requires TermSrv patching (termsrv.dll) or RDS licensing
- Most complete isolation but most complex

### New Files to Create

#### `src/platform/windows/session_isolation.h`
```cpp
#pragma once
#include <string>
#include <optional>

namespace platf::session_isolation {
    struct desktop_session_t {
        std::string id;
        void *window_station;  // HWINSTA
        void *desktop;         // HDESK
        // Process launch context for this session
    };

    // Create an isolated desktop for a seat
    std::optional<desktop_session_t> create_session(const std::string &seat_id);

    // Destroy an isolated desktop
    void destroy_session(desktop_session_t &session);

    // Launch a process within a specific desktop session
    bool launch_in_session(desktop_session_t &session, const std::string &cmd);
}
```

#### `src/platform/windows/session_isolation.cpp`
Implementation using Win32 `CreateWindowStationW`, `CreateDesktopW`, `SetThreadDesktop`, and `CreateProcessAsUser` with `STARTUPINFO.lpDesktop`.

### Integration with Seat Manager
When `seat::manager.acquire()` is called in multi-seat mode on Windows:
1. Create a virtual display (Phase 5)
2. Create an isolated desktop session (this phase)
3. Associate the desktop with the virtual display
4. Set up audio endpoint routing (Phase 3)
5. Return the seat with all resources bound

---

## 10. Phase 8: Configuration & Web UI

**Goal**: Add configuration options for multi-seating and Web UI controls.

### Files to Modify

#### `src/config.h` - Multi-seat configuration
```cpp
struct multiseat_t {
    bool enabled = false;        // Master toggle
    int max_seats = 4;           // Maximum concurrent seats
    bool auto_virtual_display = true;  // Auto-create virtual displays
    bool session_isolation = false;    // Windows desktop isolation
    std::string audio_mode = "auto";   // "auto", "virtual", "shared"
};

// Add to top-level config namespace:
extern multiseat_t multiseat;
```

#### `src/config.cpp` - Parse multi-seat config
Add parsing for the new `multiseat` section in the config file.

#### `src/confighttp.cpp` - Web UI API endpoints
Add REST endpoints for:
- `GET /api/seats` - List active seats and their status
- `GET /api/seats/{id}` - Get seat details
- `POST /api/seats/{id}/release` - Force release a seat
- `GET /api/config/multiseat` - Get multi-seat configuration
- `PUT /api/config/multiseat` - Update multi-seat configuration

#### Web UI Frontend (`src/assets/web/`)
Add a "Multi-Seat" tab/section:
- Toggle multi-seat mode on/off
- Set max concurrent seats
- View active seats with client info, display assignment, audio endpoint
- Force disconnect individual seats
- Configure per-seat defaults (resolution, display assignment)

---

## 11. Phase 9: Multi-Seat Networking

**Goal**: Support independent network paths for each seat.

### Current State
The `broadcast` global (stream.cpp:488) binds a single set of UDP sockets that all sessions share. The `start_broadcast()` function binds to fixed port offsets from the base port.

### Approach: Per-Seat Port Allocation

Currently, sessions share the same video/audio/control sockets and are demuxed by peer address. This actually works for multi-seating because each client has a unique address. However, for robustness:

**Option A: Keep shared sockets (Recommended initially)**
- The existing broadcast architecture with shared sockets and per-session peer tracking already supports multiple sessions
- Each session has its own peer address, cipher context, and sequence numbers
- The `control_server._sessions` list already supports multiple sessions
- No changes needed for the networking layer initially

**Option B: Per-seat sockets (Future optimization)**
- Each seat gets its own port set for better QoS isolation
- Requires modifying `broadcast_ctx_t` to be per-seat
- Allows independent rate limiting per seat

### Assessment
The existing shared-socket broadcast architecture at stream.cpp:488 already supports multiple concurrent sessions. The `broadcast.ref()` pattern correctly reference-counts the shared context. Multiple sessions are already tracked in `control_server._sessions`. **No networking changes are required for initial multi-seating.**

The key limitation is that `start_broadcast` / `end_broadcast` assumes a single lifecycle. With multiple sessions, the broadcast stays alive as long as any session references it, which is the correct behavior thanks to the `safe::make_shared` reference counting.

---

## 12. Cross-Cutting Concerns

### Backward Compatibility
- All new parameters have defaults that preserve single-seat behavior
- `seat::manager.multi_seat_enabled()` gates all multi-seat code paths
- When disabled, a single default seat is used transparently
- No changes to the Moonlight client protocol are needed
- Existing config files work without modification

### Thread Safety
- `seat::manager_t` uses a mutex for seat allocation/release
- Per-display capture thread map uses a mutex
- Per-endpoint audio context map uses a mutex
- Existing `safe::shared_t` / `safe::make_shared` patterns handle lifecycle correctly

### Resource Cleanup
- Seat release is triggered by session stop (stream.cpp session cleanup)
- Virtual displays are removed when seats are released
- Audio contexts are freed when the last reference drops (safe::make_shared)
- Capture threads are freed when the last reference drops (safe::make_shared)
- Isolated desktops are destroyed when seats are released

### Platform Considerations
- **Windows**: Full multi-seating with virtual displays, audio isolation, desktop isolation
- **Linux**: Multi-seating possible with virtual displays (via DRM/KMS), audio isolation (PipeWire/PulseAudio), no desktop isolation needed (X11/Wayland sessions)
- **macOS**: Limited multi-seating (no virtual display support, limited audio isolation)

### Encoder Considerations
- `chosen_encoder` remains global -- all seats on the same GPU use the same encoder
- Hardware encoder session limits (e.g., NVENC 5 sessions on consumer GPUs) limit the number of concurrent encoding seats
- The `probe_encoders()` function should be enhanced to report max concurrent sessions
- Software encoder has no session limit but higher CPU cost

### Performance Budget
- Each seat adds: 1 capture thread, 1 encode thread, 1 audio capture thread
- Virtual displays add no meaningful overhead (GPU renders only active displays)
- Network overhead is purely additive (each seat's bitrate is independent)
- RAM overhead: ~50-100MB per seat for frame buffers and encode state

---

## 13. Key Questions Answered

### Q1: Windows Session Isolation Approach
**Answer**: Two-tier approach. Phase 5 uses virtual displays with SudoVDA for display isolation. Phase 7 adds optional Window Station/Desktop isolation via `CreateDesktop()`/`CreateWindowStation()` for full process isolation. Full Terminal Services session isolation (DuoStream/TermWrap parity) is a future option beyond Phase 7.

### Q2: Video Capture Modification
**Answer**: Replace the global `capture_thread_async` with a map of per-display capture threads (Phase 2). The existing `safe::make_shared` lifecycle pattern is preserved. The `config::video.output_name` global is replaced by a per-seat `display_name` parameter threaded through `video::capture()`. All 5 call sites in video.cpp are updated.

### Q3: Audio Isolation
**Answer**: Replace the global `control_shared` with a map of per-endpoint audio contexts (Phase 3). Each seat specifies an audio endpoint ID. On Windows, virtual display HDMI/DP audio or virtual audio cables provide per-seat endpoints. The WASAPI implementation is modified to accept a specific endpoint ID.

### Q4: Input Routing
**Answer**: Input is already per-session. The remaining work (Phase 4) is coordinate remapping -- each seat's virtual display has specific coordinates in the Windows desktop space. Mouse/keyboard events are translated to the correct display's coordinate space. Gamepad isolation is already handled by ViGEm per-session emulation.

### Q5: Cross-Platform Hooks
**Answer**: The seat abstraction (`seat.h`) is platform-agnostic. Platform-specific implementations of virtual display creation, audio endpoint targeting, and desktop isolation live in `src/platform/{windows,linux,macos}/`. On platforms without full multi-seating support, the seat manager falls back to shared-display mode with a warning.

### Q6: Incremental Phases
**Answer**: 9 phases from least invasive to most invasive:
1. Seat abstraction (no behavior change)
2. Per-seat video (display targeting)
3. Per-seat audio (endpoint targeting)
4. Per-seat input (coordinate remapping)
5. Virtual displays (automatic creation)
6. Per-seat processes (concurrent apps)
7. Desktop isolation (Window Station/Desktop)
8. Config & UI (user-facing controls)
9. Networking (if needed, optional)

Each phase is independently testable and deployable. Single-seat mode is preserved throughout.

### Q7: Code Reuse from DuoStream/Duo
**Answer**: DuoStream's architecture validates the overall approach but its implementation is tightly coupled to its own infrastructure. The key insights to reuse are:
- TermWrap's concept of per-user desktop sessions (our Phase 7)
- Virtual display per user pattern (our Phase 5, already partially implemented via SudoVDA)
- The principle that session isolation happens at the OS level, not the streaming protocol level

Apollo's existing SudoVDA integration, safe::make_shared lifecycle management, and per-session mail system provide a stronger foundation than starting from DuoStream's code.

---

## 14. File Change Summary

### New Files

| File | Phase | Purpose |
|------|-------|---------|
| `src/seat.h` | 1 | Seat abstraction and manager declarations |
| `src/seat.cpp` | 1 | Seat manager implementation |
| `src/platform/windows/session_isolation.h` | 7 | Windows desktop session isolation declarations |
| `src/platform/windows/session_isolation.cpp` | 7 | Windows desktop isolation implementation |

### Modified Files

| File | Phase(s) | Changes |
|------|----------|---------|
| `src/stream.h` | 1 | Add seat.h include, seat_ptr in session forward decl |
| `src/stream.cpp` | 1,2,3,4 | Add seat to session_t, pass through to video/audio/input, seat lifecycle in session start/stop |
| `src/video.h` | 2 | Add display_name param to capture() |
| `src/video.cpp` | 2 | Per-display capture thread pool, parameterize refresh_displays(), update 5 output_name sites |
| `src/audio.h` | 3 | Add endpoint param to capture() and get_audio_ctx_ref() |
| `src/audio.cpp` | 3 | Per-endpoint audio context pool replacing global control_shared |
| `src/input.h` | 4 | Add seat param to alloc() |
| `src/input.cpp` | 4 | Coordinate remapping for input targeting |
| `src/platform/windows/input.cpp` | 4 | Windows-specific input targeting |
| `src/platform/common.h` | 3 | Add endpoint_id to audio_control_t factory |
| `src/platform/windows/audio.cpp` | 3 | WASAPI per-endpoint initialization |
| `src/platform/windows/virtual_display.cpp` | 5 | Multi-display creation support, audio endpoint query |
| `src/process.h` | 6 | Per-seat process context |
| `src/process.cpp` | 6 | Scoped app execution per seat |
| `src/config.h` | 8 | multiseat_t configuration struct |
| `src/config.cpp` | 8 | Parse multi-seat config |
| `src/confighttp.cpp` | 8 | REST API for seat management |
| `src/nvhttp.cpp` | 1 | Seat acquisition before session creation |
| `src/rtsp.h` | 1 | Seat info in launch_session_t |
| `CMakeLists.txt` | 1 | Add seat.cpp to build |

### Recommended Implementation Order
1. Phase 1 (foundation)
2. Phase 2 + Phase 5 together (video capture needs virtual displays for testing)
3. Phase 3 (audio, can test with virtual display audio endpoints)
4. Phase 4 (input, straightforward once displays exist)
5. Phase 6 (process management, extends seat lifecycle)
6. Phase 8 (config/UI, makes everything user-accessible)
7. Phase 7 (desktop isolation, optional advanced feature)
8. Phase 9 (networking, only if needed)
