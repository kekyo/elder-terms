#include "activity-file-client.h"

#include <utility>

namespace elder_terms {

struct ActivityFileReader final : RemoteFileReader {
  std::unique_ptr<RemoteFileReader> delegate;
  std::function<void(ActivityIndicatorId)> activity;

  cardio::promise<std::size_t>
  read_async(std::span<std::byte> buffer,
      cardio::cancellation cancellation) override {
    const auto size = co_await delegate->read_async(buffer, cancellation);
    if (size != 0) {
      activity(ActivityIndicatorId::rd);
    }
    co_return size;
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    co_await delegate->close_async(cancellation);
  }
};

struct ActivityFileWriter final : RemoteFileWriter {
  std::unique_ptr<RemoteFileWriter> delegate;
  std::function<void(ActivityIndicatorId)> activity;

  RemoteFileCreationState creation_state() const noexcept override {
    return delegate->creation_state();
  }

  cardio::promise<void>
  write_all_async(std::span<const std::byte> buffer,
      cardio::cancellation cancellation) override {
    co_await delegate->write_all_async(buffer, cancellation);
    if (!buffer.empty()) {
      activity(ActivityIndicatorId::sd);
    }
  }

  cardio::promise<void>
  close_async(cardio::cancellation cancellation) override {
    co_await delegate->close_async(cancellation);
  }
};

// All protocol work remains with the original client. The wrapper observes
// completed chunks and request/reply activity on the UI dispatcher only.
struct ActivityFileClient final : RemoteFileClient {
  std::shared_ptr<RemoteFileClient> delegate;
  std::function<void(ActivityIndicatorId)> activity;

  RemoteFileCapabilities capabilities() const noexcept override {
    return delegate->capabilities();
  }
  bool try_begin_transfer() override { return delegate->try_begin_transfer(); }
  void end_transfer() override { delegate->end_transfer(); }

  cardio::promise<RemoteDirectorySnapshot>
  load_directory_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    auto result = co_await delegate->load_directory_async(
        std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
    co_return result;
  }

  cardio::promise<std::optional<RemoteFileAttributes>>
  lstat_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    auto result = co_await delegate->lstat_async(std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
    co_return result;
  }

  cardio::promise<std::string>
  read_link_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    auto result = co_await delegate->read_link_async(std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
    co_return result;
  }

  cardio::promise<void>
  make_directory_async(std::string path,
                       std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->make_directory_async(
        std::move(path), permissions, cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  remove_file_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->remove_file_async(std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  remove_directory_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->remove_directory_async(std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  remove_directory_tree_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->remove_directory_tree_async(
        std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  rename_async(std::string source, std::string destination,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->rename_async(
        std::move(source), std::move(destination), cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  commit_upload_async(std::string source, std::string destination,
                      bool overwrite,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->commit_upload_async(
        std::move(source), std::move(destination), overwrite, cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  make_symbolic_link_async(std::string target, std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->make_symbolic_link_async(
        std::move(target), std::move(path), cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<void>
  set_attributes_async(std::string path,
                       RemoteFileAttributes attributes,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    co_await delegate->set_attributes_async(
        std::move(path), std::move(attributes), cancellation);
    activity(ActivityIndicatorId::rd);
  }

  cardio::promise<std::unique_ptr<RemoteFileReader>>
  open_read_async(std::string path,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    auto pending = delegate->open_read_async(std::move(path), cancellation);
    auto result = std::move(co_await pending);
    activity(ActivityIndicatorId::rd);
    auto observed = std::make_unique<ActivityFileReader>();
    observed->delegate = std::move(result);
    observed->activity = activity;
    co_return std::move(observed);
  }

  cardio::promise<std::unique_ptr<RemoteFileWriter>>
  open_write_async(std::string path, std::uint64_t size,
                   std::optional<std::uint32_t> permissions,
      cardio::cancellation cancellation) override {
    cancellation.throw_if_cancellation_requested();
    activity(ActivityIndicatorId::sd);
    auto pending = delegate->open_write_async(
        std::move(path), size, permissions, cancellation);
    auto result = std::move(co_await pending);
    activity(ActivityIndicatorId::rd);
    auto observed = std::make_unique<ActivityFileWriter>();
    observed->delegate = std::move(result);
    observed->activity = activity;
    co_return std::move(observed);
  }
};

std::shared_ptr<RemoteFileClient> create_activity_file_client(
    std::shared_ptr<RemoteFileClient> client,
    std::function<void(ActivityIndicatorId)> activity) {
  auto observed = std::make_shared<ActivityFileClient>();
  observed->delegate = std::move(client);
  observed->activity = std::move(activity);
  return observed;
}

} // namespace elder_terms
