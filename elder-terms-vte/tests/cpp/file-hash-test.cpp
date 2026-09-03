#include "../../src/file-transfer/file-hash.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#include <cardio.h>
#include <unistd.h>

struct TemporaryHashFile {
  std::filesystem::path path;

  ~TemporaryHashFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

static void expect_true(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void test_local_file_hashes() {
  TemporaryHashFile file{
      .path = std::filesystem::temp_directory_path() /
              ("elder-terms-file-hash-" +
               std::to_string(static_cast<long long>(::getpid())) +
               ".txt"),
  };
  {
    std::ofstream output(file.path, std::ios::binary);
    output << "hello from local\n";
    expect_true(output.good(), "failed to create the hash test file");
  }

  std::optional<elder_terms::FileHashes> hashes;
  std::exception_ptr async_error;
  cardio::dispatcher_group_glib dispatcher_group;
  cardio::dispatcher_host_glib dispatcher(dispatcher_group);
  cardio::cancellation_source cancellation_source;
  auto task = [&]() -> cardio::promise<void> {
    try {
      hashes = co_await elder_terms::calculate_local_file_hashes_async(
          file.path.string(), cancellation_source.get_cancellation());
    } catch (...) {
      async_error = std::current_exception();
    }
    dispatcher_group.shutdown();
  }();

  dispatcher.park();
  task.unsafe_result();
  if (async_error) {
    std::rethrow_exception(async_error);
  }
  expect_true(hashes.has_value(), "local hash operation returned no result");
  expect_true(hashes->md5 == "7670ba85103f6872fca913c4b8b7f34d",
              "local MD5 hash did not match");
  expect_true(hashes->sha1 ==
                  "62ad614351bea67cc944128bf51c398b51d172a2",
              "local SHA-1 hash did not match");
  expect_true(
      hashes->sha256 ==
          "8c669207eccffd4b0d14436f7ae3beaef38cc0606b2fe72afde93b1759567668",
      "local SHA-256 hash did not match");
}

int main() {
  try {
    test_local_file_hashes();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
