#include <elder-terms/settings/sftp-settings.h>

namespace elder_terms {

static constexpr char sftp_section[] = "sftp";
static constexpr char sftp_local_directory_key[] = "local_directory";
static constexpr char sftp_remote_directory_key[] = "remote_directory";
static constexpr char default_sftp_remote_directory[] = ".";

static SettingKey sftp_key(const char *name) {
  return make_setting_key(sftp_section, name);
}

SettingKey sftp_local_directory_setting_key() {
  return sftp_key(sftp_local_directory_key);
}

SettingKey sftp_remote_directory_setting_key() {
  return sftp_key(sftp_remote_directory_key);
}

std::vector<SettingDefinition> sftp_connection_setting_definitions() {
  return {
      {
          .key = sftp_local_directory_setting_key(),
          .default_value = SettingValue{std::string()},
          .validate = nullptr,
      },
      {
          .key = sftp_remote_directory_setting_key(),
          .default_value =
              SettingValue{std::string(default_sftp_remote_directory)},
          .validate = nullptr,
      },
  };
}

SftpConnectionSettings sftp_connection_settings(const SettingsStore &store) {
  return {
      .endpoint = ssh_endpoint_settings(store),
      .local_directory = setting_string_value_or_default(
          store, sftp_local_directory_setting_key(), std::string()),
      .remote_directory = setting_string_value_or_default(
          store, sftp_remote_directory_setting_key(),
          default_sftp_remote_directory),
  };
}

} // namespace elder_terms
