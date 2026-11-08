// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include <elder-terms/settings-widget.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <filesystem>
#include <fstream>

static void expect(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static GtkWidget *find_widget(GtkWidget *root, std::string_view name) {
  if (name == gtk_widget_get_name(root)) return root;
  if (!GTK_IS_CONTAINER(root)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(root));
  GtkWidget *found = nullptr;
  for (auto *child = children; child && !found; child = child->next) found = find_widget(GTK_WIDGET(child->data), name);
  g_list_free(children); return found;
}
int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  try {
    using namespace elder_terms;
    auto store = create_default_settings({}, "HTTP test");
    expect(set_explicit_setting_value(&store, general_type_setting_key(), std::string("http")), "HTTP must be a persisted connection type");
    expect(!terminal_connection_profile(store), "HTTP must launch the file-transfer application");
    expect(!http_connection_settings(store).validation_errors.empty(), "An HTTP connection requires a base URL");
    const auto base_key = make_setting_key("http", "base_url");
    expect(set_explicit_setting_value(&store, base_key, std::string("https://example.com/pub/")), "A directory base URL must be accepted");
    expect(!set_explicit_setting_value(&store, base_key, std::string("https://example.com/pub/index.html")), "File-like base URLs must be rejected");
    expect(!set_explicit_setting_value(&store, base_key, std::string("https://user:secret@example.com/pub/")), "URL credentials must be rejected");
    expect(!set_explicit_setting_value(&store, http_setting_key("username"), std::string("user\tname")), "Control characters must be rejected");
    auto *temporary = g_dir_make_tmp("elder-http-settings-XXXXXX", nullptr);
    const std::filesystem::path directory(temporary);
    g_free(temporary);
    const auto config = directory / "http.ini";
    expect(save_settings(store, config).saved, "HTTP settings must be saved");
    const auto loaded = load_settings({.config_path = config, .startup_config_path = std::nullopt}, 1.0);
    expect(http_connection_settings(loaded.store).base_url == "https://example.com/pub/", "Saved HTTP base must be restored");
    auto inherited = create_default_settings({}, "Inherited HTTP");
    rebase_settings_store_fallbacks(&inherited, loaded.store);
    expect(http_connection_settings(inherited).start_url == "https://example.com/pub/", "Empty start page must inherit the base URL");
    expect(set_explicit_setting_value(&inherited, http_setting_key("start_url"), std::string("index.html")), "Relative start page must be accepted");
    expect(http_connection_settings(inherited).start_url == "https://example.com/pub/index.html", "Start page override must resolve within inherited base");
    clear_explicit_setting_value(&inherited, http_setting_key("start_url"));
    expect(http_connection_settings(inherited).start_url == "https://example.com/pub/", "Clearing start page must restore inheritance");
    { std::ofstream stream(config); stream << "[general]\ntype=http\n[http]\nbase_url=https://example.com/pub/\nidle_timeout_seconds=invalid\n"; }
    const auto invalid = load_settings({.config_path = config, .startup_config_path = std::nullopt}, 1.0);
    expect(!http_connection_settings(invalid.store).validation_errors.empty(), "Invalid persisted timeout must be rejected");
    expect(save_settings(invalid.store, config).saved, "Invalid input must remain serializable");
    expect(!http_connection_settings(load_settings({.config_path = config, .startup_config_path = std::nullopt}, 1.0).store).validation_errors.empty(), "Saving must preserve invalid input");
    std::filesystem::remove_all(directory);
    SettingsWidgetOptions options; options.store = store;
    auto *state = create_settings_widget(std::move(options));
    auto *root = settings_widget_root(state);
    auto *page = find_widget(root, "settings_http_page");
    expect(page && gtk_widget_get_visible(page), "HTTP settings page must be visible for HTTP connections");
    auto *base = find_widget(root, "settings_http_base_url_entry");
    auto *start = find_widget(root, "settings_http_start_url_entry");
    expect(base && start, "Base URL and start page must be independently editable");
    gtk_entry_set_text(GTK_ENTRY(base), "https://example.com/pub/");
    gtk_entry_set_text(GTK_ENTRY(start), "https://example.com/outside/");
    expect(!settings_widget_is_valid(state), "Out-of-scope start page must prevent Save");
    gtk_entry_set_text(GTK_ENTRY(start), "https://example.com/pub/index.html");
    expect(settings_widget_is_valid(state), "A valid scoped start page must allow Save");
    expect(gtk_widget_get_visible(find_widget(root, "settings_ssh_proxy_page")), "HTTP must expose SSH proxy settings");
    destroy_settings_widget(state);
    std::cout << "HTTP settings and validation: PASS\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
