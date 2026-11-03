// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include "http-client.h"
#include "../../src/file-transfer/file-transfer-engine.h"
#include "../../src/file-transfer/file-transfer-job.h"
#include <filesystem>
#include <fstream>
#include <iostream>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
static std::string contents(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static cardio::promise<void> run_async(
    std::string base, std::string directory, cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  using namespace elder_terms;
  std::shared_ptr<RemoteFileClient> client;
  try {
    HttpClientOpenOptions options;
    options.base_url = base;
    options.transport.scheme = "http";
    options.transport.address = "127.0.0.1";
    options.transport.authentication = HttpAuthentication::none;
    client = co_await open_http_client_async(options, {});
    expect(!client->capabilities().write_operations, "HTTP must be read-only");
    const auto root = co_await client->load_directory_async(base, {});
    const auto repeated = co_await client->load_directory_async(base, {});
    expect(root.entries.size() == 2 && repeated.entries.size() == 2, "Opening and revisiting must reuse the initial page");
    expect(client->display_path(root.canonical_path) == "/pub/",
        "HTTP browser paths must omit the scheme and authority without hiding the base directory");
    for (const std::string path : {"/pub/tree/", "/pub/tree", "/pub/a%3Fb/?page=1", "/pub/a%3Fb/?page=2", "/pub/%82%A0.txt"}) {
      const auto identity = base + path.substr(5);
      expect(client->display_path(identity) == path &&
          co_await client->resolve_input_path_async(path, {}) == identity,
          "HTTP paths must preserve trailing slashes, encoded bytes and query variants");
    }
    for (const std::string input : {"/outside/", "https://other.invalid/pub/", "//other.invalid/pub/"}) {
      bool outside = false;
      try { (void)co_await client->resolve_input_path_async(input, {}); }
      catch (const std::invalid_argument &) { outside = true; }
      expect(outside, "Typed HTTP paths must remain inside the configured connection scope");
    }
    expect(root.entries.front().type == RemoteFileType::unresolved, "Listing must not resolve child resources");
    const auto info = co_await client->lstat_async(base + "tree/", {});
    expect(info && info->type == RemoteFileType::unresolved, "Attribute display must not fetch the child");
    bool rejected = false;
    try { co_await client->make_directory_async(base + "write", {}, {}); }
    catch (const std::exception &) { rejected = true; }
    expect(rejected, "HTTP mutation must be rejected by the client");
    FileTransferRequest request;
    request.direction = FileTransferDirection::receive;
    request.source_paths = {base + "tree/"};
    request.destination_directory = directory;
    bool incremental = false;
    request.callbacks.progress = [&incremental](const FileTransferProgress &progress) {
      incremental = incremental || (!progress.discovery_complete && !progress.total_bytes && progress.transferred_bytes > 0);
    };
    const auto result = co_await run_file_transfer_async(client, request, {});
    expect(result.outcome == FileTransferOutcome::completed && incremental, "Discovery and streaming must be incremental with unknown totals");
    expect(contents(std::filesystem::path(directory) / "tree" / "unknown.bin") == "unknown bytes", "Chunked download must match exactly");
    expect(contents(std::filesystem::path(directory) / "tree" / "nested" / "file.bin") == "nested bytes", "Selected subtree download must match");
    expect(contents(std::filesystem::path(directory) / "tree" / "%82%A0.txt") == "encoded bytes", "Non-UTF-8 URL must retain its identity");
    const auto classified = co_await client->load_directory_async(base, {});
    expect(classified.entries.front().type == RemoteFileType::directory,
        "Revisiting a list must retain already classified child resources without requests");
    std::size_t variants = 0;
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(directory) / "tree"))
      if (entry.path().filename().string().starts_with("same.bin")) ++variants;
    expect(variants == 2, "Query variants must have distinct safe local names");
    request.source_paths = {base + "document.html"};
    request.receive_as_file = true;
    const auto raw_job = create_file_transfer_job("http-test", 1, "HTTP", request, {});
    co_await run_file_transfer_job_async(raw_job, client, {}, {});
    expect(contents(std::filesystem::path(directory) / "document.html") == "<a href=\"untouched/\">original</a>",
        "Receiving HTML as a file must preserve its body without traversing links");
    request.receive_as_file = false;
    const auto unknown = co_await client->load_directory_async(base + "misconfigured", {});
    expect(unknown.resource_type == RemoteFileType::regular, "Missing type must remain content");
    const auto forced = co_await client->browse_directory_async(base + "misconfigured", true, true, {});
    expect(forced.resource_type == RemoteFileType::directory && forced.entries.size() == 1,
        "Explicit HTML expansion must override only the selected response");
    for (const auto &path : {"broken", "denied", "missing", "error", "escape", "loop"}) {
      request.source_paths = {base + path};
      bool failed = false, notified = false;
      request.callbacks.finished = [&notified](const FileTransferResult &value) { notified = value.outcome == FileTransferOutcome::failed; };
      try { (void)co_await run_file_transfer_async(client, request, {}); }
      catch (const std::exception &) { failed = true; }
      expect(failed && notified && !std::filesystem::exists(std::filesystem::path(directory) / path), "Failed download must clean up and report failure");
    }
    cardio::cancellation_source cancel;
    request.source_paths = {base + "slow"};
    request.callbacks.progress = [&cancel](const FileTransferProgress &progress) { if (progress.transferred_bytes) (void)cancel.cancel(); };
    bool cancelled = false;
    request.callbacks.finished = [&cancelled](const FileTransferResult &value) { cancelled = value.outcome == FileTransferOutcome::cancelled; };
    try { (void)co_await run_file_transfer_async(client, request, cancel.get_cancellation()); }
    catch (const cardio::canceled_exception &) {}
    expect(cancelled && !std::filesystem::exists(std::filesystem::path(directory) / "slow"), "Cancellation must remove incomplete files");
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory))
      expect(entry.path().string().find("elder-terms-part-") == std::string::npos, "No incomplete temporary files may remain");
    co_await stop_http_client_async(client);
    client = co_await open_http_client_async(options, {});
    expect((co_await client->load_directory_async(base, {})).entries.size() == 2, "Reconnect must load a fresh initial page");
    co_await stop_http_client_async(client);
    options.limits.depth = 2;
    client = co_await open_http_client_async(options, {});
    request.source_paths = {base + "infinite?n=0"};
    request.callbacks.progress = {};
    request.callbacks.finished = {};
    bool limited = false;
    try { (void)co_await run_file_transfer_async(client, request, {}); }
    catch (const std::exception &error) { limited = std::string(error.what()).find("limit exceeded") != std::string::npos; }
    expect(limited, "An infinite query graph must end as a failed transfer at the depth limit");
    for (const auto limit : {"pages", "links"}) {
      co_await stop_http_client_async(client);
      options.limits.depth = 32;
      options.limits.pages = std::string(limit) == "pages" ? 1 : 1000;
      options.limits.links = std::string(limit) == "links" ? 2 : 10000;
      client = co_await open_http_client_async(options, {});
      request.source_paths = {base + "infinite?n=100"};
      const auto destination = std::filesystem::path(directory) / limit;
      std::filesystem::create_directory(destination);
      request.destination_directory = destination;
      bool failed = false;
      try { (void)co_await run_file_transfer_async(client, request, {}); }
      catch (const std::exception &error) { failed = std::string(error.what()).find("limit exceeded") != std::string::npos; }
      expect(failed, "Cumulative page and link budgets must prevent an infinite query graph from succeeding");
    }
  } catch (...) { failure = std::current_exception(); }
  if (client) co_await stop_http_client_async(client);
  group.shutdown();
}
int main(int argc, char **argv) {
  try {
    expect(argc == 3, "Expected base URL and download directory");
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto pending = run_async(argv[1], argv[2], group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
    std::cout << "HTTP transfer client: PASS\n";
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
