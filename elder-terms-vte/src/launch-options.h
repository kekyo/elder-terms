#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace elder_terms {

/**
 * Options that alter behavior for the GTK integration test harness.
 */
struct TestOptions {
  /** True when the app should render deterministic terminal fixture text. */
  bool fixture = false;
  /** True when blink activity indicators should stay lit until reset. */
  bool latch_activity_indicators = false;
  /** True when transfer dialogs should report their current folder URI. */
  bool transfer_dialog_probe = false;
  /** Optional deterministic SSH prompt rendered by the GTK test fixture. */
  std::optional<std::string> ssh_prompt;
  /** Explicit SSH known_hosts file used by connection integration tests. */
  std::string ssh_known_hosts_file;
  /** Source file URIs used instead of opening the send file dialog. */
  std::vector<std::string> transfer_source_uris;
  /** True when the SFTP fixture should pause remote writes until cancelled. */
  bool sftp_pause_transfer = false;
  /** True when the main-window transfer progress fixture should be visible. */
  bool show_transfer_progress = false;
  /** True when an integrated fixture SFTP window starts disconnected. */
  bool shared_sftp_disconnected = false;
  /** True when opening fixture SFTP leaves main-window focus on Transfer. */
  bool focus_transfer_on_sftp_open = false;
  /** True when the test harness should emulate a maximized window resize. */
  bool maximize_window = false;
};

/**
 * Command-line options consumed before GTK receives argv.
 */
struct LaunchOptions {
  /** Test harness options. */
  TestOptions test;
  /** Optional INI configuration path passed with -c. */
  std::optional<std::filesystem::path> config_path;
  /** Optional read-only startup INI configuration path passed with -s. */
  std::optional<std::filesystem::path> startup_config_path;
};

/**
 * Parses application launch options and compacts argv for GTK.
 *
 * @param argc Argument count pointer updated after consumed options are removed.
 * @param argv Argument vector compacted in place.
 * @returns Parsed launch options.
 */
LaunchOptions parse_launch_options(int *argc, char **argv);

} // namespace elder_terms
