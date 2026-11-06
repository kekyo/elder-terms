// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
static void write(const std::filesystem::path &path, const std::string &value) { std::ofstream(path) << value; }
static std::string read(const std::filesystem::path &path) {
  std::ifstream stream(path);
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

static void test_edits(const std::filesystem::path &directory) {
  const auto global = directory / "global.ini";
  const auto theme_dir = directory / "themes";
  std::filesystem::create_directory(theme_dir);
  const auto themes = theme_dir / "theme.ini";
  const std::string original = "# keep comment\n[general]\nbackground=#111111\n[unrelated]\nvalue=keep\n";
  write(global, original);
  auto session = begin_settings_edit(global, themes, 12);
  require(session.loaded, "editor files must load");
  auto catalog = session.catalog;
  catalog.themes.push_back({.id = "red", .name = "赤テーマ", .store = create_theme_settings(session.global)});
  set_explicit_setting_value(&catalog.themes[0].store, general_background_setting_key(), std::string("#990000"));
  auto result = save_settings_edit(&session, session.global, catalog);
  require(result.complete && result.global == SettingsFileSaveStatus::unchanged && result.themes == SettingsFileSaveStatus::saved,
          "theme-only save must update only theme.ini");
  require(read(global) == original, "theme-only save must leave global bytes unchanged");
  require(read(themes).find("font_size_dip") == std::string::npos, "inherited values must not expand into themes");
  auto draft = session.global;
  set_explicit_setting_value(&draft, application_ui_language_setting_key(), std::string("ja"));
  const auto saved_themes = read(themes);
  result = save_settings_edit(&session, draft, catalog);
  require(result.complete && result.themes == SettingsFileSaveStatus::unchanged, "ordinary save must not touch themes");
  require(read(themes) == saved_themes && read(global).find("value=keep") != std::string::npos,
          "language update must preserve theme bytes and unrelated global keys");
  require(save_new_window_connection("Test", global).saved, "New Window setting must save independently");
  require(read(themes) == saved_themes, "New Window save must not touch theme.ini");
  session = begin_settings_edit(global, themes, 12);
  draft = session.global;
  set_explicit_setting_value(&draft, general_background_setting_key(), std::string("#222222"));
  catalog.themes[0].name = "Renamed";
  std::filesystem::permissions(theme_dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
  result = save_settings_edit(&session, draft, catalog);
  std::filesystem::permissions(theme_dir, std::filesystem::perms::owner_all);
  require(!result.complete && result.global == SettingsFileSaveStatus::saved && result.themes == SettingsFileSaveStatus::failed,
          "partial save must report each file separately");
  require(read(themes) == saved_themes, "failed theme save must preserve old bytes");
  require(session.catalog.themes[0].name == "赤テーマ", "failed draft must not become the saved baseline");
  require(setting_string_value_or_default(session.global, general_background_setting_key(), "") == "#222222",
          "successful global save must advance its baseline");
  const auto restart = begin_settings_edit(global, themes, 12);
  require(restart.catalog.themes[0].name == "赤テーマ", "cancel/restart must keep only committed file values");
  result = save_settings_edit(&session, draft, catalog);
  require(result.complete && result.global == SettingsFileSaveStatus::unchanged && result.themes == SettingsFileSaveStatus::saved,
          "retry must write only the previously failed file");
  set_explicit_setting_value(&draft, general_background_setting_key(), std::string("#333333"));
  write(global, "[general]\nbackground=#abcdef\n");
  catalog.themes[0].name = "Saved despite global conflict";
  result = save_settings_edit(&session, draft, catalog);
  require(!result.complete && result.global == SettingsFileSaveStatus::failed && result.themes == SettingsFileSaveStatus::saved,
          "external conflicts must be detected per file");
  require(read(global).find("#abcdef") != std::string::npos, "external edit must survive a rejected save");
  require(setting_string_value_or_default(session.global, general_background_setting_key(), "") == "#222222",
          "unsaved global edits must never become theme fallbacks");
  catalog.themes.push_back(catalog.themes.front());
  require(!save_settings_edit(&session, draft, catalog).complete, "duplicate theme identities must fail before writing files");
  write(themes, "not an INI file");
  session = begin_settings_edit(global, themes, 12);
  require(!session.loaded && !save_settings_edit(&session, draft, {}).complete,
          "unreadable catalogs must never be replaced by an empty edit");
  require(read(themes) == "not an INI file", "failed reads must preserve the original file");
}

int main() {
  gchar *temporary = g_dir_make_tmp("elder-theme-edit-XXXXXX", nullptr);
  if (!temporary) return 1;
  const std::filesystem::path directory(temporary);
  g_free(temporary);
  try {
    test_edits(directory);
    std::filesystem::remove_all(directory);
    std::cout << "Settings edits passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::filesystem::permissions(directory / "themes", std::filesystem::perms::owner_all);
    std::filesystem::remove_all(directory);
    std::cerr << error.what() << "\n";
    return 1;
  }
}
