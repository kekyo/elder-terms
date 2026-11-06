// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace elder_terms;

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static void write(const std::filesystem::path &path, const std::string &text) {
  std::ofstream stream(path);
  stream << text;
  require(stream.good(), "fixture write failed");
}

static void test_themes(const std::filesystem::path &directory) {
  const auto global = directory / "global.ini";
  const auto theme = directory / "theme.ini";
  const auto connection = directory / "connection.ini";
  const auto startup = directory / "startup.ini";
  const auto background = general_background_setting_key();
  const auto exterior = general_exterior_background_setting_key();
  const auto reference = make_setting_key("general", "theme");
  write(global, "[general]\nbackground=#111111\nexterior_background=#444444\n");
  write(theme, "[theme.red]\nname=赤テーマ\n[theme.red.general]\n"
               "exterior_background=#7A2222\nbackground=#281818\n"
               "auto_close=false\n[theme.green]\nname=Green\n"
               "[theme.green.general]\nbackground=#008800\n");
  write(connection, "[general]\ntheme=red\n");
  const auto theme_editor = create_theme_settings(load_global_settings(global, 12.0).store);
  for (const auto &entry : theme_editor.entries) {
    require(entry.definition.key.name != "ui_language" && entry.definition.key.name != "open_application" &&
            entry.definition.key.name != "new_window" && entry.definition.key.name != "startup_mode",
            "application-only settings must never be theme settings");
  }
  SettingsLoadOptions options{.config_path = connection, .startup_config_path = std::nullopt,
                              .global_config_path = global};
  auto loaded = load_settings(options, 12.0);
  require(loaded.loaded, "theme connection must load");
  require(setting_string_value_or_default(loaded.store, background, "") == "#281818",
          "selected theme must override the global background");
  require(setting_string_value_or_default(loaded.store, exterior, "") == "#7A2222",
          "selected theme must override the titlebar background");
  require(!general_auto_close(loaded.store), "explicit false must survive theme inheritance");
  require(!setting_has_explicit_value(loaded.store, background), "theme must remain inherited");
  require(setting_value_source(loaded.store, background) == SettingValueSource::theme,
          "theme origin must be distinct from global defaults");
  for (const auto &entry : loaded.store.entries) {
    if (entry.definition.key.section == "general" && entry.definition.key.name == "background")
      require(entry.fallback_theme_id == "red" && entry.fallback_theme_name == "赤テーマ",
              "inherited values must identify the theme");
  }
  require(terminal_display_settings(loaded.store).font_size_dip == 12.0,
          "omitted theme values must keep built-in defaults");

  set_explicit_setting_value(&loaded.store, background, std::string("#281818"));
  require(save_settings(loaded.store, connection).saved, "connection must save");
  write(theme, "[theme.red]\nname=Renamed red\n[theme.red.general]\nbackground=#990000\n");
  loaded = load_settings(options, 12.0);
  require(setting_string_value_or_default(loaded.store, background, "") == "#281818",
          "same-value explicit override must survive a theme edit");
  clear_explicit_setting_value(&loaded.store, background);
  require(setting_string_value_or_default(loaded.store, background, "") == "#990000",
          "clearing an override must restore the selected theme");
  require(save_settings(loaded.store, connection).saved, "inherited connection must save");
  loaded = load_settings(options, 12.0);
  require(!setting_has_explicit_value(loaded.store, background), "save must not expand inheritance");
  require(setting_string_value_or_default(loaded.store, reference, "") == "red",
          "rename must preserve the reference ID");

  write(startup, "[general]\ntheme=\n");
  options.startup_config_path = startup;
  loaded = load_settings(options, 12.0);
  require(setting_string_value_or_default(loaded.store, background, "") == "#111111",
          "explicit empty startup reference must disable the theme");
  write(startup, "[general]\nauto_close=false\n");
  loaded = load_settings(options, 12.0);
  require(setting_string_value_or_default(loaded.store, background, "") == "#990000",
          "absent startup reference must preserve the connection selection");
  write(theme, "[theme.green]\nname=Green\n[theme.green.general]\nbackground=#008800\n");
  write(startup, "[general]\ntheme=green\n");
  loaded = load_settings(options, 12.0);
  require(loaded.loaded && setting_string_value_or_default(loaded.store, background, "") == "#008800",
          "startup selection must replace even an unresolved connection selection");
  write(connection, "[general]\ntheme=red\nbackground=none\n");
  loaded = load_settings(options, 12.0);
  require(setting_string_value_or_default(loaded.store, background, "") == "none",
          "connection explicit none must override the startup-selected theme");

  options.startup_config_path.reset();
  std::filesystem::remove(theme);
  loaded = load_settings(options, 12.0);
  require(!loaded.loaded && !loaded.warnings.empty(), "missing selected theme must be diagnosed");
  require(setting_string_value_or_default(loaded.store, reference, "") == "red",
          "unresolved reference must be retained");
  write(connection, "[general]\n");
  loaded = load_settings(options, 12.0);
  require(loaded.loaded && setting_string_value_or_default(loaded.store, background, "") == "#111111",
          "missing theme file without a selection must preserve existing behavior");
  std::filesystem::create_directory(theme);
  loaded = load_settings(options, 12.0);
  require(!loaded.warnings.empty(), "theme read failure must not silently become an empty catalog");
  std::filesystem::remove(theme);
  const auto alternate = directory / "separate-themes.ini";
  write(alternate, "[theme.red]\nname=Red\n[theme.red.general]\nbackground=#990000\n"
                   "[theme.red.nfs]\nuid=0\n[theme.red.ssh]\nusername=\n");
  write(connection, "[general]\ntheme=red\n");
  options.theme_config_path = alternate;
  loaded = load_settings(options, 12.0);
  require(loaded.loaded && setting_string_value_or_default(loaded.store, background, "") == "#990000",
          "explicit theme path must be independent of the connection directory");
  require(setting_integer_value_or_default(loaded.store, make_setting_key("nfs", "uid"), -1) == 0,
          "zero is an explicit theme value");
  require(setting_value_source(loaded.store, ssh_username_setting_key()) == SettingValueSource::theme,
          "empty strings are explicit theme values");
  write(alternate, "[theme.red]\nname=Red\n[theme.red.nfs]\nuid=broken\n");
  loaded = load_settings(options, 12.0);
  for (const auto &entry : loaded.store.entries) {
    if (entry.definition.key.section == "nfs" && entry.definition.key.name == "uid") {
      require(!entry.validation_error.empty() && entry.invalid_raw_value == "broken",
              "invalid inherited input must retain both its error and original representation");
      require(entry.fallback_source == SettingValueSource::theme, "invalid input must retain its origin");
    }
  }
}

