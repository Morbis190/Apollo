/**
 * @file src/platform/windows/multiseat_launcher.h
 * @brief Multi-instance multi-seat: launch Apollo workers in separate Windows sessions.
 *
 * Requires TermWrap or RDPWrap to enable concurrent RDP sessions on
 * Windows desktop editions. Each worker runs in its own logon session
 * with full OS-level isolation (display, input, audio, processes).
 */
#pragma once

#include <optional>
#include <string>
#include <vector>
#include <mutex>

#include <windows.h>

namespace platf::multiseat_launcher {

  /**
   * @brief Tracks a running Apollo worker process in a Windows session.
   */
  struct worker_t {
    DWORD session_id = 0;           ///< Windows Terminal Services session ID
    DWORD process_id = 0;           ///< Worker process ID
    HANDLE process_handle = nullptr; ///< Handle to worker process (for wait/terminate)
    HANDLE user_token = nullptr;     ///< Logon token (must be closed on cleanup)
    int port_offset = 0;            ///< Port offset assigned to this worker
    std::string username;           ///< Windows account name
    std::string seat_id;            ///< Associated seat ID
  };

  /**
   * @brief Check if concurrent Windows sessions are available.
   *
   * Checks for TermWrap/RDPWrap by looking for the patched termsrv.dll
   * or checking if multiple interactive sessions exist.
   *
   * @return true if multi-session is likely available.
   */
  bool is_multi_session_available();

  /**
   * @brief Launch an Apollo worker in a new Windows logon session.
   *
   * Uses LogonUserW() to authenticate the specified account and
   * CreateProcessAsUserW() to start Apollo.exe with --worker flags
   * in the new session context.
   *
   * @param username Local Windows account name.
   * @param password Account password.
   * @param port_offset Port offset for this worker (e.g., 100, 200).
   * @param credentials_dir Shared credentials directory path.
   * @param seat_id Seat identifier for logging.
   * @return Worker info on success, or std::nullopt on failure.
   */
  std::optional<worker_t> launch_worker(
    const std::string &username,
    const std::string &password,
    int port_offset,
    const std::string &credentials_dir,
    const std::string &seat_id
  );

  /**
   * @brief Terminate a running worker and clean up its session.
   *
   * Sends a termination signal, waits briefly for graceful shutdown,
   * then force-terminates if necessary. Logs off the session.
   *
   * @param worker The worker to terminate.
   */
  void terminate_worker(worker_t &worker);

  /**
   * @brief Check if a worker process is still running.
   * @param worker The worker to check.
   * @return true if the worker process is alive.
   */
  bool is_worker_alive(const worker_t &worker);

  /**
   * @brief Get the RTSP port for a worker (base port + RTSP offset).
   * @param worker The worker.
   * @return The RTSP port number.
   */
  uint16_t worker_rtsp_port(const worker_t &worker);

  /**
   * @brief Get the HTTP port for a worker (base port).
   * @param worker The worker.
   * @return The HTTP port number.
   */
  uint16_t worker_http_port(const worker_t &worker);

}  // namespace platf::multiseat_launcher
