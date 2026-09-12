#include "webdav-client.h"
#include "../../src/file-transfer/file-transfer-engine.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void expect(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct LocalDirectory {
  std::filesystem::path path;
  ~LocalDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

static void write_pattern(const std::filesystem::path &path, std::uint64_t size) {
  std::ofstream output(path, std::ios::binary);
  expect(output.good(), "Could not create the independent server upload fixture");
  std::array<char, 65536> buffer{};
  for (std::uint64_t offset = 0; offset < size;) {
    const auto count = std::min<std::uint64_t>(buffer.size(), size - offset);
    for (std::size_t index = 0; index < count; ++index)
      buffer[index] = static_cast<char>(((offset + index) * 31 + 7) % 256);
    output.write(buffer.data(), count);
    offset += count;
  }
  output.close();
  expect(output.good(), "Could not finish the independent server upload fixture");
}

static void expect_same_file(const std::filesystem::path &source,
                             const std::filesystem::path &destination) {
  std::ifstream input(source, std::ios::binary), downloaded(destination, std::ios::binary);
  expect(input.good() && downloaded.good(), "Both transferred files must exist");
  std::array<char, 65536> expected{}, actual{};
  do {
    input.read(expected.data(), expected.size());
    downloaded.read(actual.data(), actual.size());
    expect(input.gcount() == downloaded.gcount() &&
           std::equal(expected.begin(), expected.begin() + input.gcount(), actual.begin()),
           "Independent server round trip must preserve every file byte");
  } while (input.gcount());
  expect(input.eof() && downloaded.eof(), "Both round-trip readers must reach EOF");
}

static cardio::promise<elder_terms::FileTransferConflictAction> overwrite_async(
    const elder_terms::FileTransferConflict &, cardio::cancellation) {
  co_return elder_terms::FileTransferConflictAction::overwrite;
}

static cardio::promise<void> transfer_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client,
    elder_terms::FileTransferDirection direction, std::string source,
    std::string destination, bool overwrite) {
  elder_terms::FileTransferRequest request;
  request.direction = direction;
  request.source_paths.push_back(std::move(source));
  request.destination_directory = std::move(destination);
  if (overwrite) request.callbacks.conflict = overwrite_async;
  auto transferring = elder_terms::run_file_transfer_async(client, std::move(request), {});
  co_await transferring;
}

static cardio::promise<void> exercise_async(
    std::shared_ptr<elder_terms::RemoteFileClient> client) {
  using namespace elder_terms;
  const auto initial = co_await client->load_directory_async("/", {});
  expect(initial.canonical_path == "/" && initial.entries.empty(),
         "Apache must expose an empty isolated virtual root");
  char pattern[] = "/tmp/elder-dav-apache-XXXXXX";
  const auto *created = ::mkdtemp(pattern);
  expect(created != nullptr, "Could not create the local fixture directory");
  const LocalDirectory directory{created};
  const auto source = directory.path / "source";
  std::filesystem::create_directories(source / "nested/empty");
  std::filesystem::create_directory(directory.path / "download");
  write_pattern(source / "large.bin", 40 * 1024 * 1024 + 13);
  write_pattern(source / "nested/資料 #+%.txt", 4099);
  write_pattern(source / "zero.txt", 0);
  co_await client->make_directory_async("/suite", std::nullopt, {});
  co_await transfer_async(client, FileTransferDirection::send, source.string(), "/suite", false);
  const auto nested = co_await client->load_directory_async("/suite/source/nested", {});
  expect(nested.entries.size() == 2, "Apache must retain the nested file and empty collection");
  const auto attributes = co_await client->lstat_async("/suite/source/large.bin", {});
  expect(attributes && attributes->size == 40 * 1024 * 1024 + 13,
         "Apache PROPFIND must return the uploaded file size");
  const auto missing = co_await client->lstat_async("/suite/missing", {});
  expect(!missing, "Apache 404 must be reported as absence");
  co_await transfer_async(client, FileTransferDirection::receive, "/suite/source",
                         (directory.path / "download").string(), false);
  for (const auto &relative : {"large.bin", "nested/資料 #+%.txt", "zero.txt"})
    expect_same_file(source / relative, directory.path / "download/source" / relative);
  expect(std::filesystem::is_empty(directory.path / "download/source/nested/empty"),
         "Round trip must preserve an empty collection");
  write_pattern(source / "nested/資料 #+%.txt", 17);
  co_await transfer_async(client, FileTransferDirection::send,
                         (source / "nested/資料 #+%.txt").string(), "/suite/source/nested", true);
  co_await transfer_async(client, FileTransferDirection::receive,
                         "/suite/source/nested/資料 #+%.txt", (directory.path / "download").string(), false);
  expect_same_file(source / "nested/資料 #+%.txt", directory.path / "download/資料 #+%.txt");
  bool collision = false;
  try { co_await client->rename_async("/suite/source/zero.txt", "/suite/source/large.bin", {}); }
  catch (const std::exception &) { collision = true; }
  expect(collision, "Ordinary Apache MOVE must refuse an existing destination");
  const auto preserved = co_await client->lstat_async("/suite/source/large.bin", {});
  expect(preserved && preserved->size == 40 * 1024 * 1024 + 13,
         "Refused MOVE must preserve the completed destination");
  co_await client->rename_async("/suite/source", "/suite/renamed 日本語 #", {});
  const auto renamed = co_await client->load_directory_async("/suite/renamed 日本語 #", {});
  expect(renamed.entries.size() == 3, "Apache MOVE must rename the whole collection");
  std::vector<std::string> paths{"/suite"};
  auto deleting = delete_file_transfer_items_async(client, FileTransferEndpoint::remote, std::move(paths), {});
  co_await deleting;
  expect((co_await client->load_directory_async("/", {})).entries.empty(),
         "Explicit recursive DELETE must remove the complete Apache collection");
}

