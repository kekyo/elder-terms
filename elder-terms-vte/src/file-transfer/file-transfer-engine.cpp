#include "file-transfer-engine.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gio/gio.h>
#include <glib.h>

namespace elder_terms {

static constexpr std::size_t file_transfer_chunk_size = 64 * 1024;
static constexpr char local_attribute_query[] =
    G_FILE_ATTRIBUTE_STANDARD_NAME ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
    G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_UNIX_MODE ","
    G_FILE_ATTRIBUTE_TIME_ACCESS ","
    G_FILE_ATTRIBUTE_TIME_MODIFIED;

struct GObjectDeleter {
  void operator()(void *object) const {
    if (object != nullptr) {
      g_object_unref(object);
    }
  }
};

struct GFreeDeleter {
  void operator()(void *value) const {
    g_free(value);
  }
};

template <typename T> using GObjectPtr = std::unique_ptr<T, GObjectDeleter>;
using GCharPtr = std::unique_ptr<char, GFreeDeleter>;

struct TransferNode {
  std::string source_path;
  std::string destination_path;
  RemoteFileAttributes attributes;
  std::string link_target;
  std::vector<TransferNode> children;
  std::uint64_t total_bytes = 0;
  std::uint64_t total_items = 1;
};

struct TransferRunState {
  std::shared_ptr<RemoteFileClient> client;
  FileTransferRequest request;
  std::optional<FileTransferConflictAction> conflict_action;
  FileTransferProgress progress;
};

struct DestinationResolution {
  bool proceed = true;
  bool overwrite = false;
};

class FileTransferAbort final : public std::runtime_error {
public:
  explicit FileTransferAbort(const std::string &message)
      : std::runtime_error(message) {
  }
};

class TransferSlotGuard {
private:
  std::shared_ptr<RemoteFileClient> client;

public:
  explicit TransferSlotGuard(std::shared_ptr<RemoteFileClient> client)
      : client(std::move(client)) {
  }

  ~TransferSlotGuard() {
    if (client != nullptr) {
      client->end_transfer();
    }
  }

