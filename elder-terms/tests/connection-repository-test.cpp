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

static void test_creates_initial_local_terminal_only_before_directory_exists() {
  const std::filesystem::path root = temporary_directory();
  const std::filesystem::path directory = root / "connections";

  const elder_terms::InitialConnectionProfileResult created =
      elder_terms::create_initial_local_terminal_profile(directory);
  expect_true(created.created &&
                  created.path == directory / "Local terminal.ini",
              "first launch should create the initial local terminal");
  expect_true(created.warnings.empty(),
              "initial profile creation should not report warnings");

  const auto profiles = elder_terms::list_connection_profiles(directory);
  expect_true(profiles.size() == 1 &&
                  profiles.front().name == "Local terminal",
              "the initial profile should be listed as Local terminal");
  const elder_terms::SettingsLoadResult loaded =
      elder_terms::load_connection_profile(created.path);
  const auto terminal = elder_terms::terminal_connection_profile(loaded.store);
  expect_true(loaded.loaded && terminal.has_value() &&
                  terminal->name == "Local terminal" &&
                  terminal->kind ==
                      elder_terms::TerminalConnectionKind::local_shell,
              "the initial profile should load as a default local terminal");

  std::filesystem::remove(created.path);
  const elder_terms::InitialConnectionProfileResult repeated =
      elder_terms::create_initial_local_terminal_profile(directory);
  expect_true(!repeated.created && repeated.warnings.empty() &&
                  elder_terms::list_connection_profiles(directory).empty(),
              "an empty existing directory should not recreate the profile");
  std::filesystem::remove_all(root);
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
      elder_terms::default_terminal_display_settings(1.0), "elder-terms");
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
          elder_terms::default_terminal_display_settings(1.0), "elder-terms");

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

static void test_renames_and_deletes_profiles_without_rewriting_contents() {
  const std::filesystem::path directory = temporary_directory();
  const std::filesystem::path original = directory / "Original.ini";
  const std::string content =
      "# Preserve this formatting\n[terminal]\nwidth = 88\n";
  write_file(original, content);

  const auto renamed = elder_terms::rename_connection_profile(
      directory, original, "  Renamed  ");
  expect_true(renamed.renamed && renamed.path == directory / "Renamed.ini",
              "renaming should normalize and move the selected profile");
  std::ifstream renamed_file(renamed.path);
  const std::string renamed_content{
      std::istreambuf_iterator<char>(renamed_file),
      std::istreambuf_iterator<char>()};
  expect_true(renamed_content == content,
              "renaming should preserve profile contents byte for byte");

  const auto deleted = elder_terms::delete_connection_profile(renamed.path);
  expect_true(deleted.deleted && !std::filesystem::exists(renamed.path),
              "deleting should remove the selected profile");
  std::filesystem::remove_all(directory);
}

static void test_rejects_rename_collisions_and_missing_deletes() {
  const std::filesystem::path directory = temporary_directory();
  const std::filesystem::path original = directory / "Original.ini";
  write_file(original, "original");
  write_file(directory / "Existing.ini", "existing");

  const auto renamed = elder_terms::rename_connection_profile(
      directory, original, "Existing");
  expect_true(!renamed.renamed && std::filesystem::exists(original),
              "rename collisions should preserve the original profile");
  const auto deleted = elder_terms::delete_connection_profile(
      directory / "Missing.ini");
  expect_true(!deleted.deleted,
              "deleting a missing profile should report failure");
  std::filesystem::remove_all(directory);
}

} // namespace elder_terms_connection_repository_test

int main() {
  try {
    elder_terms_connection_repository_test::
        test_default_directory_uses_elder_terms_connections();
    elder_terms_connection_repository_test::
        test_creates_initial_local_terminal_only_before_directory_exists();
    elder_terms_connection_repository_test::
        test_lists_only_regular_ini_profiles_in_name_order();
    elder_terms_connection_repository_test::
        test_validates_and_normalizes_profile_names();
    elder_terms_connection_repository_test::
        test_saves_loads_and_renames_profiles();
    elder_terms_connection_repository_test::test_rejects_save_name_collisions();
    elder_terms_connection_repository_test::
        test_renames_and_deletes_profiles_without_rewriting_contents();
    elder_terms_connection_repository_test::
        test_rejects_rename_collisions_and_missing_deletes();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
