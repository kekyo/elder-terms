#include <elder-terms/serial-device.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace elder_terms {

struct SerialDeviceMetadata {
  std::optional<std::string> product_label;
  std::optional<std::string> usb_serial;
};

static std::filesystem::path environment_path(const char *name,
                                              const char *fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  return value;
}

static bool starts_with(const std::string &value,
                        const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

static bool string_is_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

static std::string trim_ascii_whitespace(std::string value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return std::isspace(character) != 0; });
  const auto last = std::find_if_not(
                        value.rbegin(), value.rend(),
                        [](unsigned char character) {
                          return std::isspace(character) != 0;
                        })
                        .base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

static std::optional<std::string>
read_text_file_trimmed(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input.good()) {
    return std::nullopt;
  }
  std::string value((std::istreambuf_iterator<char>(input)),
                    std::istreambuf_iterator<char>());
  value = trim_ascii_whitespace(std::move(value));
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

static std::optional<std::string>
read_uevent_value(const std::filesystem::path &path,
                  const std::string &key) {
  std::ifstream input(path);
  if (!input.good()) {
    return std::nullopt;
  }
  const std::string prefix = key + "=";
  std::string line;
  while (std::getline(input, line)) {
    if (starts_with(line, prefix)) {
      const std::string value = line.substr(prefix.size());
      return value.empty() ? std::nullopt
                           : std::optional<std::string>(value);
    }
  }
  return std::nullopt;
}

static bool path_has_parent(const std::filesystem::path &path,
                            const std::filesystem::path &expected_parent) {
  const std::filesystem::path normalized_path = path.lexically_normal();
  const std::filesystem::path normalized_parent =
      expected_parent.lexically_normal();
  if (normalized_path.parent_path() == normalized_parent) {
    return true;
  }

  std::error_code error;
  const std::filesystem::path canonical_parent =
      std::filesystem::weakly_canonical(normalized_parent, error);
  if (error) {
    return false;
  }
  error.clear();
  const std::filesystem::path canonical_path_parent =
      std::filesystem::weakly_canonical(normalized_path.parent_path(), error);
  return !error && canonical_path_parent == canonical_parent;
}

static bool is_serial_candidate_name(const std::string &name) {
  static const std::vector<std::string> prefixes = {
      "ttyS",   "ttyUSB", "ttyACM", "ttyAMA",
      "ttyAP",  "ttyMX",  "rfcomm",
  };
  return std::any_of(prefixes.begin(), prefixes.end(),
                     [&name](const std::string &prefix) {
                       return starts_with(name, prefix);
                     });
}

static bool is_character_device(const std::filesystem::path &path) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::status(path, error);
  return !error && std::filesystem::is_character_file(status);
}

static std::optional<std::string>
resolve_current_node(const std::filesystem::path &target_path) {
  if (target_path.empty()) {
    return std::nullopt;
  }
  std::error_code error;
  if (!std::filesystem::exists(target_path, error) || error) {
    return std::nullopt;
  }
  const std::filesystem::path canonical =
      std::filesystem::canonical(target_path, error);
  if (!error) {
    return canonical.string();
  }
  return target_path.string();
}

static SerialDeviceMetadata
load_serial_device_metadata(const std::optional<std::string> &current_node,
                            const SerialDevicePaths &paths) {
  if (!current_node.has_value() || current_node->empty()) {
    return {};
  }

  std::error_code error;
  std::filesystem::path cursor =
      paths.sys_class_tty_root /
      std::filesystem::path(*current_node).filename() / "device";
  cursor = std::filesystem::weakly_canonical(cursor, error);
  if (error) {
    return {};
  }

  SerialDeviceMetadata metadata;
  while (!cursor.empty()) {
    if (!metadata.usb_serial.has_value()) {
      metadata.usb_serial = read_text_file_trimmed(cursor / "serial");
      if (!metadata.usb_serial.has_value()) {
        metadata.usb_serial =
            read_uevent_value(cursor / "uevent", "ID_SERIAL_SHORT");
      }
    }
    if (!metadata.product_label.has_value()) {
      metadata.product_label = read_text_file_trimmed(cursor / "product");
      if (!metadata.product_label.has_value()) {
        metadata.product_label = read_text_file_trimmed(cursor / "interface");
      }
    }
    const std::filesystem::path parent = cursor.parent_path();
    if (parent == cursor) {
      break;
    }
    cursor = parent;
  }
  return metadata;
}

static std::string abbreviate_usb_serial(const std::string &value) {
  if (value.size() <= 12) {
    return value;
  }
  return value.substr(0, 4) + "..." + value.substr(value.size() - 4);
}

