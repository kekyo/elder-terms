// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "nfs-application.h"
#include "nfs-client.h"
#include "../file-transfer/file-transfer-window.h"
#include "../file-transfer/file-transfer-paths.h"
#include <elder-terms/connection-desktop-session.h>
#include <iostream>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct NfsApplication {
  cardio::dispatcher_group_glib *dispatcher_group;
  NfsConnectionSettings connection;
  std::shared_ptr<RemoteFileClient> client;
  std::shared_ptr<FileTransferWindow> window;
  cardio::cancellation_source stopping;
  std::optional<cardio::promise<void>> startup;
  std::optional<cardio::promise<void>> shutdown;
  bool shutting_down = false;
};

static cardio::promise<void> finish_application_async(NfsApplication *state) {
  try { if (state->startup) co_await *state->startup; }
  catch (const cardio::canceled_exception &) {}
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  auto closing = close_file_transfer_window_async(state->window);
  try {
    if (state->client) co_await stop_nfs_client_async(state->client);
    co_await closing;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  shutdown_connection_desktop_session();
  state->dispatcher_group->shutdown();
}

static void stop_application(NfsApplication *state) {
  if (state->shutting_down) return;
  state->shutting_down = true;
  (void)state->stopping.cancel();
  state->shutdown.emplace(finish_application_async(state));
}

static cardio::promise<void> start_application_async(NfsApplication *state) {
  std::string failure;
  const auto cancellation = state->stopping.get_cancellation();
  try {
    if (!state->connection.validation_errors.empty()) throw std::invalid_argument(state->connection.validation_errors.front());
    NfsClientOpenOptions options;
    options.address = state->connection.address;
    options.version = state->connection.version == "4.0" ? 4 : state->connection.version == "2" ? 2 : 3;
    options.nfs_port = static_cast<int>(state->connection.nfs_port);
    options.mount_port = static_cast<int>(state->connection.mount_port);
    options.filename_encoding = state->connection.filename_encoding;
    options.known_exports = state->connection.known_exports;
    if (!state->connection.export_path.empty()) options.known_exports.push_back(state->connection.export_path);
    options.discover_exports = state->connection.discover_exports;
    options.known_namespace_paths = state->connection.known_namespace_paths;
    if (!state->connection.namespace_path.empty()) options.known_namespace_paths.push_back(state->connection.namespace_path);
    options.discover_namespace = state->connection.discover_namespace;
    options.uid = state->connection.uid;
    options.gid = state->connection.gid;
    options.supplementary_gids = state->connection.supplementary_gids;
    options.connect_timeout_ms = static_cast<int>(state->connection.connect_timeout_seconds * 1000);
    options.request_timeout_ms = static_cast<int>(state->connection.request_timeout_seconds * 1000);
    state->client = co_await open_nfs_server_async(std::move(options), cancellation);
    const auto initial = nfs_export_identity(state->client, state->connection.version == "4.0" ?
        state->connection.namespace_path : state->connection.export_path);
    attach_file_transfer_window_client(state->window, state->client, initial);
    co_return;
  } catch (const cardio::canceled_exception &) { stop_application(state); co_return; }
  catch (const std::exception &error) { failure = error.what(); }
  if (!state->shutting_down) {
    std::cerr << failure << '\n';
    co_await show_file_transfer_window_connection_error_async(
        state->window, _("Failed to start NFS"), std::move(failure), cancellation);
  }
}

static cardio::promise<void> reconnect_application_async(NfsApplication *state, SettingsStore settings) {
  if (state->startup) co_await *state->startup;
  if (state->client) co_await stop_nfs_client_async(state->client);
  state->client.reset();
  if (state->shutting_down) co_return;
  state->connection = nfs_connection_settings(settings);
  co_await start_application_async(state);
}

int run_nfs_application(const SettingsLoadResult &settings, std::optional<std::filesystem::path> config_path) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  NfsApplication state;
  state.dispatcher_group = &group;
  state.connection = nfs_connection_settings(settings.store);
  state.window = create_file_transfer_window({
      .connection_name = general_connection_name(settings.store), .protocol_name = "NFS v" + state.connection.version,
      .local_directory = resolve_file_transfer_local_directory(settings.store, state.connection.local_directory),
      .remote_directory = "/", .remote_file_hash = {}, .colors = general_color_settings(settings.store),
      .closed = [&state] { stop_application(&state); }, .settings = settings.store, .config_path = std::move(config_path),
      .reconnect = [&state](SettingsStore store) { return reconnect_application_async(&state, std::move(store)); }});
  show_file_transfer_window(state.window);
  state.startup.emplace(start_application_async(&state));
  dispatcher.park();
  return 0;
}

} // namespace elder_terms
