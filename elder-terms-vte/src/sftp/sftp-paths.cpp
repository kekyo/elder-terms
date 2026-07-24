#include "sftp-paths.h"

#include <filesystem>
#include <optional>
#include <string>

#include <gio/gio.h>

namespace elder_terms {

static std::string default_downloads_directory() {
  const char *downloads =
      g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
  if (downloads != nullptr && downloads[0] != '\0') {
    return downloads;
  }
  const char *home = g_get_home_dir();
  if (home == nullptr || home[0] == '\0') {
    return std::filesystem::current_path().string();
  }
  return (std::filesystem::path(home) / "Downloads").string();
}

static std::optional<std::string> native_path(
    const std::string &path_or_uri) {
  if (path_or_uri.empty()) {
    return std::nullopt;
  }
  gchar *scheme = g_uri_parse_scheme(path_or_uri.c_str());
  GFile *file = scheme == nullptr
                    ? g_file_new_for_path(path_or_uri.c_str())
                    : g_file_new_for_uri(path_or_uri.c_str());
  g_free(scheme);
  gchar *path = g_file_get_path(file);
  g_object_unref(file);
  if (path == nullptr) {
    return std::nullopt;
  }
  std::string result(path);
  g_free(path);
  return result;
}

std::string resolve_sftp_local_directory(
    const SettingsStore &store,
    const SftpConnectionSettings &settings) {
  if (const auto configured =
          native_path(settings.local_directory);
      configured.has_value()) {
    return configured.value();
  }
  if (const auto transfer =
          native_path(transfer_base_path(store));
      transfer.has_value()) {
    return transfer.value();
  }
  return default_downloads_directory();
}

} // namespace elder_terms
