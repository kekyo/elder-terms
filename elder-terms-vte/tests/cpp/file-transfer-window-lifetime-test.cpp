#include "file-transfer-window.h"
#include "activity-file-client.h"
#include "../sftp/sftp-fixture-client.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace elder_terms_file_transfer_window_lifetime_test {

using namespace elder_terms;

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Directory completion and cancellation are controlled here. Other operations
// retain the existing fixture's behavior rather than adding another filesystem mock.
class GatedClient final : public RemoteFileClient {
  std::shared_ptr<RemoteFileClient> delegate = create_sftp_fixture_client(false);
  cardio::primitives::manually_conditional pending{false};
public:
  bool complete_listing = false;
  cardio::primitives::manually_conditional listing_allowed{false};
  cardio::primitives::manually_conditional pane_enabled{false};
  cardio::primitives::manually_conditional started{false};
  cardio::primitives::manually_conditional cleanup_started{false};
  cardio::primitives::manually_conditional cleanup_allowed{false};
  bool cleanup_finished = false;

  RemoteFileCapabilities capabilities() const noexcept override { return delegate->capabilities(); }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    started.raise();
    if (complete_listing) {
      co_await listing_allowed.wait(cancellation);
      co_return RemoteDirectorySnapshot{.canonical_path = "/remote/loaded", .entries = {}};
    }
    try {
      co_await pending.wait(cancellation);
      throw std::runtime_error("Directory request must be canceled during window close");
    } catch (const cardio::canceled_exception &) {
    }
    cleanup_started.raise();
    // Cleanup deliberately outlives the canceled operation's token, as GIO
    // close/delete cleanup can do after a file transfer has been interrupted.
    co_await cleanup_allowed.wait();
    cleanup_finished = true;
    co_return RemoteDirectorySnapshot{.canonical_path = std::move(path), .entries = {}};
  }
  cardio::promise<std::optional<RemoteFileAttributes>> lstat_async(
      std::string path, cardio::cancellation cancellation) override {
    return delegate->lstat_async(std::move(path), cancellation);
  }
  cardio::promise<std::string> read_link_async(
      std::string path, cardio::cancellation cancellation) override {
    return delegate->read_link_async(std::move(path), cancellation);
  }
  cardio::promise<void> make_directory_async(
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    return delegate->make_directory_async(std::move(path), permissions, cancellation);
  }
  cardio::promise<void> remove_file_async(std::string path, cardio::cancellation cancellation) override {
    return delegate->remove_file_async(std::move(path), cancellation);
  }
  cardio::promise<void> remove_directory_async(std::string path, cardio::cancellation cancellation) override {
    return delegate->remove_directory_async(std::move(path), cancellation);
  }
  cardio::promise<void> rename_async(std::string source, std::string destination,
                                    cardio::cancellation cancellation) override {
    return delegate->rename_async(std::move(source), std::move(destination), cancellation);
  }
  cardio::promise<void> make_symbolic_link_async(std::string target, std::string path,
                                                cardio::cancellation cancellation) override {
    return delegate->make_symbolic_link_async(std::move(target), std::move(path), cancellation);
  }
  cardio::promise<void> set_attributes_async(std::string path, RemoteFileAttributes attributes,
                                            cardio::cancellation cancellation) override {
    return delegate->set_attributes_async(std::move(path), std::move(attributes), cancellation);
  }
  cardio::promise<std::unique_ptr<RemoteFileReader>> open_read_async(
      std::string path, cardio::cancellation cancellation) override {
    return delegate->open_read_async(std::move(path), cancellation);
  }
  cardio::promise<std::unique_ptr<RemoteFileWriter>> open_write_async(
      std::string path, std::uint64_t expected_size, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    return delegate->open_write_async(std::move(path), expected_size, permissions, cancellation);
  }
  bool try_begin_transfer() override { return delegate->try_begin_transfer(); }
  void end_transfer() override { delegate->end_transfer(); }
};

static GtkWidget *find_widget(GtkWidget *widget, const char *name) {
  if (std::strcmp(gtk_widget_get_name(widget), name) == 0) return widget;
  if (!GTK_IS_CONTAINER(widget)) return nullptr;
  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  GtkWidget *found = nullptr;
  for (GList *child = children; child != nullptr && found == nullptr; child = child->next) {
    found = find_widget(GTK_WIDGET(child->data), name);
  }
  g_list_free(children);
  return found;
}

