// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include <elder-terms/settings-widget.h>

#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

static GtkWidget *find_widget(GtkWidget *root, const std::string &name) {
  if (name == gtk_widget_get_name(root)) return root;
  if (!GTK_IS_CONTAINER(root)) return nullptr;
  auto *children = gtk_container_get_children(GTK_CONTAINER(root));
  GtkWidget *found = nullptr;
  for (auto *child = children; child != nullptr && found == nullptr;
       child = child->next) {
    found = find_widget(GTK_WIDGET(child->data), name);
  }
  g_list_free(children);
  return found;
}

static cardio::promise<std::string> pending_name_async(
    cardio::promise_source<void> *started,
    std::shared_ptr<cardio::promise_source<std::string>> response,
    std::shared_ptr<bool> lifetime) {
  (void)lifetime;
  started->resolve();
  co_return co_await response->get_promise();
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib *group,
                                       std::exception_ptr *error) {
  GtkWidget *window = nullptr;
  try {
    for (const auto *type : {"webdav", "smb"}) {
      cardio::promise_source<void> started;
      auto response = std::make_shared<cardio::promise_source<std::string>>();
      std::weak_ptr<bool> lookup_lifetime;
      cardio::cancellation scan_signal;
      elder_terms::SettingsWidgetOptions options;
      options.store = elder_terms::create_default_settings({}, "Scan test");
      elder_terms::set_explicit_setting_value(
          &options.store, elder_terms::make_setting_key("general", "type"),
          std::string(type));
      options.ip_scanner_dependencies_factory = [&] {
        return elder_terms::IpScannerDependencies{
            .interfaces = {{UINT32_C(0xc0000219), UINT32_MAX}},
            .maximum_concurrent_hosts = 1,
            .probe_port = [&](std::uint32_t, std::uint16_t port,
                              cardio::cancellation signal) {
              scan_signal = signal;
              return cardio::resolved(port == 80 || port == 445);
            },
            .reverse_lookup = [&](std::uint32_t, cardio::cancellation) {
              auto lifetime = std::make_shared<bool>(true);
              lookup_lifetime = lifetime;
              return pending_name_async(&started, response, std::move(lifetime));
            },
            .name_lookup_timeout = [](std::uint64_t) {
              return cardio::cancellation_source{};
            },
        };
      };
      auto *state = elder_terms::create_settings_widget(std::move(options));
      window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
      g_signal_connect(window, "destroy",
          G_CALLBACK(+[](GtkWidget *, gpointer data) {
            elder_terms::destroy_settings_widget(
                static_cast<elder_terms::SettingsWidgetState *>(data));
          }), state);
      gtk_container_add(GTK_CONTAINER(window),
                        elder_terms::settings_widget_root(state));
      gtk_widget_show_all(window);
      auto *button = find_widget(window,
          std::string("settings_") + type + "_ip_scan_button");
      if (button == nullptr) throw std::runtime_error("Missing IP scan button");
      gtk_button_clicked(GTK_BUTTON(button));
      co_await started.get_promise();
      // A promise owns its coroutine. Closing settings cancels the scan and
      // destroys the pending lookup, so a later response has no UI recipient.
      gtk_widget_destroy(window);
      window = nullptr;
      if (!scan_signal.is_cancellation_requested() || !lookup_lifetime.expired())
        throw std::runtime_error("Closing settings must cancel and release the scan");
      (void)response->try_resolve(std::string("late.example.test"));
    }
  } catch (...) {
    if (window != nullptr) gtk_widget_destroy(window);
    *error = std::current_exception();
  }
  group->shutdown();
}

int main(int argc, char **argv) {
  gtk_init(&argc, &argv);
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  std::exception_ptr error;
  auto task = run_async(&group, &error);
  dispatcher.park();
  if (error != nullptr) {
    try {
      std::rethrow_exception(error);
    } catch (const std::exception &failure) {
      std::cerr << failure.what() << '\n';
    }
    return 1;
  }
  return 0;
}
