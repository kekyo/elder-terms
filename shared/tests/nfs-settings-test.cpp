// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static GtkWidget *find_widget(GtkWidget *root, const std::string &name) {
  if (name == gtk_widget_get_name(root)) return root;
  if (!GTK_IS_CONTAINER(root)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(root));
  GtkWidget *found = nullptr;
  for (auto *child = children; child && !found; child = child->next)
    found = find_widget(GTK_WIDGET(child->data), name);
  g_list_free(children);
  return found;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  using namespace elder_terms;
  try {
    auto store = create_default_settings({}, "NFS settings");
    expect(set_explicit_setting_value(&store, general_type_setting_key(), std::string("nfs")),
        "NFS must be a persisted connection type");
    expect(!terminal_connection_profile(store), "NFS must launch the transfer application");
    expect(!nfs_connection_settings(store).validation_errors.empty(), "NFS requires a server address");
    expect(set_explicit_setting_value(&store, nfs_setting_key("address"), std::string("localhost")), "Accept a hostname");
    expect(nfs_connection_settings(store).filename_encoding == "UTF-8", "The default encoding must be explicit UTF-8");
    expect(nfs_connection_settings(store).validation_errors.empty(), "Default NFS v3 settings must be valid");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("filename_encoding"), std::string("CP932//TRANSLIT")),
        "Lossy conversion modifiers must be rejected");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("filename_encoding"), std::string("unknown-encoding")),
        "Unavailable encodings must be rejected");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("export_path"), std::string("/srv/../public")),
        "Export paths must not contain parent segments");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("nfs_port"), gint64{65536}), "Reject invalid ports");
    expect(set_explicit_setting_value(&store, nfs_setting_key("nfs_port"), gint64{0}), "Zero must select service discovery");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("uid"), gint64{4294967296}), "Reject overflowing AUTH_SYS IDs");
    expect(set_explicit_setting_value(&store, nfs_setting_key("uid"), gint64{4294967295}), "Accept unsigned 32-bit AUTH_SYS IDs");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("supplementary_gids"), std::vector<std::string>(17, "1")),
        "AUTH_SYS permits at most 16 supplementary groups");
    expect(set_explicit_setting_value(&store, nfs_setting_key("supplementary_gids"), std::vector<std::string>{"1", "4294967295"}),
        "Accept supplementary groups without signed truncation");
    expect(nfs_connection_settings(store).supplementary_gids == std::vector<std::uint32_t>{1, 4294967295U},
        "Resolve exact group IDs");
    expect(set_explicit_setting_value(&store, nfs_setting_key("filename_encoding"), std::string("CP932")), "Accept CP932");
    auto *temporary = g_dir_make_tmp("elder-nfs-settings-XXXXXX", nullptr);
    const std::filesystem::path directory(temporary);
    g_free(temporary);
    struct Cleanup { std::filesystem::path directory; ~Cleanup() { std::filesystem::remove_all(directory); } } cleanup{directory};
    const auto global_path = directory / "global.ini", connection_path = directory / "nfs.ini";
    expect(save_global_settings(store, global_path).saved, "Save NFS global defaults");
    { std::ofstream file(connection_path); file << "[general]\ntype=nfs\n[nfs]\naddress=localhost\n"; }
    const SettingsLoadOptions load_options{.config_path = connection_path, .startup_config_path = std::nullopt,
        .global_config_path = global_path};
    auto inherited = load_settings(load_options, 1.0).store;
    expect(nfs_connection_settings(inherited).filename_encoding == "CP932" &&
        setting_value_source(inherited, nfs_setting_key("filename_encoding")) == SettingValueSource::global,
        "Unset connection encoding must inherit the global default");
    expect(set_explicit_setting_value(&inherited, nfs_setting_key("filename_encoding"), std::string("EUC-JP")), "Override encoding");
    expect(set_explicit_setting_value(&inherited, nfs_setting_key("known_exports"), std::vector<std::string>{"/srv/public", "/other/public"}),
        "Known paths must preserve distinct full export names");
    expect(save_settings(inherited, connection_path).saved, "Save the connection override");
    auto loaded = load_settings(load_options, 1.0).store;
    expect(nfs_connection_settings(loaded).filename_encoding == "EUC-JP" &&
        nfs_connection_settings(loaded).known_exports == std::vector<std::string>{"/srv/public", "/other/public"},
        "Connection settings must survive save and reload");
    clear_explicit_setting_value(&loaded, nfs_setting_key("filename_encoding"));
    expect(nfs_connection_settings(loaded).filename_encoding == "CP932", "Clearing an override restores the global value");
    { std::ofstream file(connection_path); file << "[general]\ntype=nfs\n[nfs]\naddress=localhost\nfilename_encoding=UTF-8//IGNORE\n"; }
    auto invalid = load_settings(load_options, 1.0);
    expect(!invalid.warnings.empty() && !nfs_connection_settings(invalid.store).validation_errors.empty(),
        "Invalid persisted encodings must stop connection instead of falling back");
    expect(save_settings(invalid.store, connection_path).saved &&
        !nfs_connection_settings(load_settings(load_options, 1.0).store).validation_errors.empty(),
        "Saving must retain invalid settings for correction");
    SettingsWidgetOptions widget_options;
    widget_options.store = inherited;
    auto *widget = create_settings_widget(std::move(widget_options));
    auto *root = settings_widget_root(widget);
    auto *page = find_widget(root, "settings_nfs_page");
    expect(page && gtk_widget_get_visible(page), "NFS connections must have an editable settings page");
    auto *encoding = find_widget(root, "settings_nfs_filename_encoding_entry");
    auto *inherit = find_widget(root, "settings_nfs_filename_encoding_inherit");
    expect(encoding && inherit, "Encoding must support an explicit override and inheritance");
    expect(std::string(gtk_entry_get_text(GTK_ENTRY(encoding))) == "EUC-JP", "Show the effective override");
    gtk_entry_set_text(GTK_ENTRY(encoding), "CP932//IGNORE");
    expect(!settings_widget_is_valid(widget), "An invalid encoding must prevent Save");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inherit), TRUE);
    expect(settings_widget_is_valid(widget) && !gtk_widget_get_sensitive(encoding) &&
        std::string(gtk_entry_get_text(GTK_ENTRY(encoding))) == "CP932", "Inheritance must restore the global encoding");
    expect(!gtk_widget_get_visible(find_widget(root, "settings_ssh_proxy_page")), "NFS must not advertise an unsupported SSH proxy");
    destroy_settings_widget(widget);
    SettingsWidgetOptions runtime_options;
    runtime_options.store = inherited;
    runtime_options.is_runtime = true;
    auto *runtime = create_settings_widget(std::move(runtime_options));
    auto *runtime_root = settings_widget_root(runtime);
    expect(gtk_widget_get_sensitive(find_widget(runtime_root, "settings_nfs_filename_encoding_entry")),
        "Runtime encoding edits must be available for the next connection without changing the active client's encoding");
    expect(!gtk_widget_get_sensitive(find_widget(runtime_root, "settings_nfs_address_entry")),
        "Runtime encoding edits must not unlock the active endpoint");
    destroy_settings_widget(runtime);
    std::cout << "PASS NFS settings and inheritance\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
