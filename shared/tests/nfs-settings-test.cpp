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
    expect(!nfs_connection_settings(store).require_reserved_port, "Reserved source ports are optional by default");
    expect(set_explicit_setting_value(&store, nfs_setting_key("require_reserved_port"), true),
        "Requiring a reserved source port must be a persisted NFS setting");
    expect(nfs_connection_settings(store).require_reserved_port, "Resolve the required source-port policy");
    expect(set_explicit_setting_value(&store, nfs_setting_key("version"), std::string("4.0")),
        "NFS v4.0 must be selectable without ambiguous minor-version negotiation");
    expect(!set_explicit_setting_value(&store, nfs_setting_key("version"), std::string("4.2")),
        "An unimplemented minor version must not silently select v4.0");
    expect(set_explicit_setting_value(&store, nfs_setting_key("version"), std::string("3")), "Restore v3");
    expect(set_explicit_setting_value(&store, nfs_setting_key("version"), std::string("2")), "NFS v2 must be explicitly selectable");
    expect(set_explicit_setting_value(&store, nfs_setting_key("version"), std::string("3")), "Restore default protocol");
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
    expect(nfs_connection_settings(inherited).require_reserved_port &&
        setting_value_source(inherited, nfs_setting_key("require_reserved_port")) == SettingValueSource::global,
        "The source-port policy must inherit the global setting");
    expect(set_explicit_setting_value(&inherited, nfs_setting_key("require_reserved_port"), false),
        "A connection can override the inherited source-port requirement");
    expect(nfs_connection_settings(inherited).filename_encoding == "CP932" &&
        setting_value_source(inherited, nfs_setting_key("filename_encoding")) == SettingValueSource::global,
        "Unset connection encoding must inherit the global default");
    expect(set_explicit_setting_value(&inherited, nfs_setting_key("filename_encoding"), std::string("EUC-JP")), "Override encoding");
    expect(set_explicit_setting_value(&inherited, nfs_setting_key("known_exports"), std::vector<std::string>{"/srv/public", "/other/public"}),
        "Known paths must preserve distinct full export names");
    expect(save_settings(inherited, connection_path).saved, "Save the connection override");
    auto loaded = load_settings(load_options, 1.0).store;
    expect(!nfs_connection_settings(loaded).require_reserved_port,
        "An explicit false source-port policy must survive save and reload");
    clear_explicit_setting_value(&loaded, nfs_setting_key("require_reserved_port"));
    expect(nfs_connection_settings(loaded).require_reserved_port,
        "Clearing the source-port override restores the global requirement");
    expect(nfs_connection_settings(loaded).filename_encoding == "EUC-JP" &&
        nfs_connection_settings(loaded).known_exports == std::vector<std::string>{"/srv/public", "/other/public"},
        "Connection settings must survive save and reload");
    expect(set_explicit_setting_value(&loaded, nfs_setting_key("namespace_path"), std::string("/public")) &&
        set_explicit_setting_value(&loaded, nfs_setting_key("known_namespace_paths"), std::vector<std::string>{"/public/deep"}) &&
        set_explicit_setting_value(&loaded, nfs_setting_key("version"), std::string("4.0")),
        "Namespace paths must be configured independently of MOUNT exports");
    expect(save_settings(loaded, connection_path).saved, "Save v4 namespace settings");
    const auto v4 = nfs_connection_settings(load_settings(load_options, 1.0).store);
    expect(v4.version == "4.0" && v4.namespace_path == "/public" &&
        v4.known_namespace_paths == std::vector<std::string>{"/public/deep"} &&
        v4.known_exports == std::vector<std::string>{"/srv/public", "/other/public"},
        "Saving v4 settings must preserve separate paths for each protocol");
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
    expect(encoding, "Encoding must support an explicit override and inheritance");
    expect(std::string(gtk_entry_get_text(GTK_ENTRY(encoding))) == "EUC-JP", "Show the effective override");
    gtk_entry_set_text(GTK_ENTRY(encoding), "CP932//IGNORE");
    expect(!settings_widget_is_valid(widget), "An invalid encoding must prevent Save");
    gtk_entry_set_text(GTK_ENTRY(encoding), "");
    expect(settings_widget_is_valid(widget) && gtk_widget_get_sensitive(encoding) &&
        std::string(gtk_entry_get_placeholder_text(GTK_ENTRY(encoding))) == "CP932 (global default)" &&
        nfs_connection_settings(settings_widget_draft_store(widget)).filename_encoding == "CP932",
        "An empty editable entry must show and use the inherited encoding like other settings tabs");
    expect(!gtk_widget_get_visible(find_widget(root, "settings_ssh_proxy_page")), "NFS must not advertise an unsupported SSH proxy");
    auto *version = find_widget(root, "settings_nfs_version_combo");
    expect(std::string(gtk_combo_box_get_active_id(GTK_COMBO_BOX(version))) == "inherit",
        "The version selector must include inheritance in its choices");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(version), "4.0");
    expect(!gtk_widget_get_sensitive(find_widget(root, "settings_nfs_known_exports_combo")) &&
        gtk_widget_get_sensitive(find_widget(root, "settings_nfs_known_namespace_paths_combo")),
        "The v4 editor must distinguish namespace inputs from inactive v3 export settings");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(version), "inherit");
    expect(nfs_connection_settings(settings_widget_draft_store(widget)).version == "3",
        "Selecting inheritance must restore the global protocol version");
    auto *discovery = find_widget(root, "settings_nfs_discover_exports_combo");
    auto *reserved_port = find_widget(root, "settings_nfs_require_reserved_port_combo");
    expect(reserved_port && std::string(gtk_combo_box_get_active_id(GTK_COMBO_BOX(reserved_port))) == "disabled",
        "The source-port selector must show the connection override");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(reserved_port), "enabled");
    expect(nfs_connection_settings(settings_widget_draft_store(widget)).require_reserved_port,
        "Enable the source-port requirement from the settings page");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(reserved_port), "disabled");
    expect(!nfs_connection_settings(settings_widget_draft_store(widget)).require_reserved_port,
        "Disable the source-port requirement from the settings page");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(reserved_port), "inherit");
    expect(nfs_connection_settings(settings_widget_draft_store(widget)).require_reserved_port &&
        !setting_has_explicit_value(settings_widget_draft_store(widget), nfs_setting_key("require_reserved_port")),
        "The source-port selector must restore inheritance");
    expect(discovery, "Boolean settings must offer inherited, enabled and disabled choices");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(discovery), "disabled");
    expect(!nfs_connection_settings(settings_widget_draft_store(widget)).discover_exports,
        "Discovery can be disabled independently of the inherited value");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(discovery), "inherit");
    expect(nfs_connection_settings(settings_widget_draft_store(widget)).discover_exports,
        "The discovery choice can return to inheritance");
    auto *groups = find_widget(root, "settings_nfs_supplementary_gids_combo");
    auto *group_list = find_widget(root, "settings_nfs_supplementary_gids_list");
    expect(groups && group_list, "List settings must offer inherited and custom values in the control column");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(groups), "custom");
    auto *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(group_list));
    gtk_text_buffer_set_text(buffer, "", -1);
    expect(nfs_connection_settings(settings_widget_draft_store(widget)).supplementary_gids.empty() &&
        setting_has_explicit_value(settings_widget_draft_store(widget), nfs_setting_key("supplementary_gids")),
        "An explicit empty list must override inherited groups");
    gtk_text_buffer_set_text(buffer, "invalid", -1);
    expect(!settings_widget_is_valid(widget), "Invalid list values must prevent Save");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(groups), "inherit");
    expect(settings_widget_is_valid(widget) && !gtk_widget_get_sensitive(group_list) &&
        nfs_connection_settings(settings_widget_draft_store(widget)).supplementary_gids == std::vector<std::uint32_t>{1, 4294967295U},
        "Selecting inheritance must discard invalid list input and restore global groups");
    auto *port = find_widget(root, "settings_nfs_nfs_port_entry");
    gtk_entry_set_text(GTK_ENTRY(port), "65536");
    expect(!settings_widget_is_valid(widget), "Invalid numeric overrides must prevent Save");
    gtk_entry_set_text(GTK_ENTRY(port), "");
    expect(settings_widget_is_valid(widget) && nfs_connection_settings(settings_widget_draft_store(widget)).nfs_port == 0,
        "Clearing a numeric override must restore automatic port selection");
    expect(find_widget(root, "settings_nfs_ip_scan_button"), "NFS must offer address discovery like the other server settings");
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
    expect(!gtk_widget_get_sensitive(find_widget(runtime_root, "settings_nfs_require_reserved_port_combo")),
        "The active connection's source-port requirement must not change in place");
    destroy_settings_widget(runtime);
    std::cout << "PASS NFS settings and inheritance\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
