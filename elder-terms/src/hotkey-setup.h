#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <elder-terms/key-binding.h>

namespace elder_terms {

/** One compositor shortcut that invokes an elder-terms control command. */
struct ExternalHotkeyCommand {
  /** Configured key and modifiers. */
  KeyBinding binding;
  /** Command and arguments passed to the desktop's command launcher. */
  std::vector<std::string> arguments;
};

/** Facts about the active graphical session and its configuration search path. */
struct ExternalHotkeySetupEnvironment {
  /** Whether the session is Wayland rather than X11. */
  bool wayland;
  /** User configuration directory. */
  std::filesystem::path config_home;
  /** User home directory for legacy Sway configuration paths. */
  std::filesystem::path home;
  /** System configuration directories in search order. */
  std::vector<std::filesystem::path> config_dirs;
  /** Sway IPC socket exported by the active compositor, if any. */
  std::string sway_socket;
  /** PID exported by an active labwc session, if any. */
  std::string labwc_pid;
};

/** Result of changing shortcuts in an external desktop configuration. */
struct ExternalHotkeySetupResult {
  /** Whether the requested operation completed successfully. */
  bool success;
  /** Human-readable transport and outcome or reason for failure. */
  std::string message;
};

/** Output of one compositor command. */
struct ExternalHotkeyCommandResult {
  /** Whether the command exited successfully. */
  bool success;
  /** Standard output returned by the compositor. */
  std::string output;
};

/** Runs a compositor command without a shell. */
using ExternalHotkeyCommandRunner =
    std::function<ExternalHotkeyCommandResult(const std::vector<std::string> &)>;

/**
 * Installs current hotkeys in a supported Wayland compositor's user config.
 * Repeated calls replace only elder-terms bindings, then reload the compositor.
 *
 * @param environment Session capabilities and config search path.
 * @param actions Ordered active shortcuts and their control commands.
 * @param run Executes a compositor IPC/reload command without a shell.
 * @returns Success only after the active compositor accepts a reload.
 */
ExternalHotkeySetupResult setup_external_hotkeys(
    const ExternalHotkeySetupEnvironment &environment,
    const std::vector<ExternalHotkeyCommand> &actions,
    const ExternalHotkeyCommandRunner &run);

/**
 * Removes hotkeys previously saved by setup from user compositor files.
 *
 * @param environment Session capabilities and user config paths.
 * @param run Executes a compositor reload command without a shell.
 * @returns Whether managed entries were removed or already absent.
 */
ExternalHotkeySetupResult unsetup_external_hotkeys(
    const ExternalHotkeySetupEnvironment &environment,
    const ExternalHotkeyCommandRunner &run);

} // namespace elder_terms
