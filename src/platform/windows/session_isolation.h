/**
 * @file src/platform/windows/session_isolation.h
 * @brief Per-seat Windows desktop isolation for multi-seat mode.
 *
 * Creates isolated Window Station + Desktop pairs so each seat's processes
 * run in their own desktop, invisible to other seats. Uses a shared
 * Window Station ("ApolloWinSta0") with per-seat Desktops ("Seat_<id>").
 */
#pragma once

#include <optional>
#include <string>

#include <windows.h>

namespace platf::session_isolation {

  /**
   * @brief Holds handles and metadata for an isolated desktop session.
   */
  struct desktop_session_t {
    HWINSTA winsta = nullptr;   ///< Shared Window Station handle
    HDESK desktop = nullptr;    ///< Per-seat Desktop handle
    std::string desktop_name;   ///< Full name "ApolloWinSta0\\Seat_<id>" (UTF-8)
  };

  /**
   * @brief Create an isolated desktop for a seat.
   *
   * Opens or creates the shared "ApolloWinSta0" Window Station (idempotent),
   * then creates a "Seat_<id>" Desktop within it. The desktop's DACL grants
   * GENERIC_ALL to the current user and SYSTEM.
   *
   * @param seat_id The seat identifier used to name the desktop.
   * @return The desktop session, or std::nullopt on failure.
   */
  std::optional<desktop_session_t> create_isolated_desktop(const std::string &seat_id);

  /**
   * @brief Destroy an isolated desktop session.
   *
   * Closes the desktop handle and decrements the Window Station refcount.
   * When the last seat releases, the Window Station is also closed.
   *
   * @param session The desktop session to destroy.
   */
  void destroy_isolated_desktop(desktop_session_t &session);

}  // namespace platf::session_isolation
