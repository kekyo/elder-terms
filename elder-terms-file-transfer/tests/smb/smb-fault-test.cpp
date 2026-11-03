// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb-client.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> wait_for_gate_async() {
  (void)co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char gate{};
  require(::read(STDIN_FILENO, &gate, 1) == 1 && gate == 'r', "Expected fault gate");
}

static cardio::promise<void> cancel_on_input_async(cardio::cancellation_source &cancellation) {
  (void)co_await cardio::from_fd(STDIN_FILENO, cardio::fd_event::read);
  char trigger{};
  require(::read(STDIN_FILENO, &trigger, 1) == 1 && trigger == 'c', "Expected cancel trigger");
  (void)cancellation.cancel();
}

static cardio::promise<std::shared_ptr<elder_terms::RemoteFileClient>> connect_async(
    const std::string &host, const std::string &share, const std::string &user,
    int port, int &disconnects, int idle_seconds) {
  elder_terms::SmbClientOpenOptions options;
  options.connection.address = host;

  options.connection.username = user;
  options.connection.port = port;
  options.connection.idle_timeout_seconds = idle_seconds;
  options.password = g_getenv("ELDER_SMB_PASSWORD");
  options.disconnected = [&disconnects] { ++disconnects; };
  co_return co_await elder_terms::open_smb_client_async(std::move(options), share, {});
}

static cardio::promise<void> write_text_async(
    const std::shared_ptr<elder_terms::RemoteFileClient> &client,
    const std::string &path, const std::string &text) {
  auto writer = std::move(co_await client->open_write_async(path, text.size(), {}, {}));
  co_await writer->write_all_async(std::as_bytes(std::span(text)), {});
  co_await writer->close_async({});
}

static cardio::promise<void> read_fault_async(int argc, char **argv) {
  using namespace elder_terms;
  require(argc == 6, "Usage: smb-fault-test host share user port mode");
  const std::string mode = argv[5];
  require(mode == "cancel-read" || mode == "disconnect-read" || mode == "timeout-read",
          "Unsupported fault mode");
  const std::string host = argv[1], share = argv[2], user = argv[3];
  const int port = std::stoi(argv[4]);
  int disconnects = 0;
  auto client = co_await connect_async(host, share, user, port, disconnects,
      mode == "timeout-read" ? 1 : 30);
  auto *uuid = g_uuid_string_random();
  const std::string root = "/elder-fault-" + std::string(uuid);
  g_free(uuid);
  co_await client->make_directory_async(root, {}, {});
  co_await write_text_async(client, root + "/source.txt", "before");
  auto reader = std::move(co_await client->open_read_async(root + "/source.txt", {}));
  std::cout << "READY SMB fault " << mode << std::endl;
  co_await wait_for_gate_async();
  cardio::cancellation_source cancellation;
  cardio::promise<void> cancel_task;
  if (mode == "cancel-read") cancel_task = cancel_on_input_async(cancellation);
  bool interrupted = false;
  std::array<std::byte, 16> bytes{};
  try {
    (void)co_await reader->read_async(bytes, cancellation.get_cancellation());
  } catch (const cardio::canceled_exception &) {
    require(mode == "cancel-read", "Unexpected read cancellation");
    interrupted = true;
  } catch (const RemoteFileError &error) {
    require(mode != "cancel-read" && error.connection_lost, "Read fault did not lose connection");
    interrupted = true;
  }
  if (mode == "cancel-read") co_await cancel_task;
  require(interrupted, "Fault read unexpectedly succeeded");
  require(disconnects == 1, "Interrupted session did not notify exactly once");
  reader.reset();
  bool stopped = false;
  try { (void)co_await client->load_directory_async(root, {}); }
  catch (const RemoteFileError &error) { stopped = error.connection_lost; }
  require(stopped, "Old SMB session accepted a request");
  co_await stop_smb_client_async(client);
  require(disconnects == 1, "Stopped session notified twice");
  int recovered_disconnects = 0;
  auto recovered = co_await connect_async(host, share, user, port, recovered_disconnects, 30);
  auto reopened = std::move(co_await recovered->open_read_async(root + "/source.txt", {}));
  const auto count = co_await reopened->read_async(bytes, {});
  require(count == 6 && std::string(reinterpret_cast<char *>(bytes.data()), count) == "before",
          "Recovered session read different content");
  co_await reopened->close_async({});
  co_await recovered->remove_file_async(root + "/source.txt", {});
  co_await recovered->remove_directory_async(root, {});
  co_await stop_smb_client_async(recovered);
  require(recovered_disconnects == 1, "Recovered session did not close exactly once");
  std::cout << "PASS SMB fault " << mode << " reconnect/content/cleanup" << std::endl;
}

