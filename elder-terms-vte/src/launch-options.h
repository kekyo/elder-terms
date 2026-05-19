#pragma once

#include <filesystem>
#include <optional>

namespace elder_terms {

/**
 * Options that alter behavior for the GTK integration test harness.
 */
struct TestOptions {
  /** True when the app should render deterministic terminal fixture text. */
  bool fixture = false;
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
