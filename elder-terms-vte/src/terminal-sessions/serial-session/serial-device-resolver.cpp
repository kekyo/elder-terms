#include "serial-device-resolver.h"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace elder_terms {

static bool string_is_blank(const std::string &value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  });
}

static bool contains_slash(const std::string &value) {
  return value.find('/') != std::string::npos;
}

static bool starts_with(const std::string &value, const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

static bool path_exists(const std::filesystem::path &path) {
  std::error_code error;
  return std::filesystem::exists(path, error) && !error;
}

static bool is_character_device(const std::filesystem::path &path) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::status(path, error);
  return !error && std::filesystem::is_character_file(status);
}

static bool is_tty_name(const std::string &name) {
  return starts_with(name, "tty");
}

static std::filesystem::path canonical_key(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(path,
                                                                     error);
  if (error) {
    return path.lexically_normal();
  }
  return resolved.lexically_normal();
}

static void add_unique_candidate(
    std::map<std::string, std::filesystem::path> *candidates,
    const std::filesystem::path &path) {
  const std::string key = canonical_key(path).string();
  if (candidates->find(key) == candidates->end()) {
    candidates->emplace(key, path);
  }
}

static std::optional<std::string>
uevent_value(const std::filesystem::path &uevent_path,
             const std::string &key) {
  std::ifstream file(uevent_path);
  if (!file.good()) {
    return std::nullopt;
  }

  std::string line;
  const std::string prefix = key + "=";
  while (std::getline(file, line)) {
    if (starts_with(line, prefix)) {
      return line.substr(prefix.size());
    }
  }
  return std::nullopt;
}

static bool sysfs_tty_identifier_matches(
    const std::filesystem::path &sysfs_root, const std::string &tty_name,
    const std::string &selector) {
  const std::filesystem::path uevent_path =
      sysfs_root / "class" / "tty" / tty_name / "device" / "uevent";
  const std::optional<std::string> id_serial =
      uevent_value(uevent_path, "ID_SERIAL");
  if (id_serial.has_value() && id_serial.value() == selector) {
    return true;
  }

  const std::optional<std::string> id_serial_short =
      uevent_value(uevent_path, "ID_SERIAL_SHORT");
  return id_serial_short.has_value() && id_serial_short.value() == selector;
}

static void add_by_id_candidates(
    std::map<std::string, std::filesystem::path> *candidates,
    const std::filesystem::path &dev_root, const std::string &selector) {
  const std::filesystem::path by_id_root = dev_root / "serial" / "by-id";
  std::error_code error;
  if (!std::filesystem::is_directory(by_id_root, error) || error) {
    return;
  }

  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(by_id_root, error)) {
    if (error) {
      return;
    }
    if (entry.path().filename().string() == selector) {
      add_unique_candidate(candidates, entry.path());
    }
  }
}

static void add_sysfs_identifier_candidates(
    std::map<std::string, std::filesystem::path> *candidates,
    const std::filesystem::path &dev_root,
    const std::filesystem::path &sysfs_root, const std::string &selector) {
  std::error_code error;
  if (!std::filesystem::is_directory(dev_root, error) || error) {
    return;
  }

  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(dev_root, error)) {
    if (error) {
      return;
    }

    const std::string name = entry.path().filename().string();
    if (!is_tty_name(name) || !is_character_device(entry.path())) {
      continue;
    }
    if (sysfs_tty_identifier_matches(sysfs_root, name, selector)) {
      add_unique_candidate(candidates, entry.path());
    }
  }
}

static SerialDeviceResolveResult resolve_one_candidate(
    const std::string &selector,
    const std::map<std::string, std::filesystem::path> &candidates) {
  if (candidates.empty()) {
    return {
        .resolved = false,
        .path = {},
        .warnings = {"Warning: serial device not found: " + selector},
    };
  }

  if (candidates.size() > 1) {
    return {
        .resolved = false,
        .path = {},
        .warnings = {"Warning: serial device selector matched multiple "
                     "devices: " +
                     selector},
    };
  }

  return {
      .resolved = true,
      .path = candidates.begin()->second,
      .warnings = {},
  };
}

SerialDeviceResolveResult
resolve_serial_device(const std::string &selector,
                      const SerialDeviceResolverOptions &options) {
  if (string_is_blank(selector)) {
    return {
        .resolved = false,
        .path = {},
        .warnings = {"Warning: missing required configuration value [serial] "
                     "device; serial session will not connect"},
    };
  }

  const std::filesystem::path selector_path(selector);
  const std::filesystem::path by_path_root =
      options.dev_root / "serial" / "by-path";
  const std::string by_path_prefix =
      by_path_root.lexically_normal().string() + "/";
  const std::string normalized_selector =
      selector_path.lexically_normal().string();
  if (starts_with(normalized_selector, by_path_prefix)) {
    if (path_exists(selector_path)) {
      return {
          .resolved = true,
          .path = selector_path,
          .warnings = {},
      };
    }

    return {
        .resolved = false,
        .path = {},
        .warnings = {"Warning: serial device not found: " + selector},
    };
  }

  if (!contains_slash(selector)) {
    std::map<std::string, std::filesystem::path> candidates;
    add_by_id_candidates(&candidates, options.dev_root, selector);
    add_sysfs_identifier_candidates(&candidates, options.dev_root,
                                    options.sysfs_root, selector);
    return resolve_one_candidate(selector, candidates);
  }

  if (selector_path.is_absolute() &&
      is_tty_name(selector_path.filename().string()) &&
      is_character_device(selector_path)) {
    return {
        .resolved = true,
        .path = selector_path,
        .warnings = {},
    };
  }

  return {
      .resolved = false,
      .path = {},
      .warnings = {"Warning: serial device not found: " + selector},
  };
}

SerialDeviceResolveResult resolve_serial_device(const std::string &selector) {
  return resolve_serial_device(selector, SerialDeviceResolverOptions{});
}

} // namespace elder_terms
