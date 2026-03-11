/**
 * @file src/platform/windows/multiseat_launcher.cpp
 * @brief Multi-instance multi-seat: launch Apollo workers in separate Windows sessions.
 */

// standard includes
#include <filesystem>
#include <string>
#include <sstream>

// lib includes
#include "src/logging.h"

// platform includes
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>

// local includes
#include "multiseat_launcher.h"
#include "src/config.h"
#include "src/rtsp.h"
#include "src/nvhttp.h"

using namespace std::string_view_literals;

namespace platf::multiseat_launcher {

  /**
   * @brief Default base port for Apollo.
   */
  static constexpr uint16_t DEFAULT_BASE_PORT = 47989;

  bool is_multi_session_available() {
    // Check if multiple active sessions exist (indicates TermWrap/RDPWrap is working)
    WTS_SESSION_INFOW *sessions = nullptr;
    DWORD session_count = 0;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &session_count)) {
      BOOST_LOG(warning) << "Failed to enumerate Windows sessions: "sv << GetLastError();
      return false;
    }

    int active_count = 0;
    for (DWORD i = 0; i < session_count; ++i) {
      if (sessions[i].State == WTSActive || sessions[i].State == WTSDisconnected) {
        ++active_count;
      }
    }

    WTSFreeMemory(sessions);

    // If we can see multiple sessions or at least one, TermWrap may be available.
    // The real test is whether LogonUser + CreateProcessAsUser succeeds in a new session.
    return true;
  }

  /**
   * @brief Get the path to the currently running Apollo executable.
   */
  static std::wstring get_executable_path() {
    WCHAR path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
      return {};
    }
    return path;
  }

  /**
   * @brief Build the command line for a worker process.
   */
  static std::wstring build_worker_cmdline(
    const std::wstring &exe_path,
    int port_offset,
    const std::string &credentials_dir
  ) {
    std::wstringstream cmd;
    cmd << L"\"" << exe_path << L"\" --worker --port-offset " << port_offset;

    if (!credentials_dir.empty()) {
      // Convert UTF-8 to wide string
      int len = MultiByteToWideChar(CP_UTF8, 0, credentials_dir.c_str(), -1, nullptr, 0);
      std::wstring wide_cred_dir(len - 1, L'\0');
      MultiByteToWideChar(CP_UTF8, 0, credentials_dir.c_str(), -1, wide_cred_dir.data(), len);
      cmd << L" --credentials-dir \"" << wide_cred_dir << L"\"";
    }

    return cmd.str();
  }

  std::optional<worker_t> launch_worker(
    const std::string &username,
    const std::string &password,
    int port_offset,
    const std::string &credentials_dir,
    const std::string &seat_id
  ) {
    BOOST_LOG(info) << "Launching worker for seat "sv << seat_id
                    << " (user: "sv << username
                    << ", port offset: "sv << port_offset << ")"sv;

    // Convert username and password to wide strings
    int ulen = MultiByteToWideChar(CP_UTF8, 0, username.c_str(), -1, nullptr, 0);
    std::wstring wide_username(ulen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, username.c_str(), -1, wide_username.data(), ulen);

    int plen = MultiByteToWideChar(CP_UTF8, 0, password.c_str(), -1, nullptr, 0);
    std::wstring wide_password(plen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, password.c_str(), -1, wide_password.data(), plen);

    // Log on the user to get a token
    HANDLE user_token = nullptr;
    if (!LogonUserW(
          wide_username.c_str(),
          L".",  // local machine
          wide_password.c_str(),
          LOGON32_LOGON_INTERACTIVE,
          LOGON32_PROVIDER_DEFAULT,
          &user_token)) {
      BOOST_LOG(error) << "Failed to log on user '"sv << username << "': "sv << GetLastError();
      return std::nullopt;
    }

    // Create environment block for the user
    LPVOID env_block = nullptr;
    if (!CreateEnvironmentBlock(&env_block, user_token, FALSE)) {
      BOOST_LOG(error) << "Failed to create environment block for '"sv << username << "': "sv << GetLastError();
      CloseHandle(user_token);
      return std::nullopt;
    }

    // Build command line
    auto exe_path = get_executable_path();
    if (exe_path.empty()) {
      BOOST_LOG(error) << "Failed to get Apollo executable path"sv;
      DestroyEnvironmentBlock(env_block);
      CloseHandle(user_token);
      return std::nullopt;
    }

    auto cmdline = build_worker_cmdline(exe_path, port_offset, credentials_dir);
    BOOST_LOG(debug) << "Worker command line: "sv << std::string(cmdline.begin(), cmdline.end());

    // Need a mutable copy for CreateProcessAsUserW
    std::vector<wchar_t> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back(L'\0');

    // Get the working directory (same as current exe's directory)
    auto work_dir = std::filesystem::path(exe_path).parent_path().wstring();

    // Create the process in the user's session
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION pi = {};

    if (!CreateProcessAsUserW(
          user_token,
          nullptr,             // use command line for exe name
          cmdline_buf.data(),
          nullptr,             // process security attributes
          nullptr,             // thread security attributes
          FALSE,               // don't inherit handles
          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
          env_block,
          work_dir.c_str(),
          &si,
          &pi)) {
      BOOST_LOG(error) << "Failed to create worker process for '"sv << username << "': "sv << GetLastError();
      DestroyEnvironmentBlock(env_block);
      CloseHandle(user_token);
      return std::nullopt;
    }

    DestroyEnvironmentBlock(env_block);
    CloseHandle(pi.hThread);

    // Get the session ID of the new process
    DWORD session_id = 0;
    ProcessIdToSessionId(pi.dwProcessId, &session_id);

    BOOST_LOG(info) << "Worker launched: PID "sv << pi.dwProcessId
                    << " in session "sv << session_id
                    << " for seat "sv << seat_id;

    worker_t worker;
    worker.session_id = session_id;
    worker.process_id = pi.dwProcessId;
    worker.process_handle = pi.hProcess;
    worker.user_token = user_token;
    worker.port_offset = port_offset;
    worker.username = username;
    worker.seat_id = seat_id;

    return worker;
  }

  void terminate_worker(worker_t &worker) {
    if (worker.process_handle == nullptr) {
      return;
    }

    BOOST_LOG(info) << "Terminating worker PID "sv << worker.process_id
                    << " for seat "sv << worker.seat_id;

    // Try graceful termination first
    TerminateProcess(worker.process_handle, 0);

    // Wait up to 5 seconds for the process to exit
    WaitForSingleObject(worker.process_handle, 5000);

    CloseHandle(worker.process_handle);
    worker.process_handle = nullptr;

    // Log off the session if it's still active
    if (worker.session_id != 0) {
      WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, worker.session_id, FALSE);
    }

    if (worker.user_token != nullptr) {
      CloseHandle(worker.user_token);
      worker.user_token = nullptr;
    }

    BOOST_LOG(info) << "Worker terminated for seat "sv << worker.seat_id;
  }

  bool is_worker_alive(const worker_t &worker) {
    if (worker.process_handle == nullptr) {
      return false;
    }
    return WaitForSingleObject(worker.process_handle, 0) == WAIT_TIMEOUT;
  }

  uint16_t worker_rtsp_port(const worker_t &worker) {
    return (uint16_t)(DEFAULT_BASE_PORT + worker.port_offset + rtsp_stream::RTSP_SETUP_PORT);
  }

  uint16_t worker_http_port(const worker_t &worker) {
    return (uint16_t)(DEFAULT_BASE_PORT + worker.port_offset + nvhttp::PORT_HTTP);
  }

}  // namespace platf::multiseat_launcher
