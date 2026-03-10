# Phase 6: Per-Seat Process Management — COMPLETE

## Summary of Changes

### `src/seat.h`
- Forward-declared `proc::proc_t`
- Added `proc::proc_t *process = nullptr` to `seat_t` (non-owning pointer)

### `src/seat.cpp`
- Added `#include "process.h"`
- `make_default_seat()`: sets `process = &proc::proc` (global singleton)
- `acquire()` multi-seat path: sets `process = &proc::proc` if not already set

### `src/stream.cpp`
Updated 6 call sites from `proc::proc` to use seat's process pointer with fallback:

| Line | Old | New |
|------|-----|-----|
| ~1022 | `proc::proc.get_env()` | `session->seat->process->get_env()` (server cmd lambda) |
| ~1195 | `proc::proc.running()` | Kept global + added TODO for Phase 8 |
| ~2066 | `proc::proc.get_env()` | `session.seat->process->get_env()` (undo_cmds lambda) |
| ~2084-85 | `proc::proc.running()` / `.pause()` | `process->running()` / `process->pause()` via seat |
| ~2140 | `proc::proc.resume()` | `process->resume()` via seat |
| ~2147 | `proc::proc.get_env()` | `do_process->get_env()` (do_cmds lambda) |

All call sites use the pattern: `auto *process = session.seat ? session.seat->process : &proc::proc;`

## Backward Compatibility
- Single-seat: `seat->process` points to `&proc::proc`, identical behavior
- All HTTP API paths (nvhttp, confighttp) unchanged — they'll be addressed in Phase 8
- No new config options needed