static std::string
device_display_label(SerialDeviceMatchMode mode,
                     const std::filesystem::path &target_path,
                     const SerialDeviceMetadata &metadata) {
  const std::string fallback =
      mode == SerialDeviceMatchMode::exact_path
          ? target_path.string()
          : target_path.filename().string();
  if (mode != SerialDeviceMatchMode::stable_id ||
      (!metadata.product_label.has_value() &&
       !metadata.usb_serial.has_value())) {
    return fallback;
  }

  std::string label = metadata.product_label.value_or(fallback);
  if (metadata.usb_serial.has_value()) {
    label += " [SN:" + abbreviate_usb_serial(*metadata.usb_serial) + "]";
  }
  return label;
}

static SerialDeviceChoice build_choice(SerialDeviceMatchMode mode,
                                       const std::filesystem::path &target,
                                       const SerialDevicePaths &paths) {
  const std::optional<std::string> current_node = resolve_current_node(target);
  const SerialDeviceMetadata metadata =
      load_serial_device_metadata(current_node, paths);
  return {
      .target_path = target.string(),
      .display_label = device_display_label(mode, target, metadata),
      .current_node = current_node,
      .usb_serial = metadata.usb_serial,
  };
}

static bool target_matches_mode(SerialDeviceMatchMode mode,
                                const std::filesystem::path &target_path,
                                const SerialDevicePaths &paths) {
  if (mode == SerialDeviceMatchMode::exact_path) {
    return path_has_parent(target_path, paths.dev_root) &&
           is_serial_candidate_name(target_path.filename().string());
  }
  if (mode == SerialDeviceMatchMode::stable_id) {
    return path_has_parent(target_path, paths.by_id_root);
  }
  return path_has_parent(target_path, paths.by_path_root);
}

static std::optional<SerialDeviceChoice>
find_choice_for_node(SerialDeviceMatchMode mode, const std::string &node_path,
                     const SerialDevicePaths &paths) {
  const std::vector<SerialDeviceChoice> choices =
      list_serial_device_choices(mode, paths);
  const auto match = std::find_if(
      choices.begin(), choices.end(),
      [&node_path](const SerialDeviceChoice &choice) {
        return choice.current_node == node_path ||
               choice.target_path == node_path;
      });
  if (match == choices.end()) {
    return std::nullopt;
  }
  return *match;
}

static std::optional<SerialDeviceChoice>
find_choice_for_target(SerialDeviceMatchMode mode,
                       const std::string &target_path,
                       const SerialDevicePaths &paths) {
  const std::vector<SerialDeviceChoice> choices =
      list_serial_device_choices(mode, paths);
  const std::filesystem::path candidate(target_path);
  const auto match = std::find_if(
      choices.begin(), choices.end(),
      [&target_path, &candidate](const SerialDeviceChoice &choice) {
        return choice.target_path == target_path ||
               (!candidate.has_parent_path() &&
                std::filesystem::path(choice.target_path).filename() ==
                    candidate);
      });
  if (match == choices.end()) {
    return std::nullopt;
  }
  return *match;
}

std::string serial_device_match_mode_to_string(SerialDeviceMatchMode mode) {
  if (mode == SerialDeviceMatchMode::stable_id) {
    return "by-id";
  }
  if (mode == SerialDeviceMatchMode::physical_port) {
    return "by-path";
  }
  return "path";
}

std::optional<SerialDeviceMatchMode>
parse_serial_device_match_mode(const std::string &value) {
  if (value == "path") {
    return SerialDeviceMatchMode::exact_path;
  }
  if (value == "by-id") {
    return SerialDeviceMatchMode::stable_id;
  }
  if (value == "by-path") {
    return SerialDeviceMatchMode::physical_port;
  }
  return std::nullopt;
}

SerialDevicePaths host_serial_device_paths() {
  return {
      .dev_root = environment_path("ELDER_TERMS_SERIAL_DEV_ROOT", "/dev"),
      .by_id_root = environment_path("ELDER_TERMS_SERIAL_BY_ID_ROOT",
                                     "/dev/serial/by-id"),
      .by_path_root = environment_path("ELDER_TERMS_SERIAL_BY_PATH_ROOT",
                                       "/dev/serial/by-path"),
      .sys_class_tty_root = environment_path(
          "ELDER_TERMS_SERIAL_SYS_CLASS_TTY_ROOT", "/sys/class/tty"),
  };
}

