// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#include "../../src/smb/smb-client.h"
#include <array>
#include <iostream>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> test_async(int argc, char **argv) {
  require(argc == 5, "Usage: smb-client-test host share user port (ELDER_SMB_PASSWORD)");
  elder_terms::SmbClientOpenOptions options;
  options.connection.address = argv[1];
  options.connection.share = argv[2];
  options.connection.username = argv[3];
  options.connection.port = std::stoi(argv[4]);
  options.password = g_getenv("ELDER_SMB_PASSWORD");
  int disconnects = 0;
  options.disconnected = [&] { ++disconnects; };
  auto client = co_await elder_terms::open_smb_client_async(std::move(options), {});
  auto directory = co_await client->load_directory_async("/", {});
  require(directory.canonical_path == "/", "Share root differs");
  require(!directory.entries.empty(), "Expected server fixtures");
  const auto file = co_await client->lstat_async("/sample.txt", {});
  require(file && file->type == elder_terms::RemoteFileType::regular, "File attributes differ");
  require(!co_await client->lstat_async("/absent.txt", {}), "Missing file was reported present");
  auto reader = std::move(co_await client->open_read_async("/sample.txt", {}));
  std::string text;
  std::array<std::byte, 7> chunk{};
  for (;;) {
    const auto count = co_await reader->read_async(chunk, {});
    if (count == 0) break;
    text.append(reinterpret_cast<const char *>(chunk.data()), count);
  }
  co_await reader->close_async({});
  require(text == "SMB fixture content\n", "Downloaded content differs");
  for (const auto &path : {"/../sample.txt", "/outside-link/sentinel.txt", "/sample.txt:stream"}) {
    bool refused = false;
    try { (void)co_await client->open_read_async(path, {}); }
    catch (const std::exception &) { refused = true; }
    require(refused, "Unsafe or unsupported path was followed");
  }
  co_await elder_terms::stop_smb_client_async(client);
  require(disconnects == 1, "Session shutdown must notify its observer once");
  co_await elder_terms::stop_smb_client_async(client);
  require(disconnects == 1, "Repeated shutdown notified twice");
  bool stopped = false;
  try { (void)co_await client->load_directory_async("/", {}); }
  catch (const elder_terms::RemoteFileError &error) { stopped = error.connection_lost; }
  require(stopped, "Stopped client accepted another request");
  std::cout << "PASS SMB client listing/download/path boundaries/shutdown\n";
}

static cardio::promise<void> run_async(cardio::dispatcher_group_glib &group,
    int argc, char **argv, std::exception_ptr &failure) {
  try { co_await test_async(argc, argv); }
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
