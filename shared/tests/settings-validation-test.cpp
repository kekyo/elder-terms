// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>
#include <cassert>
#include <cstring>
#include <iostream>

static GtkWidget *find(GtkWidget *widget, const char *id) {
  if (std::strcmp(gtk_widget_get_name(widget), id) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next)
    result = find(GTK_WIDGET(item->data), id);
  g_list_free(children);
  return result;
}

static std::string tooltip(GtkWidget *widget) {
  auto *text = gtk_widget_get_tooltip_text(widget);
  const std::string result = text ? text : "";
  g_free(text);
  return result;
}

int main(int argc, char **argv) {
  using namespace elder_terms;
  gtk_init(&argc, &argv);
  SettingsWidgetOptions options;
  options.store = create_default_settings(default_terminal_display_settings(12), "Validation test");
  const std::string scenario = argc > 1 ? argv[1] : "scalar";
  if (scenario == "smb" || scenario == "http" || scenario == "ftp" || scenario == "webdav" || scenario == "nfs")
    set_explicit_setting_value(&options.store, general_type_setting_key(), scenario);
  if (scenario == "macro") options.store.macro_rules = {
      {.id = "test", .pattern = "(test)", .action = MacroCommandAction{"echo", {"${2}"}}}};
  if (scenario == "links") options.store.hyperlink_rules = {
      {.id = "test", .pattern = "(test)", .command = "echo", .arguments = {"${2}"}}};
  auto *state = create_settings_widget(std::move(options));
  auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  auto *root = settings_widget_root(state);
  gtk_container_add(GTK_CONTAINER(window), root);
  g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    destroy_settings_widget(static_cast<SettingsWidgetState *>(data));
  }), state);
  auto *apply = find(root, "settings_apply_button");
  if (scenario == "smb") {
    auto *address = find(root, "settings_smb_address_entry");
    assert(!gtk_widget_get_sensitive(apply));
    assert(!tooltip(address).empty());
    gtk_entry_set_text(GTK_ENTRY(address), "server.example");
    assert(tooltip(address).empty());
    assert(gtk_widget_get_sensitive(apply));
    auto *version = find(root, "settings_smb_version_combo");
    auto *proxy = find(root, "settings_ssh_proxy_enabled_combo");
    gtk_entry_set_text(GTK_ENTRY(find(root, "settings_ssh_proxy_address_entry")), "gateway.example");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(version), "1");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(proxy), "true");
    assert(!gtk_widget_get_sensitive(apply));
    assert(tooltip(proxy).find("SMB") != std::string::npos);
    assert(gtk_style_context_has_class(gtk_widget_get_style_context(proxy), GTK_STYLE_CLASS_ERROR));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(version), "3");
    assert(tooltip(proxy).empty());
    assert(gtk_widget_get_sensitive(apply));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(version), "1");
    assert(!gtk_widget_get_sensitive(apply));
    assert(tooltip(proxy).find("SMB") != std::string::npos);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(proxy), "false");
    assert(tooltip(proxy).empty());
    assert(gtk_widget_get_sensitive(apply));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(version), "inherit");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(proxy), "inherit");
    auto inherited = create_default_settings(default_terminal_display_settings(12), "Defaults");
    set_explicit_setting_value(&inherited, smb_setting_key("version"), std::string("1"));
    set_explicit_setting_value(&inherited, ssh_proxy_setting_key("enabled"), true);
    settings_widget_rebase_fallbacks(state, inherited);
    assert(!gtk_widget_get_sensitive(apply));
    assert(!tooltip(proxy).empty() && !tooltip(version).empty());
    auto *port = find(root, "settings_smb_port_entry");
    gtk_entry_set_text(GTK_ENTRY(port), "unfinished");
    set_explicit_setting_value(&inherited, smb_setting_key("version"), std::string("3"));
    settings_widget_rebase_fallbacks(state, inherited);
    assert(tooltip(proxy).empty());
    assert(std::string(gtk_entry_get_text(GTK_ENTRY(port))) == "unfinished");
    assert(!tooltip(port).empty() && !gtk_widget_get_sensitive(apply));
    gtk_entry_set_text(GTK_ENTRY(port), "445");
    assert(tooltip(port).empty() && gtk_widget_get_sensitive(apply));
  } else if (scenario == "http") {
    auto *base = find(root, "settings_http_base_url_entry");
    assert(!gtk_widget_get_sensitive(apply));
    assert(!tooltip(base).empty());
    gtk_entry_set_text(GTK_ENTRY(base), "https://example.test/files/");
    assert(gtk_widget_get_sensitive(apply));
    auto *start = find(root, "settings_http_start_url_entry");
    gtk_entry_set_text(GTK_ENTRY(start), "https://example.test/outside/");
    assert(!gtk_widget_get_sensitive(apply));
    assert(!tooltip(start).empty());
    gtk_entry_set_text(GTK_ENTRY(start), "index.html");
    assert(tooltip(start).empty());
    assert(gtk_widget_get_sensitive(apply));
  } else if (scenario == "ftp") {
    auto *minimum = find(root, "settings_ftp_tls_min_version_combo");
    auto *maximum = find(root, "settings_ftp_tls_max_version_combo");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(root, "settings_ftp_tls_mode_combo")), "explicit");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(minimum), "1.3");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(maximum), "1.2");
    assert(!gtk_widget_get_sensitive(apply));
    assert(!tooltip(minimum).empty() && !tooltip(maximum).empty());
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(maximum), "1.3");
    assert(tooltip(minimum).empty() && tooltip(maximum).empty());
    assert(gtk_widget_get_sensitive(apply));
  } else if (scenario == "webdav") {
    auto *port = find(root, "settings_webdav_port_entry");
    gtk_entry_set_text(GTK_ENTRY(port), "invalid");
    assert(!gtk_widget_get_sensitive(apply) && !tooltip(port).empty());
    gtk_entry_set_text(GTK_ENTRY(port), "443");
    assert(gtk_widget_get_sensitive(apply) && tooltip(port).empty());
  } else if (scenario == "nfs") {
    auto *address = find(root, "settings_nfs_address_entry");
    assert(!gtk_widget_get_sensitive(apply) && !tooltip(address).empty());
    gtk_entry_set_text(GTK_ENTRY(address), "server.example");
    assert(gtk_widget_get_sensitive(apply) && tooltip(address).empty());
    assert(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(root, "settings_nfs_discover_exports_combo")), "disabled"));
    auto *path = find(root, "settings_nfs_export_path_entry");
    assert(!gtk_widget_get_sensitive(apply) && !tooltip(path).empty());
    gtk_entry_set_text(GTK_ENTRY(path), "/export");
    assert(gtk_widget_get_sensitive(apply) && tooltip(path).empty());
  } else if (scenario == "macro" || scenario == "links") {
    const auto prefix = scenario == "macro" ? "settings_macro_" : "settings_link_";
    auto *argument = find(root, (std::string(prefix) + "argument_0_entry").c_str());
    auto *command = find(root, (std::string(prefix) + "command_entry").c_str());
    assert(argument && command);
    assert(!gtk_widget_get_sensitive(apply) && !tooltip(argument).empty());
    assert(!gtk_style_context_has_class(gtk_widget_get_style_context(command), GTK_STYLE_CLASS_ERROR));
    gtk_entry_set_text(GTK_ENTRY(argument), "${1}");
    assert(gtk_widget_get_sensitive(apply) && tooltip(argument).empty());
  } else {
  auto *width = find(root, "settings_terminal_width_entry");
  assert(width && apply);
  gtk_entry_set_text(GTK_ENTRY(width), "invalid");
  assert(!gtk_widget_get_sensitive(apply));
  assert(gtk_style_context_has_class(gtk_widget_get_style_context(width), GTK_STYLE_CLASS_ERROR));
  // The whole input must explain why Apply is blocked, including away from its icon.
  assert(!tooltip(width).empty());
  gtk_entry_set_text(GTK_ENTRY(width), "88");
  assert(gtk_widget_get_sensitive(apply));
  assert(!gtk_style_context_has_class(gtk_widget_get_style_context(width), GTK_STYLE_CLASS_ERROR));
  assert(tooltip(width).empty());

  auto *strength = find(root, "settings_terminal_color_vision_strength_entry");
  assert(strength);
  const auto original = tooltip(strength);
  gtk_entry_set_text(GTK_ENTRY(strength), "101");
  assert(!gtk_widget_get_sensitive(apply));
  assert(!tooltip(strength).empty());
  assert(original != tooltip(strength));
  gtk_entry_set_text(GTK_ENTRY(strength), "50");
  assert(gtk_widget_get_sensitive(apply));
  assert(original == tooltip(strength));
  }

  gtk_widget_destroy(window);
  std::cout << "PASS field errors explain blocked settings and restore normal help\n";
}
