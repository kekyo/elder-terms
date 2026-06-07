#include <iostream>
#include <string>

#include "launch-options.h"

namespace elder_terms {

static void consume_path_option(int *index, int argc, char **argv,
                                const char *option,
                                std::optional<std::filesystem::path> *path) {
  if (*index + 1 >= argc) {
    std::cerr << "Warning: " << option
              << " requires a configuration file path; using defaults" << '\n';
    ++*index;
    return;
  }

  const std::string config_path = argv[*index + 1];
  if (config_path.empty()) {
    std::cerr << "Warning: " << option
              << " requires a configuration file path; using defaults" << '\n';
  } else {
    *path = std::filesystem::path(config_path);
  }
  *index += 2;
}

LaunchOptions parse_launch_options(int *argc, char **argv) {
  LaunchOptions options;
  int write_index = 1;

  for (int index = 1; index < *argc;) {
    const std::string argument = argv[index];
    if (argument == "--test-fixture") {
      options.test.fixture = true;
      ++index;
      continue;
    }

    if (argument == "--test-transfer-dialog-probe") {
      options.test.transfer_dialog_probe = true;
      ++index;
      continue;
    }

    static constexpr const char transfer_source_uri_option[] =
        "--test-transfer-source-uri=";
    if (argument.rfind(transfer_source_uri_option, 0) == 0) {
      options.test.transfer_source_uris.push_back(
          argument.substr(std::char_traits<char>::length(
              transfer_source_uri_option)));
      ++index;
      continue;
    }

    if (argument == "-c") {
      consume_path_option(&index, *argc, argv, "-c", &options.config_path);
      continue;
    }

    if (argument == "-s") {
      consume_path_option(&index, *argc, argv, "-s",
                          &options.startup_config_path);
      continue;
    }

    argv[write_index] = argv[index];
    ++write_index;
    ++index;
  }
  argv[write_index] = nullptr;
  *argc = write_index;

  return options;
}

} // namespace elder_terms