static cardio::promise<bool> approve_certificate_async(
    std::shared_ptr<unsigned> confirmations,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  ++*confirmations;
  co_return true;
}

static cardio::promise<void> run_async(elder_terms::WebdavClientOpenOptions options,
    bool expected_failure, cardio::dispatcher_group_glib &group,
    std::exception_ptr &failure) {
  std::shared_ptr<elder_terms::RemoteFileClient> client;
  try {
    std::exception_ptr opening_failure;
    try { client = co_await elder_terms::open_webdav_client_async(std::move(options), {}); }
    catch (...) { opening_failure = std::current_exception(); }
    if (expected_failure) {
      expect(opening_failure != nullptr, "Untrusted Apache HTTPS connection must be rejected");
      try { std::rethrow_exception(opening_failure); }
      catch (const std::exception &error) { std::cout << error.what() << '\n'; }
    } else {
      if (opening_failure) std::rethrow_exception(opening_failure);
      co_await exercise_async(client);
    }
  } catch (...) { failure = std::current_exception(); }
  try { if (client) co_await elder_terms::stop_webdav_client_async(client); }
  catch (...) { if (!failure) failure = std::current_exception(); }
  group.shutdown();
}

int main(int argc, char **argv) {
  try {
    expect(argc == 8, "Expected scheme, port, authentication, password, CA file, expected result and certificate action");
    elder_terms::WebdavClientOpenOptions options;
    options.connection.scheme = argv[1];
    options.connection.address = "127.0.0.1";
    options.connection.port = std::stoll(argv[2]);
    options.connection.base_path = "/dav/";
    options.connection.username = "alice";
    const std::string authentication(argv[3]);
    options.connection.authentication = authentication == "none" ? elder_terms::WebdavAuthentication::none
        : authentication == "basic" ? elder_terms::WebdavAuthentication::basic
        : authentication == "digest" ? elder_terms::WebdavAuthentication::digest
        : elder_terms::WebdavAuthentication::automatic;
    options.password = argv[4];
    options.connection.ca_file = argv[5];
    const bool prompt = std::string(argv[7]) == "prompt";
    options.connection.prompt_certificate = prompt;
    const auto confirmations = std::make_shared<unsigned>(0);
    options.confirm_certificate = [confirmations](const auto &, cardio::cancellation cancellation) {
      return approve_certificate_async(confirmations, cancellation);
    };
    std::exception_ptr failure;
    {
      cardio::dispatcher_group_glib group;
      cardio::dispatcher_host_glib_auto dispatcher(group);
      auto task = run_async(std::move(options), std::string(argv[6]) == "failure", group, failure);
      dispatcher.park();
    }
    if (failure) std::rethrow_exception(failure);
    expect(*confirmations == (prompt ? 1U : 0U),
           "An independent Apache certificate exception must be confirmed once per logical session");
    std::cout << "webdav-apache-test: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
