#include <elder-terms/settings-widget.h>

#include <iostream>
#include <stdexcept>
#include <string_view>

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

static GtkWidget *find_widget(GtkWidget *root, std::string_view name) {
  if (name == gtk_widget_get_name(root)) return root;
  if (!GTK_IS_CONTAINER(root)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(root));
  GtkWidget *result = nullptr;
  for (auto *item = children; item && !result; item = item->next)
    result = find_widget(GTK_WIDGET(item->data), name);
  g_list_free(children);
  return result;
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  try {
    using namespace elder_terms;
    for (const auto *type : {"telnet", "ssh", "local", "serial"}) {
      auto store = create_default_settings({}, "proxy test");
      set_explicit_setting_value(&store, general_type_setting_key(), std::string(type));
      SettingsWidgetOptions options;
      options.store = store;
      auto *state = create_settings_widget(std::move(options));
      auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
      auto *root = settings_widget_root(state);
      gtk_container_add(GTK_CONTAINER(window), root);
      g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
        elder_terms::destroy_settings_widget(static_cast<elder_terms::SettingsWidgetState *>(data));
      }), state);
      auto *page = find_widget(root, "settings_ssh_proxy_page");
      require(page != nullptr, "SSH proxy must have an independent settings page");
      const bool network = std::string_view(type) == "telnet" || std::string_view(type) == "ssh";
      require(static_cast<bool>(gtk_widget_get_visible(page)) == network, "proxy page visibility must follow connection type");
      if (network) {
        auto *enabled = find_widget(root, "settings_ssh_proxy_enabled_combo");
        auto *address = find_widget(root, "settings_ssh_proxy_address_entry");
        auto *port = find_widget(root, "settings_ssh_proxy_port_entry");
        require(enabled && address && port, "proxy fields must be accessible");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(enabled), "true");
        require(!settings_widget_is_valid(state), "enabled proxy must require a gateway");
        gtk_entry_set_text(GTK_ENTRY(address), "bastion.example");
        gtk_entry_set_text(GTK_ENTRY(port), "2222");
        require(settings_widget_is_valid(state), "valid proxy must be applicable");
        const auto proxy = ssh_proxy_settings(settings_widget_draft_store(state));
        require(proxy.enabled && proxy.endpoint.address == "bastion.example" && proxy.endpoint.port == 2222,
                "UI must update the independent proxy settings");
        gtk_entry_set_text(GTK_ENTRY(port), "invalid");
        require(!settings_widget_is_valid(state), "unfinished port must prevent Apply");
        settings_widget_rebase_fallbacks(state, store);
        require(!settings_widget_is_valid(state), "rebasing must preserve unfinished proxy edits");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(enabled), "false");
        require(settings_widget_is_valid(state), "disabled proxy must not require an endpoint");
      }
      gtk_widget_destroy(window);
    }
    for (const bool runtime : {false, true}) {
      SettingsWidgetOptions options;
      options.store = create_default_settings({}, "proxy defaults");
      options.mode = SettingsWidgetMode::global_defaults;
      options.is_runtime = runtime;
      auto *state = create_settings_widget(std::move(options));
      auto *root = settings_widget_root(state);
      require(gtk_widget_get_visible(find_widget(root, "settings_ssh_proxy_page")),
              "global settings must display SSH proxy independently of connection type");
      require(static_cast<bool>(gtk_widget_get_sensitive(find_widget(root, "settings_ssh_proxy_enabled_combo"))) == !runtime,
              "runtime proxy settings must be read only");
      destroy_settings_widget(state);
    }
    std::cout << "SSH proxy widget: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