int main(int argc, char **argv) {
  try {
    if (argc == 4) {
      const auto result = load_settings({.config_path = argv[3], .startup_config_path = std::nullopt,
                                        .global_config_path = argv[1], .theme_config_path = argv[2]}, 12.0);
      for (const auto &entry : result.store.entries) {
        if (entry.definition.key.section != "general") continue;
        std::cout << entry.definition.key.name << "=";
        std::visit([](const auto &value) {
          if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::vector<std::string>>) {
            for (const auto &text : value) std::cout << text << ";";
          } else std::cout << value;
        }, entry.value);
        const auto source = setting_value_source(result.store, entry.definition.key);
        std::cout << " source=" << (source == SettingValueSource::override ? "connection" :
            source == SettingValueSource::theme ? "theme" :
            source == SettingValueSource::global ? "global" : "built-in");
        if (source == SettingValueSource::theme) std::cout << " (" << entry.fallback_theme_name << ")";
        std::cout << "\n";
      }
      for (const auto &warning : result.warnings) std::cerr << warning << "\n";
      return result.loaded ? 0 : 1;
    }
    gchar *temporary = g_dir_make_tmp("elder-themes-XXXXXX", nullptr);
    require(temporary != nullptr, "temporary directory must exist");
    const std::filesystem::path directory(temporary);
    g_free(temporary);
    try { test_themes(directory); }
    catch (...) { std::filesystem::remove_all(directory); throw; }
    std::filesystem::remove_all(directory);
    std::cout << "Named settings themes passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
