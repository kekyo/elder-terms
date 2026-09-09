#include "file-transfer-window.h"
#include "../sftp/sftp-fixture-client.h"

#include <algorithm>
#include <array>
#include <cstdlib>
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

// Only directory cancellation is controlled here. Other operations retain the
// existing fixture's behavior rather than adding another filesystem mock.
class GatedClient final : public RemoteFileClient {
  std::shared_ptr<RemoteFileClient> delegate = create_sftp_fixture_client(false);
  cardio::primitives::manually_conditional pending{false};
public:
  cardio::primitives::manually_conditional started{false};
  cardio::primitives::manually_conditional cleanup_started{false};
  cardio::primitives::manually_conditional cleanup_allowed{false};
  bool cleanup_finished = false;

  RemoteFileCapabilities capabilities() const noexcept override { return delegate->capabilities(); }
  cardio::promise<RemoteDirectorySnapshot> load_directory_async(
      std::string path, cardio::cancellation cancellation) override {
    started.raise();
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
      std::string path, std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    return delegate->open_write_async(std::move(path), permissions, cancellation);
  }
  bool try_begin_transfer() override { return delegate->try_begin_transfer(); }
  void end_transfer() override { delegate->end_transfer(); }
};

static cardio::promise<void> verify_async(
    std::shared_ptr<FileTransferWindow> window, std::shared_ptr<GatedClient> client,
    cardio::dispatcher_group_glib &group, std::exception_ptr &failure) {
  std::optional<cardio::promise<void>> closing;
  try {
    show_file_transfer_window(window);
    attach_file_transfer_window_client(window, client);
    co_await client->started.wait();
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
  {
    cardio::dispatcher_group_glib group;
    cardio::dispatcher_host_glib dispatcher(group);
    auto client = std::make_shared<GatedClient>();
    auto window = create_file_transfer_window({
        .connection_name = "Window lifetime", .protocol_name = "FTP",
        .local_directory = path.data(), .remote_directory = "/remote",
        .remote_file_hash = {}, .colors = {}, .closed = {}});
    auto task = verify_async(window, client, group, failure);
    dispatcher.park();
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
