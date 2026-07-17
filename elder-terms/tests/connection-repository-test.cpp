#include "connection-repository.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <elder-terms/settings.h>
#include <elder-terms/settings/terminal-settings.h>

namespace elder_terms_connection_repository_test {

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static std::filesystem::path temporary_directory() {
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("elder-terms-repository-" + std::to_string(timestamp));
  std::filesystem::create_directories(path);
  return path;
}

static void write_file(const std::filesystem::path &path,
                       const std::string &content) {
  std::ofstream file(path);
  expect_true(file.good(), "test file should open");
  file << content;
  expect_true(file.good(), "test file should be written");
}

static void test_default_directory_uses_elder_terms_connections() {
  const std::filesystem::path path =
      elder_terms::default_connection_directory();
  expect_true(path.filename() == "connections",
              "default directory should end with connections");
  expect_true(path.parent_path().filename() == "elder-terms",
              "default directory should be scoped to elder-terms");
}

static void test_lists_only_regular_ini_profiles_in_name_order() {
  const std::filesystem::path directory = temporary_directory();
  write_file(directory / "Beta.ini", "");
  write_file(directory / "Alpha.ini", "");
  write_file(directory / "ignored.txt", "");
  std::filesystem::create_directory(directory / "Folder.ini");

  const auto profiles = elder_terms::list_connection_profiles(directory);
  std::filesystem::remove_all(directory);

  expect_true(profiles.size() == 2,
              "only regular INI files should be listed");
  expect_true(profiles[0].name == "Alpha" && profiles[1].name == "Beta",
              "profile names should be sorted ascending");
}

static void test_validates_and_normalizes_profile_names() {
  const std::filesystem::path directory = temporary_directory();
  write_file(directory / "Existing.ini", "");
  const auto profiles = elder_terms::list_connection_profiles(directory);

  const auto normalized = elder_terms::validate_connection_name(
      "  New connection  ", profiles, std::nullopt);
  expect_true(normalized.valid && normalized.name == "New connection",
              "name validation should trim surrounding whitespace");
  expect_true(!elder_terms::validate_connection_name("/", profiles,
                                                     std::nullopt)
                   .valid,
              "path separators should be rejected");
  expect_true(!elder_terms::validate_connection_name("..", profiles,
                                                     std::nullopt)
                   .valid,
              "parent path names should be rejected");
  expect_true(!elder_terms::validate_connection_name("Existing", profiles,
                                                     std::nullopt)
                   .valid,
              "duplicate names should be rejected");
  expect_true(elder_terms::validate_connection_name(
                  "Existing", profiles, directory / "Existing.ini")
                  .valid,
              "the current profile name should remain valid");
  std::filesystem::remove_all(directory);
}

static void test_saves_loads_and_renames_profiles() {
  const std::filesystem::path directory = temporary_directory();
  elder_terms::SettingsStore store = elder_terms::create_default_settings(
      elder_terms::default_terminal_display_settings(1.0));
  elder_terms::set_setting_value(
      &store, elder_terms::terminal_width_setting_key(),
      elder_terms::SettingValue{static_cast<gint64>(91)});

  const auto created = elder_terms::save_connection_profile(
      directory, std::nullopt, "First", store);
  expect_true(created.saved && created.path == directory / "First.ini",
              "new profile should be saved at its connection name");

  const elder_terms::SettingsLoadResult loaded =
      elder_terms::load_connection_profile(created.path);
  expect_true(loaded.loaded,
              "saved connection profile should load successfully");
  expect_true(elder_terms::terminal_display_settings(loaded.store).width == 91,
              "loaded connection profile should preserve settings");

  const auto renamed = elder_terms::save_connection_profile(
      directory, created.path, "Renamed", store);
  expect_true(renamed.saved && renamed.path == directory / "Renamed.ini",
              "renamed profile should use its new path");
  expect_true(!std::filesystem::exists(created.path) &&
                  std::filesystem::exists(renamed.path),
              "successful rename should remove the old path");
  std::filesystem::remove_all(directory);
}

static void test_rejects_save_name_collisions() {
  const std::filesystem::path directory = temporary_directory();
  write_file(directory / "Existing.ini", "[terminal]\nwidth=88\n");
  const elder_terms::SettingsStore store =
      elder_terms::create_default_settings(
          elder_terms::default_terminal_display_settings(1.0));

  const auto result = elder_terms::save_connection_profile(
      directory, std::nullopt, "Existing", store);
  expect_true(!result.saved,
              "saving a new profile over an existing name should fail");
  const elder_terms::SettingsLoadResult loaded =
      elder_terms::load_connection_profile(directory / "Existing.ini");
  expect_true(elder_terms::terminal_display_settings(loaded.store).width == 88,
              "collision failure should preserve the existing profile");
  std::filesystem::remove_all(directory);
}

} // namespace elder_terms_connection_repository_test

int main() {
  try {
    elder_terms_connection_repository_test::
        test_default_directory_uses_elder_terms_connections();
    elder_terms_connection_repository_test::
        test_lists_only_regular_ini_profiles_in_name_order();
    elder_terms_connection_repository_test::
        test_validates_and_normalizes_profile_names();
    elder_terms_connection_repository_test::
        test_saves_loads_and_renames_profiles();
    elder_terms_connection_repository_test::test_rejects_save_name_collisions();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