std::vector<SerialDeviceChoice>
list_serial_device_choices(SerialDeviceMatchMode mode,
                           const SerialDevicePaths &paths) {
  std::vector<SerialDeviceChoice> choices;
  std::error_code error;
  const std::filesystem::path root =
      mode == SerialDeviceMatchMode::exact_path
          ? paths.dev_root
          : mode == SerialDeviceMatchMode::stable_id ? paths.by_id_root
                                                      : paths.by_path_root;
  if (!std::filesystem::is_directory(root, error) || error) {
    return choices;
  }

  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(root, error)) {
    if (error) {
      break;
    }
    if (mode == SerialDeviceMatchMode::exact_path) {
      const std::string name = entry.path().filename().string();
      if (is_serial_candidate_name(name) &&
          can_open_serial_device_target(entry.path().string())) {
        choices.push_back(build_choice(mode, entry.path(), paths));
      }
      continue;
    }

    error.clear();
    const bool supported = entry.is_symlink(error) || entry.is_regular_file(error);
    if (!error && supported) {
      choices.push_back(build_choice(mode, entry.path(), paths));
    }
  }

  std::sort(choices.begin(), choices.end(),
            [](const SerialDeviceChoice &left,
               const SerialDeviceChoice &right) {
              if (left.display_label != right.display_label) {
                return left.display_label < right.display_label;
              }
              return left.target_path < right.target_path;
            });
  return choices;
}

std::vector<SerialDeviceChoice>
list_serial_device_choices(SerialDeviceMatchMode mode) {
  return list_serial_device_choices(mode, host_serial_device_paths());
}

std::optional<std::string>
resolve_serial_device_current_node(const std::string &target_path) {
  return resolve_current_node(target_path);
}

std::optional<std::string>
resolve_serial_device_target_for_mode(SerialDeviceMatchMode mode,
                                      const std::string &target_path,
                                      const SerialDevicePaths &paths) {
  if (target_path.empty()) {
    return std::nullopt;
  }

  const std::filesystem::path path(target_path);
  if (target_matches_mode(mode, path, paths)) {
    return path.string();
  }

  const std::optional<SerialDeviceChoice> direct_choice =
      find_choice_for_target(mode, target_path, paths);
  if (direct_choice.has_value()) {
    return direct_choice->target_path;
  }

  const std::optional<std::string> current_node =
      resolve_serial_device_current_node(target_path);
  if (!current_node.has_value()) {
    return std::nullopt;
  }
  const std::optional<SerialDeviceChoice> choice =
      find_choice_for_node(mode, *current_node, paths);
  return choice.has_value() ? std::optional<std::string>(choice->target_path)
                            : std::nullopt;
}

std::optional<std::string>
resolve_serial_device_target_for_mode(SerialDeviceMatchMode mode,
                                      const std::string &target_path) {
  return resolve_serial_device_target_for_mode(mode, target_path,
                                               host_serial_device_paths());
}

SerialDeviceResolveResult resolve_serial_device(
    SerialDeviceMatchMode mode, const std::string &target_path,
    const std::optional<std::string> &usb_serial,
    const SerialDevicePaths &paths) {
  if (string_is_blank(target_path)) {
    return {
        .resolved = false,
        .path = {},
        .warnings = {"Warning: missing required configuration value [serial] "
                     "device; serial session will not connect"},
    };
  }

  std::optional<std::string> resolved_target =
      resolve_serial_device_target_for_mode(mode, target_path, paths);
  if (mode == SerialDeviceMatchMode::stable_id && usb_serial.has_value() &&
      !usb_serial->empty()) {
    const std::vector<SerialDeviceChoice> choices =
        list_serial_device_choices(mode, paths);
    const auto selected = std::find_if(
        choices.begin(), choices.end(),
        [&resolved_target](const SerialDeviceChoice &choice) {
          return resolved_target.has_value() &&
                 choice.target_path == *resolved_target;
        });
    if (selected == choices.end() || selected->usb_serial != usb_serial) {
      std::vector<SerialDeviceChoice> serial_matches;
      std::copy_if(choices.begin(), choices.end(),
                   std::back_inserter(serial_matches),
                   [&usb_serial](const SerialDeviceChoice &choice) {
                     return choice.usb_serial == usb_serial;
                   });
      if (serial_matches.size() > 1) {
        return {
            .resolved = false,
            .path = {},
            .warnings = {
                "Warning: serial USB identity matched multiple devices: " +
                *usb_serial,
            },
        };
      }
      if (serial_matches.size() == 1) {
        resolved_target = serial_matches.front().target_path;
      }
    }
  }

  const std::filesystem::path candidate =
      resolved_target.value_or(target_path);
  if (is_character_device(candidate)) {
    return {
        .resolved = true,
        .path = candidate,
        .warnings = {},
    };
  }
  return {
      .resolved = false,
      .path = {},
      .warnings = {"Warning: serial device not found: " + target_path},
  };
}

SerialDeviceResolveResult resolve_serial_device(
    SerialDeviceMatchMode mode, const std::string &target_path,
    const std::optional<std::string> &usb_serial) {
  return resolve_serial_device(mode, target_path, usb_serial,
                               host_serial_device_paths());
}

bool can_open_serial_device_target(const std::string &target_path) {
  const int file_descriptor =
      open(target_path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (file_descriptor < 0) {
    return false;
  }
  termios settings{};
  const bool usable = tcgetattr(file_descriptor, &settings) == 0;
  close(file_descriptor);
  return usable;
}

} // namespace elder_terms