static cardio::promise<void> verify_async(
    std::shared_ptr<FileTransferWindow> window, std::shared_ptr<GatedClient> client,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::optional<cardio::promise<void>> closing;
  try {
    std::vector<ActivityIndicatorId> activity;
    auto observed = create_activity_file_client(create_sftp_fixture_client(false),
        [&activity](ActivityIndicatorId id) { activity.push_back(id); });
    const auto listing = co_await observed->load_directory_async("/remote", {});
    expect(!listing.entries.empty() && activity == std::vector{ActivityIndicatorId::sd, ActivityIndicatorId::rd},
           "Directory requests and replies must report SD and RD");
    auto opening = observed->open_read_async("/remote/readme.txt", {});
    auto reader = std::move(co_await opening);
    activity.clear();
    std::array<std::byte, 128> bytes{};
    const auto size = co_await reader->read_async(bytes, {});
    expect(size > 0 && activity == std::vector{ActivityIndicatorId::rd},
           "Downloaded chunks must report RD");
    activity.clear();
    expect(co_await reader->read_async(bytes, {}) == 0 && activity.empty(),
           "EOF must not report received data");
    co_await reader->close_async({});
    auto creating = observed->open_write_async("/remote/upload.txt", size, std::nullopt, {});
    auto writer = std::move(co_await creating);
    activity.clear();
    co_await writer->write_all_async(std::span<const std::byte>(bytes.data(), size), {});
    expect(activity == std::vector{ActivityIndicatorId::sd}, "Uploaded chunks must report SD");
    co_await writer->close_async({});
    expect((co_await observed->lstat_async("/remote/upload.txt", {}))->size == size,
           "Activity observation must preserve the remote upload");

    GtkWidget *connection_image = find_widget(file_transfer_window_widget(window), "conn_indicator_image");
    expect(GTK_IS_IMAGE(connection_image), "File browser must expose CONN");
    auto *off_icon = gtk_image_get_pixbuf(GTK_IMAGE(connection_image));
    show_file_transfer_window(window);
    attach_file_transfer_window_client(window, client);
    co_await client->started.wait();
    expect(gtk_image_get_pixbuf(GTK_IMAGE(connection_image)) != off_icon,
           "Attaching an authenticated service must activate CONN");
    GtkWidget *root = file_transfer_window_widget(window);
    for (const char *id : {"conn_indicator_image", "sd_indicator_image", "rd_indicator_image"}) {
      expect(GTK_IS_IMAGE(find_widget(root, id)), "File browser must expose CONN, SD and RD indicators");
    }
    GtkWidget *menu_button = find_widget(gtk_window_get_titlebar(GTK_WINDOW(root)), "application_menu_button");
    expect(GTK_IS_MENU_BUTTON(menu_button), "File browser must expose a settings menu button");
    GtkWidget *menu = GTK_WIDGET(gtk_menu_button_get_popup(GTK_MENU_BUTTON(menu_button)));
    expect(find_widget(menu, "settings_menu_item") != nullptr &&
           find_widget(menu, "about_menu_item") != nullptr,
           "File browser menu must offer Settings and About");
    GtkWidget *frame = find_widget(root, "file_transfer_remote_group");
    GtkWidget *tree = find_widget(root, "file_transfer_remote_tree");
    GtkWidget *path = find_widget(root, "file_transfer_remote_path_entry");
    expect(frame != nullptr && tree != nullptr && path != nullptr,
           "The remote browser must expose its controls");
    expect(!gtk_widget_is_sensitive(tree) && !gtk_widget_is_sensitive(path),
           "Directory loading must prevent selecting stale rows or editing an overwritten path");
    GList *menus = gtk_menu_get_for_attach_widget(tree);
    expect(menus != nullptr, "The remote browser must expose its context menu");
    GtkWidget *rename = find_widget(GTK_WIDGET(menus->data), "file_transfer_remote_rename_item");
    GtkWidget *remove = find_widget(GTK_WIDGET(menus->data), "file_transfer_remote_delete_item");
    expect(rename != nullptr && remove != nullptr,
           "The browser context menu must expose rename and delete");
    expect(!gtk_widget_is_sensitive(rename) && !gtk_widget_is_sensitive(remove),
           "Directory loading must disable actions in the separately attached menu");
    if (client->complete_listing) {
      g_signal_connect(frame, "notify::sensitive",
          G_CALLBACK(+[](GtkWidget *widget, GParamSpec *, gpointer data) {
            if (gtk_widget_is_sensitive(widget)) {
              static_cast<GatedClient *>(data)->pane_enabled.raise();
            }
          }), client.get());
      client->listing_allowed.raise();
      co_await client->pane_enabled.wait();
      expect(gtk_widget_is_sensitive(tree) && gtk_widget_is_sensitive(path),
             "The browser must accept input after directory loading completes");
      expect(gtk_widget_is_sensitive(rename) && gtk_widget_is_sensitive(remove),
             "The context menu must become available after directory loading");
      expect(std::strcmp(gtk_entry_get_text(GTK_ENTRY(path)), "/remote/loaded") == 0,
             "The canonical path must be applied before input is enabled");
      g_signal_emit_by_name(find_widget(menu, "settings_menu_item"), "activate");
      GList *toplevels = gtk_window_list_toplevels();
      GtkWidget *settings_dialog = nullptr;
      for (auto *item = toplevels; item != nullptr; item = item->next) {
        auto *candidate = GTK_WIDGET(item->data);
        if (std::strcmp(gtk_widget_get_name(candidate), "settings_dialog") == 0) settings_dialog = candidate;
      }
      g_list_free(toplevels);
      expect(settings_dialog != nullptr &&
             find_widget(settings_dialog, "settings_widget_root") != nullptr,
             "Settings menu must open the shared runtime settings editor");
      expect(gtk_window_get_transient_for(GTK_WINDOW(settings_dialog)) == GTK_WINDOW(root),
             "Settings must belong to the file browser");
      set_file_transfer_window_connection_available(window, false);
      expect(gtk_image_get_pixbuf(GTK_IMAGE(connection_image)) == off_icon,
             "Disconnected service must deactivate CONN");
      closing.emplace(close_file_transfer_window_async(window));
      co_await *closing;
    } else {
      closing.emplace(close_file_transfer_window_async(window));
      co_await client->cleanup_started.wait();
      expect(file_transfer_window_widget(window) == nullptr,
             "Window close must destroy the GTK window before waiting for cleanup");
      expect(!closing->is_ready(),
             "Window close must wait for canceled operations to finish cleanup");
      client->cleanup_allowed.raise();
      co_await *closing;
      expect(client->cleanup_finished,
             "Window close returned before canceled directory cleanup finished");
      co_await close_file_transfer_window_async(window);
    }
  } catch (...) {
    failure = std::current_exception();
    client->cleanup_allowed.raise();
    if (!closing) closing.emplace(close_file_transfer_window_async(window));
  }
  if (closing) {
    try { co_await *closing; } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  }
  group.shutdown();
}

} // namespace elder_terms_file_transfer_window_lifetime_test

int main(int argc, char **argv) {
  using namespace elder_terms_file_transfer_window_lifetime_test;
  gtk_init(&argc, &argv);
  std::array<char, 64> path{};
  const std::string pattern = "/tmp/elder-terms-window-lifetime-XXXXXX";
  std::copy(pattern.begin(), pattern.end(), path.begin());
  if (::mkdtemp(path.data()) == nullptr) return 1;
  std::exception_ptr failure;
  for (const bool complete_listing : {false, true}) {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    auto client = std::make_shared<GatedClient>();
    client->complete_listing = complete_listing;
    auto window = create_file_transfer_window({
        .connection_name = "Window lifetime", .protocol_name = "FTP",
        .local_directory = path.data(), .remote_directory = "/remote",
        .remote_file_hash = {}, .colors = {}, .closed = {},
        .settings = create_default_settings({}, "Window lifetime")});
    auto task = verify_async(window, client, group, failure);
    dispatcher.park();
    if (failure) break;
  }
  std::filesystem::remove_all(path.data());
  try {
    if (failure) std::rethrow_exception(failure);
    std::cout << "file-transfer-window-lifetime-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "file-transfer-window-lifetime-test: FAIL: " << error.what() << '\n';
    return 1;
  }
}
