#include "connection-repository.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include <glib.h>
#include <unistd.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr gdouble default_terminal_zoom = 1.0;

static std::string trim_ascii_whitespace(const std::string &value) {
  const auto first = std::find_if_not(
      value.begin(), value.end(),
      [](unsigned char character) { return g_ascii_isspace(character); });
  const auto last = std::find_if_not(
      value.rbegin(), value.rend(),
      [](unsigned char character) { return g_ascii_isspace(character); })
                        .base();
  return first < last ? std::string(first, last) : std::string();
}

static std::string file_error(const char *format,
                              const std::filesystem::path &path,
                              const std::error_code &error) {
  gchar *formatted = g_strdup_printf(
      format, path.string().c_str(), error.message().c_str());
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static std::string temporary_file_error(const char *format,
                                        const std::string &path,
                                        int error) {
  gchar *formatted =
      g_strdup_printf(format, path.c_str(), std::strerror(error));
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static std::optional<std::filesystem::path> create_temporary_file(
    const std::filesystem::path &directory, const char *kind,
    std::vector<std::string> *warnings) {
  std::string template_path =
      (directory / (std::string(".elder-terms-") + kind + "-XXXXXX.ini"))
          .string();
  const int descriptor = g_mkstemp(template_path.data());
  if (descriptor < 0) {
    const int failure = errno;
    warnings->push_back(temporary_file_error(
        _("Warning: failed to create temporary file %s: %s"), template_path,
        failure));
    return std::nullopt;
  }
  if (close(descriptor) != 0) {
    const int failure = errno;
    warnings->push_back(temporary_file_error(
        _("Warning: failed to close temporary file %s: %s"), template_path,
        failure));
    std::error_code remove_error;
    std::filesystem::remove(template_path, remove_error);
    return std::nullopt;
  }
  return std::filesystem::path(template_path);
}

static void remove_temporary_file(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

std::filesystem::path default_connection_directory() {
  return std::filesystem::path(g_get_user_config_dir()) / "elder-terms" /
         "connections";
}

std::vector<ConnectionProfile>
list_connection_profiles(const std::filesystem::path &directory) {
  std::vector<ConnectionProfile> profiles;
  std::error_code iterator_error;
  std::filesystem::directory_iterator iterator(directory, iterator_error);
  if (iterator_error) {
    return profiles;
  }

  for (const std::filesystem::directory_entry &entry : iterator) {
    std::error_code type_error;
    if (!entry.is_regular_file(type_error) || type_error ||
        entry.path().extension() != ".ini") {
      continue;
    }
    profiles.push_back({
        .name = entry.path().stem().string(),
        .path = entry.path(),
    });
  }
  std::sort(profiles.begin(), profiles.end(),
            [](const ConnectionProfile &left,
               const ConnectionProfile &right) {
              return left.name < right.name;
            });
  return profiles;
}

ConnectionNameValidationResult validate_connection_name(
    const std::string &candidate,
    const std::vector<ConnectionProfile> &profiles,
    const std::optional<std::filesystem::path> &current_path) {
  ConnectionNameValidationResult result{
      .valid = false,
      .name = trim_ascii_whitespace(candidate),
      .error = {},
  };
  if (result.name.empty()) {
    result.error = _("Connection name must not be empty");
    return result;
  }
  if (result.name == "." || result.name == "..") {
    result.error = _("Connection name must not be a relative path");
    return result;
  }
  if (result.name.find('/') != std::string::npos ||
      result.name.find('\0') != std::string::npos) {
    result.error = _("Connection name must not contain a path separator");
    return result;
  }

  const auto duplicate = std::find_if(
      profiles.begin(), profiles.end(),
      [&result, &current_path](const ConnectionProfile &profile) {
        return profile.name == result.name &&
               (!current_path.has_value() ||
                profile.path != current_path.value());
      });
  if (duplicate != profiles.end()) {
    result.error = _("A connection with this name already exists");
    return result;
  }
  result.valid = true;
  return result;
}

SettingsLoadResult load_connection_profile(const std::filesystem::path &path) {
  return load_settings(
      SettingsLoadOptions{
          .config_path = path,
          .startup_config_path = std::nullopt,
          .global_config_path = default_global_config_path(),
      },
      default_terminal_zoom);
}

ConnectionSaveResult save_connection_profile(
    const std::filesystem::path &directory,
    const std::optional<std::filesystem::path> &original_path,
    const std::string &name, const SettingsStore &store) {
  ConnectionSaveResult result;
  std::error_code directory_error;
  std::filesystem::create_directories(directory, directory_error);
  if (directory_error) {
    result.warnings.push_back(
        file_error(_("Warning: failed to create connection directory %s: %s"),
                   directory, directory_error));
    return result;
  }

  const auto profiles = list_connection_profiles(directory);
  const ConnectionNameValidationResult validation =
      validate_connection_name(name, profiles, original_path);
  if (!validation.valid) {
    gchar *formatted =
        g_strdup_printf(_("Warning: %s"), validation.error.c_str());
    result.warnings.emplace_back(formatted == nullptr ? "" : formatted);
    g_free(formatted);
    return result;
  }

  const std::filesystem::path target =
      directory / (validation.name + ".ini");
  const auto temporary =
      create_temporary_file(directory, "save", &result.warnings);
  if (!temporary.has_value()) {
    return result;
  }
  const SettingsSaveResult save_result = save_settings(store, *temporary);
  result.warnings.insert(result.warnings.end(), save_result.warnings.begin(),
                         save_result.warnings.end());
  if (!save_result.saved) {
    remove_temporary_file(*temporary);
    return result;
  }

  if (!original_path.has_value() || original_path.value() == target) {
    std::error_code replace_error;
    std::filesystem::rename(*temporary, target, replace_error);
    if (replace_error) {
      result.warnings.push_back(
          file_error(_("Warning: failed to replace connection profile %s: %s"),
                     target, replace_error));
      remove_temporary_file(*temporary);
      return result;
    }
    result.saved = true;
    result.path = target;
    return result;
  }

  const auto backup =
      create_temporary_file(directory, "backup", &result.warnings);
  if (!backup.has_value()) {
    remove_temporary_file(*temporary);
    return result;
  }
  std::error_code backup_error;
  std::filesystem::rename(original_path.value(), *backup, backup_error);
  if (backup_error) {
    result.warnings.push_back(file_error(
        _("Warning: failed to prepare connection rename %s: %s"),
        original_path.value(), backup_error));
    remove_temporary_file(*temporary);
    remove_temporary_file(*backup);
    return result;
  }

  std::error_code replace_error;
  std::filesystem::rename(*temporary, target, replace_error);
  if (replace_error) {
    result.warnings.push_back(
        file_error(_("Warning: failed to rename connection profile %s: %s"),
                   target, replace_error));
    std::error_code rollback_error;
    std::filesystem::rename(*backup, original_path.value(), rollback_error);
    if (rollback_error) {
      result.warnings.push_back(file_error(
          _("Warning: failed to restore connection profile %s: %s"),
          original_path.value(), rollback_error));
    }
    remove_temporary_file(*temporary);
    remove_temporary_file(*backup);
    return result;
  }

  remove_temporary_file(*backup);
  result.saved = true;
  result.path = target;
  return result;
}

ConnectionRenameResult rename_connection_profile(
    const std::filesystem::path &directory,
    const std::filesystem::path &original_path, const std::string &name) {
  ConnectionRenameResult result;
  const auto profiles = list_connection_profiles(directory);
  const ConnectionNameValidationResult validation =
      validate_connection_name(name, profiles, original_path);
  if (!validation.valid) {
    gchar *formatted =
        g_strdup_printf(_("Warning: %s"), validation.error.c_str());
    result.warnings.emplace_back(formatted == nullptr ? "" : formatted);
    g_free(formatted);
    return result;
  }

  const std::filesystem::path target =
      directory / (validation.name + ".ini");
  if (original_path == target) {
    std::error_code type_error;
    if (std::filesystem::is_regular_file(original_path, type_error) &&
        !type_error) {
      result.renamed = true;
      result.path = target;
      return result;
    }
  }

  std::error_code rename_error;
  std::filesystem::rename(original_path, target, rename_error);
  if (rename_error) {
    result.warnings.push_back(
        file_error(_("Warning: failed to rename connection profile %s: %s"),
                   target, rename_error));
    return result;
  }
  result.renamed = true;
  result.path = target;
  return result;
}

ConnectionDeleteResult
delete_connection_profile(const std::filesystem::path &path) {
  ConnectionDeleteResult result;
  std::error_code delete_error;
  const bool deleted = std::filesystem::remove(path, delete_error);
  if (!deleted || delete_error) {
    if (!delete_error) {
      delete_error = std::make_error_code(
          std::errc::no_such_file_or_directory);
    }
    result.warnings.push_back(
        file_error(_("Warning: failed to delete connection profile %s: %s"),
                   path, delete_error));
    return result;
  }
  result.deleted = true;
  return result;
}

} // namespace elder_terms
