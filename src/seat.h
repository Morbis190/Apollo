/**
 * @file src/seat.h
 * @brief Declarations for multi-seat management.
 *
 * A "seat" is a bundle of desktop resources (display, audio endpoint, input target)
 * assigned to one streaming session. In single-seat mode (default), a single default
 * seat transparently wraps the existing global configuration. In multi-seat mode,
 * each concurrent session gets its own isolated seat with dedicated resources.
 */
#pragma once

// local includes — forward declarations only
namespace proc {
  class proc_t;
}
#ifdef _WIN32
namespace platf::session_isolation {
  struct desktop_session_t;
}
namespace platf::multiseat_launcher {
  struct worker_t;
}
#endif

// standard includes
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <guiddef.h>
#endif

namespace seat {

  enum class state_e {
    AVAILABLE,   ///< Seat resources are allocated but not bound to a session
    BOUND,       ///< Seat is actively streaming
    RELEASING,   ///< Seat is being torn down
  };

  /**
   * @brief Represents an input routing target for a seat.
   */
  struct input_target_t {
    std::string display_name;  ///< For SendInput targeting (Windows)
  };

  /**
   * @brief A bundle of desktop resources assigned to one streaming session.
   */
  struct seat_t {
    std::string id;                    ///< Unique seat identifier
    std::string display_name;          ///< Video capture target (empty = use config default)
    std::string audio_sink_id;         ///< Audio capture target (empty = system default)
    input_target_t input_target;

#ifdef _WIN32
    std::optional<GUID> vdisplay_guid; ///< Virtual display GUID (Windows)

    /**
     * @brief Create a virtual display for this seat using SudoVDA.
     * @param client_uid Client unique ID for display identity.
     * @param client_name Client display name.
     * @param width Display width in pixels.
     * @param height Display height in pixels.
     * @param fps Display refresh rate (in mHz, e.g., 60000 = 60Hz).
     * @param guid The GUID to assign to the virtual display.
     * @return true if the virtual display was created successfully.
     *
     * Sets display_name, vdisplay_guid, and input_target on success.
     * No-op if the seat already owns a virtual display.
     */
    bool setup_virtual_display(
      const std::string &client_uid,
      const std::string &client_name,
      uint32_t width,
      uint32_t height,
      uint32_t fps,
      const GUID &guid
    );

    /**
     * @brief Adopt an existing virtual display created elsewhere (e.g., by proc).
     * @param guid The virtual display GUID.
     * @param name The display device name (UTF-8).
     *
     * Transfers ownership of the virtual display to this seat so it will be
     * cleaned up when the seat is released.
     */
    void adopt_virtual_display(const GUID &guid, const std::string &name);

    /**
     * @brief Tear down the virtual display owned by this seat.
     *
     * Removes the virtual display via SudoVDA and clears vdisplay_guid.
     * Safe to call even if no virtual display is owned.
     */
    void teardown_virtual_display();

    /**
     * @brief Isolated desktop session for this seat (multi-seat mode only).
     *
     * When multi-seat is enabled, each seat gets its own Windows Desktop
     * within a shared Window Station. Processes launched for this seat
     * target this desktop via STARTUPINFO.lpDesktop.
     *
     * nullptr in single-seat mode.
     */
    std::unique_ptr<platf::session_isolation::desktop_session_t> desktop_session;

    /**
     * @brief Worker process for multi-instance mode.
     *
     * When multiseat_mode is "multi_instance", each seat launches a separate
     * Apollo worker process in its own Windows session. This tracks that worker.
     *
     * nullptr in single-seat mode or desktop_object mode.
     */
    std::unique_ptr<platf::multiseat_launcher::worker_t> worker;
#endif

    state_e state = state_e::AVAILABLE;

    std::string client_uuid;  ///< UUID of the client currently bound to this seat (empty when available)

    /**
     * @brief Process context for this seat.
     *
     * In single-seat mode, this points to the global `proc::proc` singleton.
     * In multi-seat mode, each seat owns its own proc_t instance via `_owned_process`
     * to allow independent app execution per seat.
     */
    proc::proc_t *process = nullptr;

    /**
     * @brief Owned process context for multi-seat mode.
     *
     * When set, this seat owns the proc_t and `process` points to it.
     * In single-seat mode this is nullptr and `process` points to `proc::proc`.
     */
    std::unique_ptr<proc::proc_t> _owned_process;

    /**
     * @brief The session currently bound to this seat.
     * Weak pointer to avoid circular references with session_t.
     */
    std::weak_ptr<void> bound_session;
  };

  using seat_ptr = std::shared_ptr<seat_t>;

  /**
   * @brief Creates a default seat that mirrors current single-user behavior.
   *
   * The default seat uses empty display_name and audio_sink_id, which causes
   * all subsystems to fall back to their existing global configuration.
   */
  seat_ptr make_default_seat();

  /**
   * @brief Manages seat allocation, tracking, and release.
   *
   * In single-seat mode, acquire() always returns the same default seat.
   * In multi-seat mode, it allocates from a pool or creates new virtual displays.
   */
  class manager_t {
  public:
    /**
     * @brief Initialize the seat manager from configuration.
     * @param multi_seat Whether multi-seat mode is enabled.
     * @param max_seats Maximum number of concurrent seats.
     */
    void init(bool multi_seat, int max_seats);

    /**
     * @brief Acquire a seat for a new session.
     * @return A seat pointer, or nullptr if no seat is available.
     */
    seat_ptr acquire();

    /**
     * @brief Release a seat when a session ends.
     * @param seat The seat to release.
     */
    void release(const seat_ptr &seat);

    /**
     * @brief Get all active (non-available) seats.
     * @return Vector of seat pointers.
     */
    std::vector<seat_ptr> active_seats() const;

    /**
     * @brief Get all seats (regardless of state).
     * @return Vector of all seat pointers.
     */
    std::vector<seat_ptr> all_seats() const;

    /**
     * @brief Check if multi-seating is enabled.
     * @return true if multi-seat mode is active.
     */
    bool multi_seat_enabled() const;

    /**
     * @brief Get the maximum number of seats.
     * @return Maximum seat count.
     */
    int max_seats() const;

    /**
     * @brief Force-release a seat by ID.
     * @param seat_id The seat ID to release.
     * @return true if a seat was found and released.
     */
    bool force_release(const std::string &seat_id);

    /**
     * @brief Find a seat bound to a specific client.
     * @param client_uuid The client's certificate UUID.
     * @return Seat pointer, or nullptr if not found.
     */
    seat_ptr find_by_client(const std::string &client_uuid) const;

  private:
    mutable std::mutex _mutex;
    std::vector<seat_ptr> _seats;
    seat_ptr _default_seat;  ///< Singleton seat for single-seat mode
    bool _multi_seat = false;  ///< Driven by config
    int _max_seats = 4;
  };

  /**
   * @brief Global seat manager instance.
   */
  extern manager_t manager;

}  // namespace seat