  TransferSlotGuard(const TransferSlotGuard &) = delete;
  TransferSlotGuard &operator=(const TransferSlotGuard &) = delete;
};

static std::atomic<std::uint64_t> temporary_path_sequence = 0;

static std::string exception_message(std::exception_ptr error) {
  if (!error) {
    return "Unknown file transfer failure";
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &exception) {
    return exception.what();
  } catch (...) {
    return "Unknown file transfer failure";
  }
}

static bool is_not_found(const cardio::gio::gio_error &error) {
  return error.domain() == G_IO_ERROR &&
         error.code() == G_IO_ERROR_NOT_FOUND;
}

static cardio::promise<GFileInfo *>
query_local_info_async(GFile *file,
                       cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GFileInfo *>(
      [file](GCancellable *cancellable, GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_query_info_async(
            file, local_attribute_query,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
            cancellable, callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        return g_file_query_info_finish(G_FILE(object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<std::optional<RemoteFileAttributes>>
query_optional_local_attributes_async(
    GFile *file, cardio::cancellation cancellation) {
  try {
    GObjectPtr<GFileInfo> info(
        co_await query_local_info_async(file, cancellation));
    GCharPtr path(g_file_get_path(file));
    if (path == nullptr) {
      throw std::runtime_error(
          "The local pane only supports native filesystem paths");
    }

    const bool is_link =
        g_file_info_get_is_symlink(info.get()) != FALSE ||
        g_file_info_get_file_type(info.get()) ==
            G_FILE_TYPE_SYMBOLIC_LINK;
    RemoteFileType type = RemoteFileType::other;
    if (is_link) {
      type = RemoteFileType::symbolic_link;
    } else if (g_file_info_get_file_type(info.get()) ==
               G_FILE_TYPE_REGULAR) {
      type = RemoteFileType::regular;
    } else if (g_file_info_get_file_type(info.get()) ==
               G_FILE_TYPE_DIRECTORY) {
      type = RemoteFileType::directory;
    }

    const char *name = g_file_info_get_name(info.get());
    co_return RemoteFileAttributes{
        .name = name == nullptr ? std::string() : std::string(name),
        .path = path.get(),
        .type = type,
        .size =
            g_file_info_has_attribute(
                info.get(), G_FILE_ATTRIBUTE_STANDARD_SIZE)
                ? static_cast<std::uint64_t>(
                      g_file_info_get_size(info.get()))
                : 0,
        .permissions =
            g_file_info_has_attribute(
                info.get(), G_FILE_ATTRIBUTE_UNIX_MODE)
                ? g_file_info_get_attribute_uint32(
                      info.get(), G_FILE_ATTRIBUTE_UNIX_MODE)
                : std::optional<std::uint32_t>(),
        .access_time_unix_seconds =
            g_file_info_has_attribute(
                info.get(), G_FILE_ATTRIBUTE_TIME_ACCESS)
                ? static_cast<std::int64_t>(
                      g_file_info_get_attribute_uint64(
                          info.get(), G_FILE_ATTRIBUTE_TIME_ACCESS))
                : std::optional<std::int64_t>(),
        .modification_time_unix_seconds =
            g_file_info_has_attribute(
                info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED)
                ? static_cast<std::int64_t>(
                      g_file_info_get_attribute_uint64(
                          info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED))
                : std::optional<std::int64_t>(),
    };
  } catch (const cardio::gio::gio_error &error) {
    if (is_not_found(error)) {
      co_return std::nullopt;
    }
    throw;
  }
}

static cardio::promise<GFileEnumerator *>
enumerate_local_directory_async(
    GFile *directory, cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GFileEnumerator *>(
      [directory](GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer user_data) {
        g_file_enumerate_children_async(
            directory, G_FILE_ATTRIBUTE_STANDARD_NAME,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
            cancellable, callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        return g_file_enumerate_children_finish(
            G_FILE(object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<GList *>
next_local_files_async(GFileEnumerator *enumerator,
                       cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GList *>(
      [enumerator](GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data) {
        g_file_enumerator_next_files_async(
            enumerator, 64, G_PRIORITY_DEFAULT, cancellable, callback,
            user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        return g_file_enumerator_next_files_finish(
            G_FILE_ENUMERATOR(object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<void>
close_local_enumerator_async(
    GFileEnumerator *enumerator,
    cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [enumerator](GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data) {
        g_file_enumerator_close_async(
            enumerator, G_PRIORITY_DEFAULT, cancellable, callback,
            user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_file_enumerator_close_finish(
                G_FILE_ENUMERATOR(object), result, error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<std::vector<std::string>>
local_child_names_async(GFile *directory,
                        cardio::cancellation cancellation) {
  GObjectPtr<GFileEnumerator> enumerator(
      co_await enumerate_local_directory_async(directory, cancellation));
  std::vector<std::string> names;
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    GList *batch =
        co_await next_local_files_async(enumerator.get(), cancellation);
    if (batch == nullptr) {
      break;
    }
    for (GList *item = batch; item != nullptr; item = item->next) {
      auto *info = G_FILE_INFO(item->data);
      const char *name = g_file_info_get_name(info);
      if (name != nullptr && name[0] != '\0') {
        names.emplace_back(name);
      }
    }
    g_list_free_full(batch, g_object_unref);
  }
  co_await close_local_enumerator_async(enumerator.get(), cancellation);
  std::sort(names.begin(), names.end());
  co_return names;
}

static cardio::promise<GFileInputStream *>
open_local_read_async(GFile *file,
                      cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GFileInputStream *>(
      [file](GCancellable *cancellable, GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_read_async(file, G_PRIORITY_DEFAULT, cancellable, callback,
                          user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        return g_file_read_finish(G_FILE(object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<GFileOutputStream *>
open_local_replace_async(GFile *file,
                         cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GFileOutputStream *>(
      [file](GCancellable *cancellable, GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_replace_async(file, nullptr, FALSE, G_FILE_CREATE_PRIVATE,
                             G_PRIORITY_DEFAULT, cancellable, callback,
                             user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        return g_file_replace_finish(G_FILE(object), result, error);
      },
      std::move(cancellation));
}

static cardio::promise<std::size_t>
read_local_stream_async(GInputStream *stream,
                        std::span<std::byte> buffer,
                        cardio::cancellation cancellation) {
  co_return co_await cardio::gio::read(
      stream, buffer, std::move(cancellation), G_PRIORITY_DEFAULT);
}

static cardio::promise<void>
write_local_stream_async(GOutputStream *stream,
                         std::span<const std::byte> buffer,
                         cardio::cancellation cancellation) {
  const std::size_t written =
      co_await cardio::gio::submit<std::size_t>(
          [stream, buffer](GCancellable *cancellable,
                           GAsyncReadyCallback callback,
                           gpointer user_data) {
            g_output_stream_write_all_async(
                stream, buffer.data(), buffer.size(), G_PRIORITY_DEFAULT,
                cancellable, callback, user_data);
          },
          [](GObject *object, GAsyncResult *result, GError **error) {
            gsize bytes_written = 0;
            if (!g_output_stream_write_all_finish(
                    G_OUTPUT_STREAM(object), result, &bytes_written,
                    error)) {
              throw cardio::gio::gio_error(*error);
            }
            return static_cast<std::size_t>(bytes_written);
          },
          std::move(cancellation));
  if (written != buffer.size()) {
    throw std::runtime_error("Failed to write complete local file chunk");
  }
  co_return;
}

static cardio::promise<void>
close_local_input_async(GInputStream *stream,
                        cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [stream](GCancellable *cancellable, GAsyncReadyCallback callback,
               gpointer user_data) {
        g_input_stream_close_async(stream, G_PRIORITY_DEFAULT, cancellable,
                                   callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_input_stream_close_finish(G_INPUT_STREAM(object), result,
                                         error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<void>
close_local_output_async(GOutputStream *stream,
                         cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [stream](GCancellable *cancellable, GAsyncReadyCallback callback,
               gpointer user_data) {
        g_output_stream_close_async(stream, G_PRIORITY_DEFAULT, cancellable,
                                    callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_output_stream_close_finish(G_OUTPUT_STREAM(object), result,
                                          error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<void>
delete_local_async(GFile *file,
                   cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [file](GCancellable *cancellable, GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_delete_async(file, G_PRIORITY_DEFAULT, cancellable, callback,
                            user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_file_delete_finish(G_FILE(object), result, error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<void>
make_local_directory_async(GFile *file,
                           cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [file](GCancellable *cancellable, GAsyncReadyCallback callback,
             gpointer user_data) {
        g_file_make_directory_async(file, G_PRIORITY_DEFAULT, cancellable,
                                    callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_file_make_directory_finish(G_FILE(object), result, error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<void>
make_local_symbolic_link_async(
    GFile *file, const std::string &target,
    cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [file, &target](GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer user_data) {
        g_file_make_symbolic_link_async(
            file, target.c_str(), G_PRIORITY_DEFAULT, cancellable, callback,
            user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_file_make_symbolic_link_finish(G_FILE(object), result,
                                              error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<void>
move_local_async(GFile *source, GFile *destination,
                 cardio::cancellation cancellation) {
  co_await cardio::gio::submit<void>(
      [source, destination](GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data) {
        g_file_move_async(source, destination,
                          G_FILE_COPY_NOFOLLOW_SYMLINKS,
                          G_PRIORITY_DEFAULT, cancellable, nullptr, nullptr,
                          callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        if (!g_file_move_finish(G_FILE(object), result, error)) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static cardio::promise<void>
set_local_attributes_async(
    GFile *file, const RemoteFileAttributes &attributes,
    bool include_permissions, cardio::cancellation cancellation) {
  GObjectPtr<GFileInfo> info(g_file_info_new());
  bool has_attributes = false;
  if (include_permissions && attributes.permissions.has_value()) {
    g_file_info_set_attribute_uint32(
        info.get(), G_FILE_ATTRIBUTE_UNIX_MODE, *attributes.permissions);
    has_attributes = true;
  }
  if (attributes.access_time_unix_seconds.has_value()) {
    g_file_info_set_attribute_uint64(
        info.get(), G_FILE_ATTRIBUTE_TIME_ACCESS,
        static_cast<std::uint64_t>(std::max<std::int64_t>(
            0, *attributes.access_time_unix_seconds)));
    has_attributes = true;
  }
  if (attributes.modification_time_unix_seconds.has_value()) {
    g_file_info_set_attribute_uint64(
        info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED,
        static_cast<std::uint64_t>(std::max<std::int64_t>(
            0, *attributes.modification_time_unix_seconds)));
    has_attributes = true;
  }
  if (!has_attributes) {
    co_return;
  }

  co_await cardio::gio::submit<void>(
      [file, info = info.get()](GCancellable *cancellable,
                                GAsyncReadyCallback callback,
                                gpointer user_data) {
        g_file_set_attributes_async(
            file, info, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
            G_PRIORITY_DEFAULT, cancellable, callback, user_data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        GFileInfo *result_info = nullptr;
        const gboolean succeeded = g_file_set_attributes_finish(
            G_FILE(object), result, &result_info, error);
        if (result_info != nullptr) {
          g_object_unref(result_info);
        }
        if (!succeeded) {
          throw cardio::gio::gio_error(*error);
        }
      },
      std::move(cancellation));
}

static RemoteFileAttributes supported_remote_attributes(
    const RemoteFileClient &client,
    const RemoteFileAttributes &attributes) {
  RemoteFileAttributes supported = attributes;
  const RemoteFileCapabilities capabilities = client.capabilities();
  if (!capabilities.permissions) {
    supported.permissions.reset();
  }
  if (!capabilities.access_time) {
    supported.access_time_unix_seconds.reset();
  }
  if (!capabilities.modification_time) {
    supported.modification_time_unix_seconds.reset();
  }
  return supported;
}

static bool has_mutable_attributes(
    const RemoteFileAttributes &attributes) noexcept {
  return attributes.permissions.has_value() ||
         attributes.access_time_unix_seconds.has_value() ||
         attributes.modification_time_unix_seconds.has_value();
}

static bool valid_child_name(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos;
}

static std::string remote_basename(const std::string &path) {
  std::string trimmed = path;
  while (trimmed.size() > 1 && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  const std::size_t separator = trimmed.find_last_of('/');
  return separator == std::string::npos
             ? trimmed
             : trimmed.substr(separator + 1);
}

static std::string remote_child_path(const std::string &directory,
                                     const std::string &name) {
  if (!valid_child_name(name)) {
    throw std::runtime_error(
        "Remote service returned an invalid item name");
  }
  if (directory.empty() || directory == ".") {
    return directory.empty() ? name : "./" + name;
  }
  if (directory == "/") {
    return "/" + name;
  }
  return directory.back() == '/' ? directory + name
                                 : directory + "/" + name;
}

static std::string local_child_path(const std::string &directory,
                                    const std::string &name) {
  if (!valid_child_name(name)) {
    throw std::runtime_error(
        "Remote service returned an invalid item name");
  }
  return (std::filesystem::path(directory) / name).string();
}

static std::string local_link_target(GFileInfo *info) {
  const char *target = g_file_info_get_symlink_target(info);
  return target == nullptr ? std::string() : std::string(target);
}

static cardio::promise<TransferNode>
discover_local_node_async(GFile *file, std::string destination_path,
                          cardio::cancellation cancellation) {
  GObjectPtr<GFileInfo> info(
      co_await query_local_info_async(file, cancellation));
  const std::optional<RemoteFileAttributes> attributes =
      co_await query_optional_local_attributes_async(file, cancellation);
  if (!attributes.has_value()) {
    throw std::runtime_error("Local transfer source disappeared");
  }
  if (attributes->type == RemoteFileType::other) {
    throw std::runtime_error("Unsupported local transfer source type: " +
                             attributes->path);
  }

  TransferNode node{
      .source_path = attributes->path,
      .destination_path = std::move(destination_path),
      .attributes = *attributes,
      .link_target =
          attributes->type == RemoteFileType::symbolic_link
              ? local_link_target(info.get())
              : std::string(),
      .children = {},
      .total_bytes =
          attributes->type == RemoteFileType::regular ? attributes->size : 0,
      .total_items = 1,
  };
  if (node.attributes.type != RemoteFileType::directory) {
    co_return node;
  }

  const std::vector<std::string> names =
      co_await local_child_names_async(file, cancellation);
  for (const std::string &name : names) {
    cancellation.throw_if_cancellation_requested();
    GObjectPtr<GFile> child(g_file_get_child(file, name.c_str()));
    TransferNode child_node = co_await discover_local_node_async(
        child.get(), remote_child_path(node.destination_path, name),
        cancellation);
    node.total_bytes += child_node.total_bytes;
    node.total_items += child_node.total_items;
    node.children.push_back(std::move(child_node));
  }
  co_return node;
}

static cardio::promise<TransferNode>
discover_remote_node_async(
    const std::shared_ptr<RemoteFileClient> &client, std::string source_path,
    std::string destination_path, cardio::cancellation cancellation) {
  const std::optional<RemoteFileAttributes> attributes =
      co_await client->lstat_async(source_path, cancellation);
  if (!attributes.has_value()) {
    throw std::runtime_error("Remote transfer source does not exist: " +
                             source_path);
  }
  if (attributes->type == RemoteFileType::other) {
    throw std::runtime_error("Unsupported remote transfer source type: " +
                             source_path);
  }

  TransferNode node{
      .source_path = std::move(source_path),
      .destination_path = std::move(destination_path),
      .attributes = *attributes,
      .link_target = {},
      .children = {},
      .total_bytes =
          attributes->type == RemoteFileType::regular ? attributes->size : 0,
      .total_items = 1,
  };
  if (node.attributes.type == RemoteFileType::symbolic_link) {
    if (!client->capabilities().symbolic_links) {
      throw std::runtime_error(
          "Remote service reported an unsupported symbolic link");
    }
    node.link_target =
        co_await client->read_link_async(node.source_path, cancellation);
    co_return node;
  }
  if (node.attributes.type != RemoteFileType::directory) {
    co_return node;
  }

  std::vector<RemoteFileAttributes> children =
      (co_await client->load_directory_async(node.source_path, cancellation))
          .entries;
  std::sort(children.begin(), children.end(),
            [](const RemoteFileAttributes &left,
               const RemoteFileAttributes &right) {
              return left.name < right.name;
            });
  for (const RemoteFileAttributes &child : children) {
    if (!valid_child_name(child.name)) {
      throw std::runtime_error(
          "Remote service returned an invalid item name");
    }
    TransferNode child_node = co_await discover_remote_node_async(
        client, remote_child_path(node.source_path, child.name),
        local_child_path(node.destination_path, child.name),
        cancellation);
    node.total_bytes += child_node.total_bytes;
    node.total_items += child_node.total_items;
    node.children.push_back(std::move(child_node));
  }
  co_return node;
}

static void publish_progress(TransferRunState *state,
                             const std::string &current_path) {
  state->progress.current_path = current_path;
  if (state->request.callbacks.progress) {
    state->request.callbacks.progress(state->progress);
  }
}

static void complete_subtree(TransferRunState *state,
                             const TransferNode &node) {
  state->progress.transferred_bytes += node.total_bytes;
  state->progress.completed_items += node.total_items;
  publish_progress(state, node.source_path);
}

static cardio::promise<void>
remove_remote_tree_async(
    const std::shared_ptr<RemoteFileClient> &client, const std::string &path,
    cardio::cancellation cancellation) {
  const std::optional<RemoteFileAttributes> attributes =
      co_await client->lstat_async(path, cancellation);
  if (!attributes.has_value()) {
    co_return;
  }
  if (attributes->type != RemoteFileType::directory) {
    co_await client->remove_file_async(path, cancellation);
    co_return;
  }

  const std::vector<RemoteFileAttributes> children =
      (co_await client->load_directory_async(path, cancellation)).entries;
  for (const RemoteFileAttributes &child : children) {
    co_await remove_remote_tree_async(
        client, remote_child_path(path, child.name), cancellation);
  }
  co_await client->remove_directory_async(path, cancellation);
}

static cardio::promise<void>
remove_local_tree_async(GFile *file,
                        cardio::cancellation cancellation) {
  const std::optional<RemoteFileAttributes> attributes =
      co_await query_optional_local_attributes_async(file, cancellation);
  if (!attributes.has_value()) {
    co_return;
  }
  if (attributes->type == RemoteFileType::directory) {
    const std::vector<std::string> children =
        co_await local_child_names_async(file, cancellation);
    for (const std::string &name : children) {
      GObjectPtr<GFile> child(g_file_get_child(file, name.c_str()));
      co_await remove_local_tree_async(child.get(), cancellation);
    }
  }
  co_await delete_local_async(file, cancellation);
}

static cardio::promise<DestinationResolution>
resolve_conflict_async(
    TransferRunState *state, const TransferNode &node,
    std::optional<RemoteFileAttributes> destination,
    cardio::cancellation cancellation) {
  if (!destination.has_value()) {
    co_return DestinationResolution{};
  }
  if (node.attributes.type == RemoteFileType::directory &&
      destination->type == RemoteFileType::directory) {
    co_return DestinationResolution{};
  }

  if (!state->conflict_action.has_value()) {
    if (!state->request.callbacks.conflict) {
      throw FileTransferAbort(
          "Destination conflict requires a decision");
    }
    state->conflict_action =
        co_await state->request.callbacks.conflict(
            FileTransferConflict{
                .direction = state->request.direction,
                .source_path = node.source_path,
                .destination_path = node.destination_path,
                .source_type = node.attributes.type,
            },
            cancellation);
  }
  if (*state->conflict_action == FileTransferConflictAction::cancel) {
    throw FileTransferAbort("File transfer canceled at a conflict");
  }
  if (*state->conflict_action == FileTransferConflictAction::skip) {
    co_return DestinationResolution{
        .proceed = false,
        .overwrite = false,
    };
  }
  co_return DestinationResolution{
      .proceed = true,
      .overwrite = true,
  };
}

static std::string next_temporary_path(const std::string &destination) {
  const std::uint64_t sequence =
      temporary_path_sequence.fetch_add(1, std::memory_order_relaxed);
  return destination + ".elder-terms-part-" +
         std::to_string(static_cast<long long>(::getpid())) + "-" +
         std::to_string(sequence);
}

static cardio::promise<std::string>
available_remote_temporary_path_async(
    const std::shared_ptr<RemoteFileClient> &client,
    const std::string &destination,
    cardio::cancellation cancellation) {
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    const std::string candidate = next_temporary_path(destination);
    if (!(co_await client->lstat_async(candidate, cancellation))
             .has_value()) {
      co_return candidate;
    }
  }
  throw std::runtime_error(
      "Could not reserve a remote temporary path");
}

static cardio::promise<std::string>
available_local_temporary_path_async(
    const std::string &destination,
    cardio::cancellation cancellation) {
  for (std::size_t attempt = 0; attempt < 100; ++attempt) {
    const std::string candidate = next_temporary_path(destination);
    GObjectPtr<GFile> file(g_file_new_for_path(candidate.c_str()));
    if (!(co_await query_optional_local_attributes_async(
              file.get(), cancellation))
             .has_value()) {
      co_return candidate;
    }
  }
  throw std::runtime_error(
      "Could not reserve a local temporary path");
}

static cardio::promise<void>
cleanup_remote_temporary_file_async(
    const std::shared_ptr<RemoteFileClient> &client,
    const std::string &path) {
  try {
    co_await client->remove_file_async(path, {});
  } catch (...) {
  }
}

static cardio::promise<void>
cleanup_local_temporary_file_async(const std::string &path) {
  try {
    GObjectPtr<GFile> file(g_file_new_for_path(path.c_str()));
    co_await delete_local_async(file.get(), {});
  } catch (...) {
  }
}

static cardio::promise<void>
send_regular_file_async(TransferRunState *state,
                        const TransferNode &node, bool overwrite,
                        cardio::cancellation cancellation) {
  const std::string temporary_path =
      co_await available_remote_temporary_path_async(
          state->client, node.destination_path, cancellation);
  GObjectPtr<GFile> source(
      g_file_new_for_path(node.source_path.c_str()));
  GObjectPtr<GFileInputStream> input;
  std::unique_ptr<RemoteFileWriter> output;
  bool temporary_created = false;
  std::exception_ptr failure;

  try {
    input.reset(co_await open_local_read_async(source.get(), cancellation));
    const RemoteFileAttributes attributes = supported_remote_attributes(
        *state->client, node.attributes);
    output = std::move(co_await state->client->open_write_async(
        temporary_path, attributes.permissions, cancellation));
    temporary_created = true;
    std::array<std::byte, file_transfer_chunk_size> buffer{};
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const std::size_t size = co_await read_local_stream_async(
          G_INPUT_STREAM(input.get()),
          std::span<std::byte>(buffer.data(), buffer.size()),
          cancellation);
      if (size == 0) {
        break;
      }
      co_await output->write_all_async(
          std::span<const std::byte>(buffer.data(), size),
          cancellation);
      state->progress.transferred_bytes += size;
      publish_progress(state, node.source_path);
    }
    co_await close_local_input_async(G_INPUT_STREAM(input.get()),
                                     cancellation);
    input.reset();
    co_await output->close_async(cancellation);
    output.reset();
    if (has_mutable_attributes(attributes)) {
      co_await state->client->set_attributes_async(
          temporary_path, attributes, cancellation);
    }
    if (overwrite) {
      co_await remove_remote_tree_async(
          state->client, node.destination_path, cancellation);
    }
    co_await state->client->rename_async(
        temporary_path, node.destination_path, cancellation);
    temporary_created = false;
  } catch (...) {
    failure = std::current_exception();
  }

  if (failure) {
    if (input != nullptr) {
      try {
        co_await close_local_input_async(G_INPUT_STREAM(input.get()), {});
      } catch (...) {
      }
    }
    if (output != nullptr) {
      try {
        co_await output->close_async({});
      } catch (...) {
      }
    }
    if (temporary_created) {
      co_await cleanup_remote_temporary_file_async(
          state->client, temporary_path);
    }
    std::rethrow_exception(failure);
  }
}

static cardio::promise<void>
receive_regular_file_async(TransferRunState *state,
                           const TransferNode &node, bool overwrite,
                           cardio::cancellation cancellation) {
  const std::string temporary_path =
      co_await available_local_temporary_path_async(
          node.destination_path, cancellation);
  GObjectPtr<GFile> temporary(
      g_file_new_for_path(temporary_path.c_str()));
  GObjectPtr<GFile> destination(
      g_file_new_for_path(node.destination_path.c_str()));
  std::unique_ptr<RemoteFileReader> input;
  GObjectPtr<GFileOutputStream> output;
  bool temporary_created = false;
  std::exception_ptr failure;

  try {
    input = std::move(co_await state->client->open_read_async(
        node.source_path, cancellation));
    output.reset(
        co_await open_local_replace_async(temporary.get(), cancellation));
    temporary_created = true;
    std::array<std::byte, file_transfer_chunk_size> buffer{};
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const std::size_t size = co_await input->read_async(
          std::span<std::byte>(buffer.data(), buffer.size()),
          cancellation);
      if (size == 0) {
        break;
      }
      co_await write_local_stream_async(
          G_OUTPUT_STREAM(output.get()),
          std::span<const std::byte>(buffer.data(), size),
          cancellation);
      state->progress.transferred_bytes += size;
      publish_progress(state, node.source_path);
    }
    co_await input->close_async(cancellation);
    input.reset();
    co_await close_local_output_async(G_OUTPUT_STREAM(output.get()),
                                      cancellation);
    output.reset();
    if (overwrite) {
      co_await remove_local_tree_async(destination.get(), cancellation);
    }
    co_await move_local_async(temporary.get(), destination.get(),
                              cancellation);
    temporary_created = false;
    co_await set_local_attributes_async(
        destination.get(), node.attributes, true, cancellation);
  } catch (...) {
    failure = std::current_exception();
  }

  if (failure) {
    if (input != nullptr) {
      try {
        co_await input->close_async({});
      } catch (...) {
      }
    }
    if (output != nullptr) {
      try {
        co_await close_local_output_async(
            G_OUTPUT_STREAM(output.get()), {});
      } catch (...) {
      }
    }
    if (temporary_created) {
      co_await cleanup_local_temporary_file_async(temporary_path);
    }
    std::rethrow_exception(failure);
  }
}

static cardio::promise<void>
transfer_node_with_recovery_async(
    TransferRunState *state, const TransferNode &node,
    cardio::cancellation cancellation);

static cardio::promise<void>
send_node_once_async(TransferRunState *state, const TransferNode &node,
                     cardio::cancellation cancellation) {
  const std::optional<RemoteFileAttributes> destination =
      co_await state->client->lstat_async(node.destination_path,
                                         cancellation);
  const DestinationResolution resolution =
      co_await resolve_conflict_async(state, node, destination,
                                      cancellation);
  if (!resolution.proceed) {
    complete_subtree(state, node);
    co_return;
  }

  if (node.attributes.type == RemoteFileType::regular) {
    co_await send_regular_file_async(state, node, resolution.overwrite,
                                     cancellation);
    ++state->progress.completed_items;
    publish_progress(state, node.source_path);
    co_return;
  }

  if (node.attributes.type == RemoteFileType::symbolic_link) {
    if (!state->client->capabilities().symbolic_links) {
      throw std::runtime_error(
          "The remote service does not support symbolic links");
    }
    if (resolution.overwrite) {
      co_await remove_remote_tree_async(
          state->client, node.destination_path, cancellation);
    }
    co_await state->client->make_symbolic_link_async(
        node.link_target, node.destination_path, cancellation);
    ++state->progress.completed_items;
    publish_progress(state, node.source_path);
    co_return;
  }

  if (resolution.overwrite) {
    co_await remove_remote_tree_async(
        state->client, node.destination_path, cancellation);
  }
  if (!destination.has_value() || resolution.overwrite) {
    const RemoteFileAttributes attributes = supported_remote_attributes(
        *state->client, node.attributes);
    co_await state->client->make_directory_async(
        node.destination_path, attributes.permissions, cancellation);
  }
  for (const TransferNode &child : node.children) {
    co_await transfer_node_with_recovery_async(state, child,
                                                cancellation);
  }
  const RemoteFileAttributes attributes = supported_remote_attributes(
      *state->client, node.attributes);
  if (has_mutable_attributes(attributes)) {
    co_await state->client->set_attributes_async(
        node.destination_path, attributes, cancellation);
  }
  ++state->progress.completed_items;
  publish_progress(state, node.source_path);
}

static cardio::promise<void>
receive_node_once_async(TransferRunState *state,
                        const TransferNode &node,
                        cardio::cancellation cancellation) {
  GObjectPtr<GFile> destination(
      g_file_new_for_path(node.destination_path.c_str()));
  const std::optional<RemoteFileAttributes> destination_attributes =
      co_await query_optional_local_attributes_async(destination.get(),
                                                     cancellation);
  const DestinationResolution resolution =
      co_await resolve_conflict_async(
          state, node, destination_attributes, cancellation);
  if (!resolution.proceed) {
    complete_subtree(state, node);
    co_return;
  }

  if (node.attributes.type == RemoteFileType::regular) {
    co_await receive_regular_file_async(
        state, node, resolution.overwrite, cancellation);
    ++state->progress.completed_items;
    publish_progress(state, node.source_path);
    co_return;
  }

  if (node.attributes.type == RemoteFileType::symbolic_link) {
    if (resolution.overwrite) {
      co_await remove_local_tree_async(destination.get(), cancellation);
    }
    co_await make_local_symbolic_link_async(
        destination.get(), node.link_target, cancellation);
    // Linux does not provide a portable symlink permission operation. Times
    // are still applied without following the stored target.
    co_await set_local_attributes_async(
        destination.get(), node.attributes, false, cancellation);
    ++state->progress.completed_items;
    publish_progress(state, node.source_path);
    co_return;
  }

  if (resolution.overwrite) {
    co_await remove_local_tree_async(destination.get(), cancellation);
  }
  if (!destination_attributes.has_value() || resolution.overwrite) {
    co_await make_local_directory_async(destination.get(), cancellation);
  }
  for (const TransferNode &child : node.children) {
    co_await transfer_node_with_recovery_async(state, child,
                                                cancellation);
  }
  // Apply directory metadata after its children so their creation does not
  // replace the source directory's modification time.
  co_await set_local_attributes_async(
      destination.get(), node.attributes, true, cancellation);
  ++state->progress.completed_items;
  publish_progress(state, node.source_path);
}

static cardio::promise<void>
transfer_node_with_recovery_async(
    TransferRunState *state, const TransferNode &node,
    cardio::cancellation cancellation) {
  const std::uint64_t baseline_bytes =
      state->progress.transferred_bytes;
  const std::uint64_t baseline_items =
      state->progress.completed_items;
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    std::exception_ptr failure;
    try {
      if (state->request.direction == FileTransferDirection::send) {
        co_await send_node_once_async(state, node, cancellation);
      } else {
        co_await receive_node_once_async(state, node, cancellation);
      }
      co_return;
    } catch (const cardio::canceled_exception &) {
      throw;
    } catch (const FileTransferAbort &) {
      throw;
    } catch (...) {
      failure = std::current_exception();
    }

    state->progress.transferred_bytes = baseline_bytes;
    state->progress.completed_items = baseline_items;
    publish_progress(state, node.source_path);
    if (!state->request.callbacks.failure) {
      std::rethrow_exception(failure);
    }
    const FileTransferFailureAction action =
        co_await state->request.callbacks.failure(
            FileTransferFailure{
                .direction = state->request.direction,
                .source_path = node.source_path,
                .destination_path = node.destination_path,
                .message = exception_message(failure),
            },
            cancellation);
    if (action == FileTransferFailureAction::retry) {
      continue;
    }
    if (action == FileTransferFailureAction::skip) {
      complete_subtree(state, node);
      co_return;
    }
    throw FileTransferAbort(exception_message(failure));
  }
}

static cardio::promise<std::optional<TransferNode>>
discover_selected_source_async(
    TransferRunState *state, const std::string &source_path,
    cardio::cancellation cancellation) {
  for (;;) {
    std::exception_ptr failure;
    try {
      if (state->request.direction == FileTransferDirection::send) {
        GObjectPtr<GFile> source(
            g_file_new_for_path(source_path.c_str()));
        GCharPtr basename(g_file_get_basename(source.get()));
        if (basename == nullptr || basename.get()[0] == '\0') {
          throw std::runtime_error(
              "Selected local transfer source has no item name");
        }
        co_return co_await discover_local_node_async(
            source.get(),
            remote_child_path(state->request.destination_directory,
                              basename.get()),
            cancellation);
      }

      const std::string basename = remote_basename(source_path);
      if (!valid_child_name(basename)) {
        throw std::runtime_error(
            "Selected remote transfer source has no valid item name");
      }
      co_return co_await discover_remote_node_async(
          state->client, source_path,
          local_child_path(state->request.destination_directory,
                           basename),
          cancellation);
    } catch (const cardio::canceled_exception &) {
      throw;
    } catch (...) {
      failure = std::current_exception();
    }

    if (!state->request.callbacks.failure) {
      std::rethrow_exception(failure);
    }
    const FileTransferFailureAction action =
        co_await state->request.callbacks.failure(
            FileTransferFailure{
                .direction = state->request.direction,
                .source_path = source_path,
                .destination_path =
                    state->request.destination_directory,
                .message = exception_message(failure),
            },
            cancellation);
    if (action == FileTransferFailureAction::retry) {
      continue;
    }
    if (action == FileTransferFailureAction::skip) {
      co_return std::nullopt;
    }
    throw FileTransferAbort(exception_message(failure));
  }
}

cardio::promise<void>
run_file_transfer_async(std::shared_ptr<RemoteFileClient> client,
                        FileTransferRequest request,
                        cardio::cancellation cancellation) {
  if (client == nullptr) {
    throw std::invalid_argument("Remote file client is required");
  }
  if (request.source_paths.empty()) {
    throw std::invalid_argument(
        "At least one file transfer source path is required");
  }
  if (request.destination_directory.empty()) {
    throw std::invalid_argument(
        "A file transfer destination directory is required");
  }
  if (!client->try_begin_transfer()) {
    throw std::runtime_error(
        "A bulk file transfer is already in progress");
  }
  TransferSlotGuard slot(client);
  TransferRunState state{
      .client = std::move(client),
      .request = std::move(request),
      .conflict_action = std::nullopt,
      .progress = {},
  };

  std::vector<TransferNode> roots;
  for (const std::string &source_path : state.request.source_paths) {
    cancellation.throw_if_cancellation_requested();
    std::optional<TransferNode> node =
        co_await discover_selected_source_async(
            &state, source_path, cancellation);
    if (!node.has_value()) {
      continue;
    }
    state.progress.total_bytes += node->total_bytes;
    state.progress.total_items += node->total_items;
    roots.push_back(std::move(*node));
  }
  publish_progress(&state, {});

  for (const TransferNode &node : roots) {
    co_await transfer_node_with_recovery_async(
        &state, node, cancellation);
  }
}

} // namespace elder_terms