static cardio::promise<void> upload_fault_async(int argc, char **argv) {
  using namespace elder_terms;
  (void)argc;
  const std::string mode = argv[5];
  require(mode == "cancel-write" || mode == "cancel-create" || mode == "disconnect-create",
          "Unsupported upload fault mode");
  const std::string host = argv[1], share = argv[2], user = argv[3];
  const int port = std::stoi(argv[4]);
  int disconnects = 0;
  auto client = co_await connect_async(host, share, user, port, disconnects, 30);
  auto *uuid = g_uuid_string_random();
  const std::string root = "/elder-fault-" + std::string(uuid);
  g_free(uuid);
  co_await client->make_directory_async(root, {}, {});
  co_await write_text_async(client, root + "/destination.txt", "original");
  char pattern[] = "/tmp/elder-smb-fault-XXXXXX";
  require(::mkdtemp(pattern) != nullptr, "Could not create local fault fixture");
  const std::filesystem::path local(pattern);
  const auto source = local / "destination.txt";
  std::ofstream(source, std::ios::binary) << std::string(256 * 1024, 'r');
  std::cout << "READY SMB fault " << mode << std::endl;
  co_await wait_for_gate_async();
  cardio::cancellation_source cancellation;
  cardio::promise<void> cancel_task;
  if (mode != "disconnect-create") cancel_task = cancel_on_input_async(cancellation);
  FileTransferRequest request;
  request.direction = FileTransferDirection::send;
  request.source_paths.push_back(source.string());
  request.destination_directory = root;
  request.callbacks.conflict = [](const FileTransferConflict &, cardio::cancellation)
      -> cardio::promise<FileTransferConflictAction> { co_return FileTransferConflictAction::overwrite; };
  std::string temporary;
  bool cleanup_failure = false;
  try {
    co_await run_file_transfer_async(client, std::move(request), cancellation.get_cancellation());
  } catch (const FileTransferCleanupFailure &error) {
    temporary = error.temporary_path;
    require(temporary.starts_with(root + "/destination.txt.elder-terms-part-"),
            "Cleanup failure did not expose its upload path");
    require(error.cancelled == (mode != "disconnect-create"), "Cleanup cancellation flag differs");
    if (mode == "cancel-create" || mode == "disconnect-create")
      require(std::string(error.what()).find("ownership is uncertain") != std::string::npos,
              "Unknown CREATE result was reported as owned");
    cleanup_failure = true;
  }
  if (mode != "disconnect-create") co_await cancel_task;
  require(cleanup_failure, "Fault upload did not report cleanup uncertainty");
  require(disconnects == 1, "Interrupted upload did not notify exactly once");
  bool stopped = false;
  try { (void)co_await client->load_directory_async(root, {}); }
  catch (const RemoteFileError &error) { stopped = error.connection_lost; }
  require(stopped, "Old SMB session accepted another upload operation");
  co_await stop_smb_client_async(client);
  require(disconnects == 1, "Stopped upload session notified twice");
  int recovered_disconnects = 0;
  auto recovered = co_await connect_async(host, share, user, port, recovered_disconnects, 30);
  auto reader = std::move(co_await recovered->open_read_async(root + "/destination.txt", {}));
  std::array<std::byte, 16> bytes{};
  const auto count = co_await reader->read_async(bytes, {});
  require(count == 8 && std::string(reinterpret_cast<char *>(bytes.data()), count) == "original",
          "Fault upload changed original destination");
  co_await reader->close_async({});
  require((co_await recovered->lstat_async(temporary, {})).has_value(),
          "Uncertain temporary upload was removed automatically");
  co_await recovered->remove_file_async(temporary, {});
  co_await recovered->remove_file_async(root + "/destination.txt", {});
  co_await recovered->remove_directory_async(root, {});
  co_await stop_smb_client_async(recovered);
  require(recovered_disconnects == 1, "Recovered upload session did not close exactly once");
  std::error_code ignored;
  std::filesystem::remove_all(local, ignored);
  std::cout << "PASS SMB fault " << mode << " reconnect/content/cleanup" << std::endl;
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib &group,
    int argc, char **argv, std::exception_ptr &failure) {
  try {
    require(argc == 6, "Usage: smb-fault-test host share user port mode");
    const std::string mode = argv[5];
    if (mode == "cancel-read" || mode == "disconnect-read" || mode == "timeout-read")
      co_await read_fault_async(argc, argv);
    else
      co_await upload_fault_async(argc, argv);
  }
  catch (...) { failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib_auto dispatcher(group);
  std::exception_ptr failure;
  auto pending = run_async(group, argc, argv, failure);
  dispatcher.park();
  if (failure) {
    try { std::rethrow_exception(failure); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
    return 1;
  }
  return 0;
}
