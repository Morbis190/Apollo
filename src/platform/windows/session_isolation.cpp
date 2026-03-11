/**
 * @file src/platform/windows/session_isolation.cpp
 * @brief Implementation of per-seat Windows desktop isolation.
 */

// local includes
#include "session_isolation.h"
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"

// lib includes
#include <sddl.h>

// standard includes
#include <atomic>
#include <mutex>

using namespace std::literals;

namespace platf::session_isolation {

  static constexpr const wchar_t *WINSTA_NAME = L"ApolloWinSta0";
  static constexpr const char *WINSTA_NAME_UTF8 = "ApolloWinSta0";

  // Shared Window Station state
  static std::mutex winsta_mutex;
  static HWINSTA shared_winsta = nullptr;
  static std::atomic<int> winsta_refcount {0};

  /**
   * @brief Build a security descriptor granting GENERIC_ALL to current user and SYSTEM.
   * @return A SECURITY_ATTRIBUTES pointer (caller must LocalFree the descriptor), or nullptr on failure.
   */
  static PSECURITY_DESCRIPTOR
  create_permissive_sd() {
    // SDDL: D = DACL
    //   (A;;GA;;;SY) = Allow GENERIC_ALL to SYSTEM
    //   (A;;GA;;;BA) = Allow GENERIC_ALL to Administrators
    //   (A;;GA;;;IU) = Allow GENERIC_ALL to Interactive Users
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)",
          SDDL_REVISION_1,
          &sd,
          nullptr)) {
      BOOST_LOG(error) << "Failed to create security descriptor: "sv << GetLastError();
      return nullptr;
    }
    return sd;
  }

  /**
   * @brief Open or create the shared Window Station.
   * @return The Window Station handle, or nullptr on failure.
   */
  static HWINSTA
  ensure_window_station() {
    std::lock_guard lg(winsta_mutex);

    if (shared_winsta) {
      winsta_refcount.fetch_add(1, std::memory_order_relaxed);
      return shared_winsta;
    }

    // Try to open existing first
    shared_winsta = OpenWindowStationW(WINSTA_NAME, FALSE, WINSTA_ALL_ACCESS);
    if (!shared_winsta) {
      // Create with permissive security
      auto sd = create_permissive_sd();
      SECURITY_ATTRIBUTES sa {};
      sa.nLength = sizeof(sa);
      sa.lpSecurityDescriptor = sd;
      sa.bInheritHandle = FALSE;

      shared_winsta = CreateWindowStationW(WINSTA_NAME, 0, WINSTA_ALL_ACCESS, &sa);

      if (sd) {
        LocalFree(sd);
      }

      if (!shared_winsta) {
        BOOST_LOG(error) << "Failed to create Window Station: "sv << GetLastError();
        return nullptr;
      }

      BOOST_LOG(info) << "Created Window Station: "sv << WINSTA_NAME_UTF8;
    }
    else {
      BOOST_LOG(info) << "Opened existing Window Station: "sv << WINSTA_NAME_UTF8;
    }

    winsta_refcount.store(1, std::memory_order_relaxed);
    return shared_winsta;
  }

  /**
   * @brief Release a reference to the shared Window Station.
   *
   * Closes the Window Station when the last reference is released.
   */
  static void
  release_window_station() {
    std::lock_guard lg(winsta_mutex);

    if (winsta_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      if (shared_winsta) {
        CloseWindowStation(shared_winsta);
        shared_winsta = nullptr;
        BOOST_LOG(info) << "Closed Window Station: "sv << WINSTA_NAME_UTF8;
      }
    }
  }

  std::optional<desktop_session_t>
  create_isolated_desktop(const std::string &seat_id) {
    auto winsta = ensure_window_station();
    if (!winsta) {
      return std::nullopt;
    }

    // Save the current Window Station and switch to ours for desktop creation
    HWINSTA prev_winsta = GetProcessWindowStation();
    if (!SetProcessWindowStation(winsta)) {
      BOOST_LOG(error) << "Failed to set process Window Station: "sv << GetLastError();
      release_window_station();
      return std::nullopt;
    }

    // Create the desktop name
    std::wstring desktop_label = L"Seat_" + platf::from_utf8(seat_id);

    // Create with permissive security
    auto sd = create_permissive_sd();
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    HDESK desktop = CreateDesktopW(
      desktop_label.c_str(),
      nullptr,
      nullptr,
      0,
      GENERIC_ALL,
      &sa
    );

    if (sd) {
      LocalFree(sd);
    }

    // Restore the original Window Station
    SetProcessWindowStation(prev_winsta);

    if (!desktop) {
      BOOST_LOG(error) << "Failed to create desktop for seat "sv << seat_id << ": "sv << GetLastError();
      release_window_station();
      return std::nullopt;
    }

    // Build the full desktop name: "ApolloWinSta0\\Seat_<id>"
    std::string full_name = std::string(WINSTA_NAME_UTF8) + "\\" + "Seat_" + seat_id;

    BOOST_LOG(info) << "Created isolated desktop: "sv << full_name;

    return desktop_session_t {
      .winsta = winsta,
      .desktop = desktop,
      .desktop_name = std::move(full_name),
    };
  }

  void
  destroy_isolated_desktop(desktop_session_t &session) {
    if (session.desktop) {
      CloseDesktop(session.desktop);
      BOOST_LOG(info) << "Closed desktop: "sv << session.desktop_name;
      session.desktop = nullptr;
    }

    if (session.winsta) {
      release_window_station();
      session.winsta = nullptr;
    }

    session.desktop_name.clear();
  }

}  // namespace platf::session_isolation
