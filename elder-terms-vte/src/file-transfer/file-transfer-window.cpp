#include "file-transfer-window.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gio/gio.h>
#include <gestament/gtk.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

#include "file-transfer-engine.h"
#include "../widget-background.h"

namespace elder_terms {

static std::string format_translated_string(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  gchar *formatted = g_strdup_vprintf(format, arguments);
  va_end(arguments);
  const std::string result = formatted == nullptr ? std::string() : formatted;
  g_free(formatted);
  return result;
}

static constexpr char local_browser_attributes[] =
    G_FILE_ATTRIBUTE_STANDARD_NAME ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_UNIX_MODE ","
    G_FILE_ATTRIBUTE_TIME_ACCESS ","
    G_FILE_ATTRIBUTE_TIME_MODIFIED;
static constexpr guint transfer_progress_pulse_period_ms = 100;
static constexpr const char *file_transfer_exterior_component_style_class =
    "file-transfer-exterior-components";
static constexpr int file_transfer_content_padding = 12;
static constexpr int file_transfer_pane_spacing = 12;
static constexpr int file_transfer_control_spacing = 8;
static constexpr int file_transfer_tree_expander_minimum_hit_width = 24;

enum FileTransferTreeColumn {
  file_transfer_tree_name_column = 0,
  file_transfer_tree_size_column = 1,
  file_transfer_tree_modified_column = 2,
  file_transfer_tree_path_column = 3,
  file_transfer_tree_type_column = 4,
  file_transfer_tree_loaded_column = 5,
  file_transfer_tree_dummy_column = 6,
  file_transfer_tree_column_count = 7,
};

struct FileTransferGObjectDeleter {
  void operator()(void *object) const {
    if (object != nullptr) {
      g_object_unref(object);
    }
  }
};

struct FileTransferGFreeDeleter {
  void operator()(void *value) const {
    g_free(value);
  }
};

template <typename T>
using FileTransferGObjectPtr =
    std::unique_ptr<T, FileTransferGObjectDeleter>;
using FileTransferGCharPtr = std::unique_ptr<char, FileTransferGFreeDeleter>;

struct FileTransferWindow;
static void clear_file_transfer_window_colors(FileTransferWindow *window);

enum class FileTransferConnectionState {
  connecting,
  authenticating,
  ready,
  failed,
  disconnected,
};

struct FileTransferPaneState {
  FileTransferWindow *window = nullptr;
  bool remote = false;
  GtkWidget *frame = nullptr;
  GtkWidget *path_entry = nullptr;
  GtkWidget *up_button = nullptr;
  GtkWidget *refresh_button = nullptr;
  GtkWidget *tree = nullptr;
  GtkTreeStore *store = nullptr;
  GtkWidget *menu = nullptr;
  GtkWidget *transfer_item = nullptr;
  GtkWidget *rename_item = nullptr;
  GtkWidget *delete_item = nullptr;
  std::string current_directory;
  std::optional<cardio::promise<void>> task;
  bool busy = false;
  std::uint64_t generation = 0;
};

struct FileTransferSelectedItem {
  std::string name;
  std::string path;
  RemoteFileType type = RemoteFileType::other;
};

struct FileTransferWindow {
  GtkWidget *window = nullptr;
  GtkWidget *header_bar = nullptr;
  GtkWidget *root_overlay = nullptr;
  GtkWidget *paned = nullptr;
  GtkWidget *dim_overlay = nullptr;
  GtkWidget *transfer_overlay = nullptr;
  GtkWidget *transfer_label = nullptr;
  GtkWidget *transfer_progress = nullptr;
  GtkWidget *transfer_cancel_button = nullptr;
  GtkWidget *prompt_panel = nullptr;
  GtkWidget *prompt_background = nullptr;
  GtkWidget *status_bar = nullptr;
  GtkWidget *status_label = nullptr;
  GtkCssProvider *exterior_background_provider = nullptr;
  GtkCssProvider *exterior_component_background_provider = nullptr;
  GtkCssProvider *background_provider = nullptr;
  GtkCssProvider *component_background_provider = nullptr;
  GtkCssProvider *popup_component_background_provider = nullptr;
  FileTransferPaneState local;
  FileTransferPaneState remote;
  std::shared_ptr<RemoteFileClient> client;
  std::shared_ptr<InlinePromptController> prompt;
  std::function<void()> closed;
  cardio::cancellation_source stop_source;
  std::optional<cardio::cancellation_source> transfer_cancel_source;
  std::optional<cardio::promise<void>> transfer_task;
  std::optional<cardio::promise<void>> browser_action_task;
  guint transfer_pulse_source = 0;
  FileTransferConnectionState connection_state =
      FileTransferConnectionState::connecting;
  bool shown = false;
  bool local_load_started = false;
  bool remote_load_started = false;
  bool connection_available = false;
  bool transfer_active = false;
  bool browser_action_active = false;
  bool browser_action_progress = false;
  bool destroyed = false;

  ~FileTransferWindow() {
    cancel_inline_prompt(prompt);
    (void)stop_source.cancel();
    if (transfer_cancel_source.has_value()) {
      (void)transfer_cancel_source->cancel();
    }
    if (transfer_pulse_source != 0) {
      g_source_remove(transfer_pulse_source);
      transfer_pulse_source = 0;
    }
    if (window != nullptr && !destroyed) {
      clear_file_transfer_window_colors(this);
      gtk_widget_destroy(window);
    }
    g_clear_object(&exterior_background_provider);
    g_clear_object(&exterior_component_background_provider);
    g_clear_object(&background_provider);
    g_clear_object(&component_background_provider);
    g_clear_object(&popup_component_background_provider);
    if (local.store != nullptr) {
      g_object_unref(local.store);
      local.store = nullptr;
    }
    if (remote.store != nullptr) {
      g_object_unref(remote.store);
      remote.store = nullptr;
    }
  }
};

static std::string exception_text(std::exception_ptr error) {
  if (!error) {
    return _("Unknown file transfer failure");
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &exception) {
    return exception.what();
  } catch (...) {
    return _("Unknown file transfer failure");
  }
}

static std::string file_size_text(const RemoteFileAttributes &attributes) {
  if (attributes.type != RemoteFileType::regular) {
    return {};
  }
  FileTransferGCharPtr formatted(
      g_format_size(static_cast<guint64>(attributes.size)));
  return formatted == nullptr ? std::string()
                              : std::string(formatted.get());
}

static std::string modification_time_text(
    const RemoteFileAttributes &attributes) {
  if (!attributes.modification_time_unix_seconds.has_value() ||
      *attributes.modification_time_unix_seconds <= 0) {
    return {};
  }
  const std::time_t seconds = static_cast<std::time_t>(
      *attributes.modification_time_unix_seconds);
  std::tm local_time = {};
  if (localtime_r(&seconds, &local_time) == nullptr) {
    return {};
  }
  char buffer[32] = {};
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M",
                    &local_time) == 0) {
    return {};
  }
  return buffer;
}

static int type_sort_rank(RemoteFileType type) {
  if (type == RemoteFileType::directory) {
    return 0;
  }
  if (type == RemoteFileType::regular) {
    return 1;
  }
  if (type == RemoteFileType::symbolic_link) {
    return 2;
  }
  return 3;
}

static void sort_browser_entries(
    std::vector<RemoteFileAttributes> *entries) {
  std::sort(
      entries->begin(), entries->end(),
      [](const RemoteFileAttributes &left,
         const RemoteFileAttributes &right) {
        const int left_rank = type_sort_rank(left.type);
        const int right_rank = type_sort_rank(right.type);
        if (left_rank != right_rank) {
          return left_rank < right_rank;
        }
        return left.name < right.name;
      });
}

static void append_dummy_row(GtkTreeStore *store,
                             GtkTreeIter *parent) {
  GtkTreeIter child;
  gtk_tree_store_append(store, &child, parent);
  gtk_tree_store_set(
      store, &child, file_transfer_tree_name_column, _("Loading…"),
      file_transfer_tree_size_column, "", file_transfer_tree_modified_column, "",
      file_transfer_tree_path_column, "", file_transfer_tree_type_column,
      static_cast<int>(RemoteFileType::other),
      file_transfer_tree_loaded_column, TRUE, file_transfer_tree_dummy_column, TRUE, -1);
}

static void set_browser_row(
    GtkTreeStore *store, GtkTreeIter *iterator,
    const RemoteFileAttributes &attributes) {
  const std::string size = file_size_text(attributes);
  const std::string modified = modification_time_text(attributes);
  gtk_tree_store_set(
      store, iterator, file_transfer_tree_name_column,
      attributes.name.c_str(), file_transfer_tree_size_column, size.c_str(),
      file_transfer_tree_modified_column, modified.c_str(),
      file_transfer_tree_path_column, attributes.path.c_str(),
      file_transfer_tree_type_column, static_cast<int>(attributes.type),
      file_transfer_tree_loaded_column,
      attributes.type != RemoteFileType::directory,
      file_transfer_tree_dummy_column, FALSE, -1);
  if (attributes.type == RemoteFileType::directory) {
    append_dummy_row(store, iterator);
  }
}

static void append_browser_row(
    GtkTreeStore *store, GtkTreeIter *parent,
    const RemoteFileAttributes &attributes) {
  GtkTreeIter iterator;
  gtk_tree_store_append(store, &iterator, parent);
  set_browser_row(store, &iterator, attributes);
}

static void append_browser_entries(
    GtkTreeStore *store, GtkTreeIter *parent,
    std::vector<RemoteFileAttributes> entries) {
  sort_browser_entries(&entries);
  for (const RemoteFileAttributes &entry : entries) {
    append_browser_row(store, parent, entry);
  }
}

static void replace_dummy_with_browser_entries(
    GtkTreeStore *store, GtkTreeIter *parent,
    std::vector<RemoteFileAttributes> entries) {
  sort_browser_entries(&entries);
  GtkTreeIter dummy;
  if (!gtk_tree_model_iter_children(
          GTK_TREE_MODEL(store), &dummy, parent)) {
    for (const RemoteFileAttributes &entry : entries) {
      append_browser_row(store, parent, entry);
    }
    return;
  }
  if (entries.empty()) {
    gtk_tree_store_remove(store, &dummy);
    return;
  }

  auto entry = entries.cbegin();
  set_browser_row(store, &dummy, *entry);
  ++entry;
  for (; entry != entries.cend(); ++entry) {
    append_browser_row(store, parent, *entry);
  }
}

static cardio::promise<GFileEnumerator *>
enumerate_local_browser_async(
    GFile *directory, cardio::cancellation cancellation) {
  co_return co_await cardio::gio::submit<GFileEnumerator *>(
      [directory](GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer user_data) {
        g_file_enumerate_children_async(
            directory, local_browser_attributes,
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
next_local_browser_files_async(
    GFileEnumerator *enumerator,
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
close_local_browser_enumerator_async(
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

static RemoteFileType local_browser_file_type(GFileInfo *info) {
  if (g_file_info_get_is_symlink(info) != FALSE ||
      g_file_info_get_file_type(info) == G_FILE_TYPE_SYMBOLIC_LINK) {
    return RemoteFileType::symbolic_link;
  }
  if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
    return RemoteFileType::directory;
  }
  if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) {
    return RemoteFileType::regular;
  }
  return RemoteFileType::other;
}

static RemoteFileAttributes local_browser_attributes_for(
    GFile *child, GFileInfo *info) {
  FileTransferGCharPtr path(g_file_get_path(child));
  if (path == nullptr) {
    throw std::runtime_error(
        _("The local pane only supports native filesystem paths"));
  }
  const char *name = g_file_info_get_name(info);
  return {
      .name = name == nullptr ? std::string() : std::string(name),
      .path = path.get(),
      .type = local_browser_file_type(info),
      .size =
          g_file_info_has_attribute(
              info, G_FILE_ATTRIBUTE_STANDARD_SIZE)
              ? static_cast<std::uint64_t>(
                    g_file_info_get_size(info))
              : 0,
      .permissions =
          g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)
              ? g_file_info_get_attribute_uint32(
                    info, G_FILE_ATTRIBUTE_UNIX_MODE)
              : std::optional<std::uint32_t>(),
      .access_time_unix_seconds =
          g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS)
              ? static_cast<std::int64_t>(
                    g_file_info_get_attribute_uint64(
                        info, G_FILE_ATTRIBUTE_TIME_ACCESS))
              : std::optional<std::int64_t>(),
      .modification_time_unix_seconds =
          g_file_info_has_attribute(
              info, G_FILE_ATTRIBUTE_TIME_MODIFIED)
              ? static_cast<std::int64_t>(
                    g_file_info_get_attribute_uint64(
                        info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
              : std::optional<std::int64_t>(),
  };
}

static cardio::promise<std::vector<RemoteFileAttributes>>
list_local_browser_directory_async(
    std::string path, cardio::cancellation cancellation) {
  FileTransferGObjectPtr<GFile> directory(
      g_file_new_for_path(path.c_str()));
  FileTransferGObjectPtr<GFileEnumerator> enumerator(
      co_await enumerate_local_browser_async(
          directory.get(), cancellation));
  std::vector<RemoteFileAttributes> entries;
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    GList *batch = co_await next_local_browser_files_async(
        enumerator.get(), cancellation);
    if (batch == nullptr) {
      break;
    }
    for (GList *item = batch; item != nullptr; item = item->next) {
      auto *info = G_FILE_INFO(item->data);
      const char *name = g_file_info_get_name(info);
      if (name == nullptr || name[0] == '\0') {
        continue;
      }
      FileTransferGObjectPtr<GFile> child(
          g_file_get_child(directory.get(), name));
      entries.push_back(
          local_browser_attributes_for(child.get(), info));
    }
    g_list_free_full(batch, g_object_unref);
  }
  co_await close_local_browser_enumerator_async(
      enumerator.get(), cancellation);
  co_return entries;
}

static std::string canonical_local_directory(
    const std::string &path) {
  FileTransferGCharPtr canonical(
      g_canonicalize_filename(path.c_str(), nullptr));
  if (canonical == nullptr || canonical.get()[0] == '\0') {
    throw std::runtime_error(_("Local directory path is empty"));
  }
  return canonical.get();
}

static std::string remote_parent_directory(
    const std::string &path) {
  std::string normalized = path;
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  if (normalized == "/") {
    return normalized;
  }
  const std::size_t separator = normalized.find_last_of('/');
  if (separator == std::string::npos) {
    return ".";
  }
  if (separator == 0) {
    return "/";
  }
  return normalized.substr(0, separator);
}

static std::string local_parent_directory(
    const std::string &path) {
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  return parent.empty() ? path : parent.string();
}

static void set_file_transfer_status(FileTransferWindow *window,
                            const std::string &text) {
  if (window == nullptr || window->destroyed ||
      window->status_label == nullptr) {
    return;
  }
  gtk_label_set_text(GTK_LABEL(window->status_label), text.c_str());
}

static void update_file_transfer_sensitivity(FileTransferWindow *window) {
  if (window == nullptr || window->destroyed) {
    return;
  }
  const bool idle =
      !window->transfer_active && !window->browser_action_active;
  gtk_widget_set_sensitive(window->local.frame, idle);
  gtk_widget_set_sensitive(
      window->remote.frame,
      idle && window->connection_available);
  gtk_widget_set_sensitive(
      window->local.transfer_item,
      idle && window->connection_available);
  gtk_widget_set_sensitive(
      window->remote.transfer_item,
      idle && window->connection_available);
  gtk_widget_set_sensitive(window->local.rename_item, idle);
  gtk_widget_set_sensitive(
      window->remote.rename_item,
      idle && window->connection_available);
  gtk_widget_set_sensitive(window->local.delete_item, idle);
  gtk_widget_set_sensitive(
      window->remote.delete_item,
      idle && window->connection_available);
}

static void set_widget_visible(GtkWidget *widget, bool visible) {
  gtk_widget_set_no_show_all(widget, !visible);
  gtk_widget_set_visible(widget, visible);
  if (visible) {
    gtk_widget_show_all(widget);
  }
}

static void update_file_transfer_overlay_presentation(
    FileTransferWindow *window) {
  if (window == nullptr || window->destroyed) {
    return;
  }
  const bool connection_blocked =
      window->connection_state == FileTransferConnectionState::connecting ||
      window->connection_state ==
          FileTransferConnectionState::authenticating ||
      window->connection_state == FileTransferConnectionState::failed;
  set_widget_visible(window->dim_overlay,
                     connection_blocked || window->transfer_active ||
                         window->browser_action_active);
  set_widget_visible(window->transfer_overlay,
                     window->transfer_active ||
                         window->browser_action_progress);
}

static bool has_file_transfer_progress_operation(
    const FileTransferWindow *window) {
  return window->transfer_active || window->browser_action_progress;
}

static gboolean pulse_file_transfer_progress(gpointer data) {
  auto *window = static_cast<FileTransferWindow *>(data);
  if (window == nullptr || window->destroyed ||
      !has_file_transfer_progress_operation(window) ||
      window->transfer_progress == nullptr) {
    if (window != nullptr) {
      window->transfer_pulse_source = 0;
    }
    return G_SOURCE_REMOVE;
  }
  gtk_progress_bar_pulse(
      GTK_PROGRESS_BAR(window->transfer_progress));
  return G_SOURCE_CONTINUE;
}

static void stop_file_transfer_pulse(FileTransferWindow *window) {
  if (window->transfer_pulse_source == 0) {
    return;
  }
  g_source_remove(window->transfer_pulse_source);
  window->transfer_pulse_source = 0;
}

static void set_file_transfer_active(FileTransferWindow *window,
                                     bool active) {
  window->transfer_active = active;
  update_file_transfer_overlay_presentation(window);
  set_widget_visible(window->transfer_cancel_button, active);
  gtk_widget_set_sensitive(window->transfer_cancel_button, active);
  if (active) {
    gtk_label_set_text(GTK_LABEL(window->transfer_label),
                       _("Preparing transfer…"));
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
    if (window->transfer_pulse_source == 0) {
      window->transfer_pulse_source = g_timeout_add(
          transfer_progress_pulse_period_ms,
          pulse_file_transfer_progress, window);
    }
  } else {
    if (!has_file_transfer_progress_operation(window)) {
      stop_file_transfer_pulse(window);
    }
  }
  update_file_transfer_sensitivity(window);
}

static void set_file_transfer_browser_action_phase(
    FileTransferWindow *window, bool active, bool show_progress,
    const std::string &progress_label) {
  window->browser_action_active = active;
  window->browser_action_progress = active && show_progress;
  set_widget_visible(window->transfer_cancel_button,
                     window->transfer_active);
  if (window->browser_action_progress) {
    gtk_label_set_text(GTK_LABEL(window->transfer_label),
                       progress_label.c_str());
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
    if (window->transfer_pulse_source == 0) {
      window->transfer_pulse_source = g_timeout_add(
          transfer_progress_pulse_period_ms,
          pulse_file_transfer_progress, window);
    }
  } else if (!has_file_transfer_progress_operation(window)) {
    stop_file_transfer_pulse(window);
  }
  update_file_transfer_overlay_presentation(window);
  update_file_transfer_sensitivity(window);
}

static void update_file_transfer_progress(
    FileTransferWindow *window, const FileTransferProgress &progress) {
  if (window == nullptr || window->destroyed ||
      !window->transfer_active) {
    return;
  }
  const bool has_items = progress.total_items > 0;
  const std::string file_name =
      progress.current_path.empty()
          ? std::string()
          : std::filesystem::path(progress.current_path).filename().string();
  std::string label;
  if (has_items && !file_name.empty()) {
    label = format_translated_string(
        _("Transferring %zu of %zu — %s"), progress.completed_items,
        progress.total_items, file_name.c_str());
  } else if (has_items) {
    label = format_translated_string(_("Transferring %zu of %zu"),
                                     progress.completed_items,
                                     progress.total_items);
  } else if (!file_name.empty()) {
    label = format_translated_string(_("Transferring — %s"),
                                     file_name.c_str());
  } else {
    label = _("Transferring");
  }
  gtk_label_set_text(GTK_LABEL(window->transfer_label),
                     label.c_str());

  if (progress.total_bytes > 0) {
    stop_file_transfer_pulse(window);
    const double fraction = std::min(
        1.0,
        static_cast<double>(progress.transferred_bytes) /
            static_cast<double>(progress.total_bytes));
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), fraction);
  } else if (progress.total_items > 0) {
    stop_file_transfer_pulse(window);
    const double fraction = std::min(
        1.0,
        static_cast<double>(progress.completed_items) /
            static_cast<double>(progress.total_items));
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), fraction);
  }
}

static cardio::promise<InlinePromptResponse>
prompt_file_transfer_choice_async(
    FileTransferWindow *window, const std::string &title,
    const std::string &detail, const std::string &cancel_label,
    const std::string &alternative_label,
    const std::string &accept_label,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  InlinePromptResponse response = co_await prompt_inline_async(
      window->prompt,
      {
          .title = title,
          .message = detail,
          .accept_label = accept_label,
          .cancel_label = cancel_label,
          .input_required = false,
          .echo = false,
          .cancel_visible = true,
          .alternative_label = alternative_label,
          .alternative_visible = true,
      },
      cancellation);
  cancellation.throw_if_cancellation_requested();
  co_return response;
}

static cardio::promise<FileTransferConflictAction>
prompt_file_transfer_conflict_async(
    FileTransferWindow *window, const FileTransferConflict &conflict,
    cardio::cancellation cancellation) {
  const InlinePromptResponse response =
      co_await prompt_file_transfer_choice_async(
          window, _("Destination already exists"),
          format_translated_string(
              _("%s\nThe selected decision applies to all remaining conflicts."),
              conflict.destination_path.c_str()),
          _("Cancel"), _("Skip"), _("Overwrite"), cancellation);
  if (response.accepted) {
    co_return FileTransferConflictAction::overwrite;
  }
  if (response.alternative) {
    co_return FileTransferConflictAction::skip;
  }
  co_return FileTransferConflictAction::cancel;
}

static cardio::promise<FileTransferFailureAction>
prompt_file_transfer_failure_async(
    FileTransferWindow *window, const FileTransferFailure &failure,
    cardio::cancellation cancellation) {
  const InlinePromptResponse response =
      co_await prompt_file_transfer_choice_async(
          window, _("File transfer failed"),
          format_translated_string(_("%s\n\n%s\n→ %s"),
                                   failure.message.c_str(),
                                   failure.source_path.c_str(),
                                   failure.destination_path.c_str()),
          _("Abort"), _("Skip"), _("Retry"), cancellation);
  if (response.accepted) {
    co_return FileTransferFailureAction::retry;
  }
  if (response.alternative) {
    co_return FileTransferFailureAction::skip;
  }
  co_return FileTransferFailureAction::abort;
}

static void on_notice_response(GtkDialog *dialog, gint, gpointer) {
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void show_file_transfer_error(FileTransferWindow *window,
                            const std::string &message) {
  if (window == nullptr || window->destroyed) {
    return;
  }
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(window->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
      _("File transfer operation failed"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
  gestament_gtk_assign_accessible_id(
      dialog, "file_transfer_operation_error_dialog");
  g_signal_connect(dialog, "response",
                   G_CALLBACK(on_notice_response), nullptr);
  gtk_widget_show_all(dialog);
}

static void clear_tree_children(GtkTreeStore *store,
                                GtkTreeIter *parent) {
  GtkTreeModel *model = GTK_TREE_MODEL(store);
  GtkTreeIter child;
  while (gtk_tree_model_iter_children(model, &child, parent)) {
    gtk_tree_store_remove(store, &child);
  }
}

static cardio::promise<RemoteDirectorySnapshot>
load_file_transfer_pane_directory_async(
    FileTransferPaneState *pane, std::string path,
    cardio::cancellation cancellation) {
  if (pane->remote) {
    co_return co_await pane->window->client->load_directory_async(
        std::move(path), std::move(cancellation));
  }
  std::string canonical_path = canonical_local_directory(path);
  std::vector<RemoteFileAttributes> entries =
      co_await list_local_browser_directory_async(
          canonical_path, std::move(cancellation));
  co_return RemoteDirectorySnapshot{
      .canonical_path = std::move(canonical_path),
      .entries = std::move(entries),
  };
}

static cardio::promise<void> load_file_transfer_pane_root_async(
    FileTransferPaneState *pane, std::string requested_path) {
  FileTransferWindow *window = pane->window;
  const std::uint64_t generation = ++pane->generation;
  const cardio::cancellation cancellation =
      window->stop_source.get_cancellation();
  try {
    RemoteDirectorySnapshot snapshot =
        co_await load_file_transfer_pane_directory_async(
            pane, std::move(requested_path), cancellation);
    if (window->destroyed || generation != pane->generation) {
      pane->busy = false;
      co_return;
    }
    gtk_tree_store_clear(pane->store);
    append_browser_entries(
        pane->store, nullptr, std::move(snapshot.entries));
    pane->current_directory = std::move(snapshot.canonical_path);
    gtk_entry_set_text(GTK_ENTRY(pane->path_entry),
                       pane->current_directory.c_str());
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
    if (!window->destroyed) {
      set_file_transfer_status(window, pane->remote
                                  ? _("Failed to load remote directory")
                                  : _("Failed to load local directory"));
      show_file_transfer_error(window, exception_text(std::current_exception()));
      gtk_entry_set_text(GTK_ENTRY(pane->path_entry),
                         pane->current_directory.c_str());
    }
  }
  pane->busy = false;
}

static void start_file_transfer_pane_navigation(
    FileTransferPaneState *pane, std::string path) {
  if (pane == nullptr || pane->window == nullptr ||
      pane->window->destroyed || pane->busy ||
      (pane->remote &&
       !pane->window->connection_available)) {
    return;
  }
  pane->task.reset();
  pane->busy = true;
  pane->task.emplace(
      load_file_transfer_pane_root_async(pane, std::move(path)));
}

static cardio::promise<void> expand_file_transfer_directory_async(
    FileTransferPaneState *pane, GtkTreeRowReference *reference,
    std::string path, std::uint64_t generation) {
  FileTransferWindow *window = pane->window;
  const cardio::cancellation cancellation =
      window->stop_source.get_cancellation();
  try {
    RemoteDirectorySnapshot snapshot =
        co_await load_file_transfer_pane_directory_async(
            pane, std::move(path), cancellation);
    if (window->destroyed || generation != pane->generation ||
        !gtk_tree_row_reference_valid(reference)) {
      gtk_tree_row_reference_free(reference);
      pane->busy = false;
      co_return;
    }
    GtkTreePath *tree_path =
        gtk_tree_row_reference_get_path(reference);
    GtkTreeIter iterator;
    if (tree_path != nullptr &&
        gtk_tree_model_get_iter(
            GTK_TREE_MODEL(pane->store), &iterator, tree_path)) {
      replace_dummy_with_browser_entries(
          pane->store, &iterator, std::move(snapshot.entries));
    }
    if (tree_path != nullptr) {
      gtk_tree_path_free(tree_path);
    }
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
    if (!window->destroyed &&
        gtk_tree_row_reference_valid(reference)) {
      GtkTreePath *tree_path =
          gtk_tree_row_reference_get_path(reference);
      GtkTreeIter iterator;
      if (tree_path != nullptr &&
          gtk_tree_model_get_iter(
              GTK_TREE_MODEL(pane->store), &iterator, tree_path)) {
        gtk_tree_store_set(
            pane->store, &iterator, file_transfer_tree_loaded_column,
            FALSE, -1);
        clear_tree_children(pane->store, &iterator);
        append_dummy_row(pane->store, &iterator);
        gtk_tree_view_collapse_row(
            GTK_TREE_VIEW(pane->tree), tree_path);
      }
      if (tree_path != nullptr) {
        gtk_tree_path_free(tree_path);
      }
      set_file_transfer_status(window, _("Failed to expand directory"));
      show_file_transfer_error(window, exception_text(std::current_exception()));
    }
  }
  gtk_tree_row_reference_free(reference);
  pane->busy = false;
}

static void on_file_transfer_row_expanded(
    GtkTreeView *, GtkTreeIter *iterator, GtkTreePath *tree_path,
    gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  if (pane == nullptr || pane->busy ||
      pane->window->destroyed ||
      (pane->remote &&
       !pane->window->connection_available)) {
    return;
  }
  gchar *path = nullptr;
  gboolean loaded = FALSE;
  gboolean dummy = FALSE;
  gint type = static_cast<gint>(RemoteFileType::other);
  gtk_tree_model_get(
      GTK_TREE_MODEL(pane->store), iterator,
      file_transfer_tree_path_column, &path, file_transfer_tree_type_column, &type,
      file_transfer_tree_loaded_column, &loaded, file_transfer_tree_dummy_column,
      &dummy, -1);
  const std::string directory =
      path == nullptr ? std::string() : std::string(path);
  g_free(path);
  if (loaded != FALSE || dummy != FALSE ||
      type != static_cast<gint>(RemoteFileType::directory) ||
      directory.empty()) {
    return;
  }

  gtk_tree_store_set(
      pane->store, iterator, file_transfer_tree_loaded_column, TRUE, -1);
  GtkTreeRowReference *reference =
      gtk_tree_row_reference_new(
          GTK_TREE_MODEL(pane->store), tree_path);
  if (reference == nullptr) {
    gtk_tree_store_set(
        pane->store, iterator, file_transfer_tree_loaded_column, FALSE, -1);
    return;
  }
  pane->task.reset();
  pane->busy = true;
  pane->task.emplace(expand_file_transfer_directory_async(
      pane, reference, directory, pane->generation));
}

static void on_file_transfer_path_activate(GtkEntry *entry, gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  const char *text = gtk_entry_get_text(entry);
  start_file_transfer_pane_navigation(
      pane, text == nullptr ? std::string() : std::string(text));
}

static void on_file_transfer_up_clicked(GtkButton *, gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  const std::string parent =
      pane->remote
          ? remote_parent_directory(pane->current_directory)
          : local_parent_directory(pane->current_directory);
  start_file_transfer_pane_navigation(pane, parent);
}

static void on_file_transfer_refresh_clicked(GtkButton *, gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  start_file_transfer_pane_navigation(pane, pane->current_directory);
}

static void on_file_transfer_row_activated(
    GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *,
    gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  GtkTreeIter iterator;
  if (!gtk_tree_model_get_iter(
          GTK_TREE_MODEL(pane->store), &iterator, path)) {
    return;
  }
  gchar *item_path = nullptr;
  gboolean dummy = FALSE;
  gint type = static_cast<gint>(RemoteFileType::other);
  gtk_tree_model_get(
      GTK_TREE_MODEL(pane->store), &iterator,
      file_transfer_tree_path_column, &item_path,
      file_transfer_tree_type_column, &type, file_transfer_tree_dummy_column,
      &dummy, -1);
  const std::string directory =
      item_path == nullptr ? std::string() : std::string(item_path);
  g_free(item_path);
  if (dummy == FALSE &&
      type == static_cast<gint>(RemoteFileType::directory) &&
      !directory.empty()) {
    start_file_transfer_pane_navigation(pane, directory);
  }
}

static std::vector<FileTransferSelectedItem>
selected_file_transfer_items(FileTransferPaneState *pane) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(pane->tree));
  GtkTreeModel *model = nullptr;
  GList *rows =
      gtk_tree_selection_get_selected_rows(selection, &model);
  std::vector<FileTransferSelectedItem> items;
  for (GList *item = rows; item != nullptr; item = item->next) {
    auto *tree_path = static_cast<GtkTreePath *>(item->data);
    GtkTreeIter iterator;
    if (!gtk_tree_model_get_iter(model, &iterator, tree_path)) {
      continue;
    }
    gchar *name = nullptr;
    gchar *path = nullptr;
    gboolean dummy = FALSE;
    gint type = static_cast<gint>(RemoteFileType::other);
    gtk_tree_model_get(
        model, &iterator, file_transfer_tree_name_column, &name,
        file_transfer_tree_path_column, &path,
        file_transfer_tree_type_column, &type,
        file_transfer_tree_dummy_column, &dummy, -1);
    if (dummy == FALSE && name != nullptr && name[0] != '\0' &&
        path != nullptr && path[0] != '\0') {
      items.push_back({
          .name = name,
          .path = path,
          .type = static_cast<RemoteFileType>(type),
      });
    }
    g_free(name);
    g_free(path);
  }
  g_list_free_full(rows,
                   reinterpret_cast<GDestroyNotify>(
                       gtk_tree_path_free));
  return items;
}

static std::vector<std::string>
selected_file_transfer_paths(FileTransferPaneState *pane) {
  const std::vector<FileTransferSelectedItem> items =
      selected_file_transfer_items(pane);
  std::vector<std::string> paths;
  paths.reserve(items.size());
  for (const FileTransferSelectedItem &item : items) {
    paths.push_back(item.path);
  }
  return paths;
}

static bool is_file_transfer_tree_expander_extension(
    GtkTreeView *tree, GtkTreePath *path,
    GtkTreeViewColumn *column, int x) {
  if (!gtk_tree_view_get_show_expanders(tree) ||
      column != gtk_tree_view_get_expander_column(tree)) {
    return false;
  }

  GtkTreeModel *model = gtk_tree_view_get_model(tree);
  GtkTreeIter iterator;
  if (model == nullptr ||
      !gtk_tree_model_get_iter(model, &iterator, path) ||
      !gtk_tree_model_iter_has_child(model, &iterator)) {
    return false;
  }

  int expander_size = 0;
  int horizontal_separator = 0;
  gboolean indent_expanders = TRUE;
  gtk_widget_style_get(
      GTK_WIDGET(tree), "expander-size", &expander_size,
      "horizontal-separator", &horizontal_separator,
      "indent-expanders", &indent_expanders, nullptr);
  if (expander_size <= 0) {
    return false;
  }

  // GTK exposes the column background but not the native expander
  // rectangle. Rebuild its horizontal range from the same style values
  // and extend only the space outside it, preserving GTK's own handling
  // inside the chevron for both LTR and RTL layouts.
  GdkRectangle background_area = {};
  gtk_tree_view_get_background_area(
      tree, path, column, &background_area);
  const int target_width = std::min(
      background_area.width,
      std::max(expander_size,
               file_transfer_tree_expander_minimum_hit_width));
  if (target_width <= expander_size) {
    return false;
  }

  const int depth = gtk_tree_path_get_depth(path);
  const int expander_stride =
      expander_size + std::max(0, horizontal_separator) / 2;
  const int depth_offset =
      indent_expanders != FALSE
          ? std::max(0, depth - 1) * expander_stride
          : 0;
  const bool right_to_left =
      gtk_widget_get_direction(GTK_WIDGET(tree)) ==
      GTK_TEXT_DIR_RTL;
  const int expander_x =
      right_to_left
          ? background_area.x + background_area.width -
                expander_size - depth_offset
          : background_area.x +
                std::max(0, horizontal_separator) / 2 +
                depth_offset;

  int target_x =
      expander_x - (target_width - expander_size) / 2;
  target_x = std::clamp(
      target_x, background_area.x,
      background_area.x + background_area.width - target_width);
  const bool inside_target =
      x >= target_x && x < target_x + target_width;
  const bool inside_native_expander =
      x >= expander_x && x < expander_x + expander_size;
  return inside_target && !inside_native_expander;
}

static gboolean toggle_file_transfer_tree_expander_extension(
    GtkWidget *widget, GdkEventButton *event) {
  auto *tree = GTK_TREE_VIEW(widget);
  if (event->button != GDK_BUTTON_PRIMARY ||
      event->window != gtk_tree_view_get_bin_window(tree)) {
    return FALSE;
  }

  GtkTreePath *path = nullptr;
  GtkTreeViewColumn *column = nullptr;
  if (!gtk_tree_view_get_path_at_pos(
          tree, static_cast<gint>(event->x),
          static_cast<gint>(event->y), &path, &column, nullptr,
          nullptr)) {
    return FALSE;
  }
  const bool inside_extension =
      is_file_transfer_tree_expander_extension(
          tree, path, column, static_cast<int>(event->x));
  if (inside_extension) {
    if (gtk_tree_view_row_expanded(tree, path)) {
      gtk_tree_view_collapse_row(tree, path);
    } else {
      gtk_tree_view_expand_row(tree, path, FALSE);
    }
    gtk_widget_grab_focus(widget);
  }
  gtk_tree_path_free(path);
  return inside_extension ? TRUE : FALSE;
}

static gboolean on_file_transfer_tree_button_press(
    GtkWidget *widget, GdkEventButton *event, gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  if (event->type != GDK_BUTTON_PRESS || pane == nullptr) {
    return FALSE;
  }
  if (toggle_file_transfer_tree_expander_extension(widget, event)) {
    return TRUE;
  }
  if (event->button != GDK_BUTTON_SECONDARY ||
      pane->window->transfer_active ||
      pane->window->browser_action_active ||
      (pane->remote &&
       !pane->window->connection_available)) {
    return FALSE;
  }
  GtkTreePath *path = nullptr;
  if (!gtk_tree_view_get_path_at_pos(
          GTK_TREE_VIEW(widget), static_cast<gint>(event->x),
          static_cast<gint>(event->y), &path, nullptr, nullptr,
          nullptr)) {
    return FALSE;
  }
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(widget));
  if (!gtk_tree_selection_path_is_selected(selection, path)) {
    gtk_tree_selection_unselect_all(selection);
    gtk_tree_selection_select_path(selection, path);
  }
  gtk_tree_path_free(path);
  const std::vector<FileTransferSelectedItem> items =
      selected_file_transfer_items(pane);
  if (items.empty()) {
    return FALSE;
  }
  gtk_widget_set_sensitive(pane->rename_item,
                           items.size() == 1 ? TRUE : FALSE);
  gtk_widget_set_sensitive(pane->delete_item, TRUE);
  gtk_menu_popup_at_pointer(GTK_MENU(pane->menu),
                            reinterpret_cast<GdkEvent *>(event));
  return TRUE;
}

static bool valid_file_transfer_item_name(const std::string &name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos &&
         name.find('\r') == std::string::npos &&
         name.find('\n') == std::string::npos;
}

static std::string file_transfer_renamed_item_path(
    const FileTransferPaneState *pane,
    const FileTransferSelectedItem &item, const std::string &new_name) {
  if (!pane->remote) {
    return (std::filesystem::path(item.path).parent_path() / new_name)
        .string();
  }
  const std::string parent = remote_parent_directory(item.path);
  if (parent.empty()) {
    return new_name;
  }
  if (parent == "/") {
    return "/" + new_name;
  }
  return parent == "." ? "./" + new_name
                       : parent + "/" + new_name;
}

static cardio::promise<std::optional<std::string>>
prompt_file_transfer_rename_name_async(
    FileTransferWindow *window, const FileTransferSelectedItem &item,
    cardio::cancellation cancellation) {
  const std::string base_message = format_translated_string(
      _("Enter a new name for \"%s\"."), item.name.c_str());
  std::string message = base_message;
  std::string initial_text = item.name;
  for (;;) {
    cancellation.throw_if_cancellation_requested();
    const InlinePromptResponse response = co_await prompt_inline_async(
        window->prompt,
        {
            .title = _("Rename item"),
            .message = message,
            .accept_label = _("Rename"),
            .cancel_label = _("Cancel"),
            .initial_text = initial_text,
            .input_required = true,
            .echo = true,
            .cancel_visible = true,
        },
        cancellation);
    cancellation.throw_if_cancellation_requested();
    if (!response.accepted) {
      co_return std::nullopt;
    }
    if (valid_file_transfer_item_name(response.text)) {
      co_return response.text;
    }
    initial_text = response.text;
    message = base_message + "\n\n" +
              _("Names must not be empty, '.', or '..', and must not "
                "contain '/' or line breaks.");
  }
}

static cardio::promise<void>
show_file_transfer_browser_action_error_async(
    FileTransferWindow *window, std::string title, std::string message,
    cardio::cancellation cancellation) {
  (void)co_await prompt_inline_async(
      window->prompt,
      {
          .title = std::move(title),
          .message = std::move(message),
          .accept_label = _("Close"),
          .cancel_label = _("Cancel"),
          .input_required = false,
          .echo = false,
          .cancel_visible = false,
      },
      std::move(cancellation));
}

static cardio::promise<void> run_file_transfer_rename_async(
    FileTransferPaneState *pane, FileTransferSelectedItem item) {
  FileTransferWindow *window = pane->window;
  const cardio::cancellation cancellation =
      window->stop_source.get_cancellation();
  bool cancelled = false;
  std::exception_ptr failure;
  try {
    set_file_transfer_browser_action_phase(window, true, false, {});
    const std::optional<std::string> new_name =
        co_await prompt_file_transfer_rename_name_async(
            window, item, cancellation);
    if (window->destroyed) {
      co_return;
    }
    if (!new_name.has_value() || *new_name == item.name) {
      set_file_transfer_browser_action_phase(window, false, false, {});
      set_file_transfer_status(window, _("Ready"));
      co_return;
    }

    const std::string destination = file_transfer_renamed_item_path(
        pane, item, *new_name);
    set_file_transfer_browser_action_phase(
        window, true, true, _("Renaming…"));
    co_await rename_file_transfer_item_async(
        window->client,
        pane->remote ? FileTransferEndpoint::remote
                     : FileTransferEndpoint::local,
        item.path, destination, cancellation);
    if (window->destroyed) {
      co_return;
    }
    set_file_transfer_browser_action_phase(window, false, false, {});
    set_file_transfer_status(
        window,
        format_translated_string(_("Renamed \"%s\" to \"%s\""),
                                 item.name.c_str(), new_name->c_str()));
    start_file_transfer_pane_navigation(pane, pane->current_directory);
  } catch (const cardio::canceled_exception &) {
    cancelled = true;
  } catch (...) {
    failure = std::current_exception();
  }

  if (window->destroyed) {
    co_return;
  }
  if (cancelled) {
    set_file_transfer_browser_action_phase(window, false, false, {});
    set_file_transfer_status(window, _("Rename cancelled"));
  } else if (failure != nullptr) {
    const std::string message = exception_text(failure);
    set_file_transfer_browser_action_phase(window, true, false, {});
    set_file_transfer_status(window, _("Rename failed"));
    co_await show_file_transfer_browser_action_error_async(
        window, _("Failed to rename item"), message, cancellation);
    if (!window->destroyed) {
      set_file_transfer_browser_action_phase(window, false, false, {});
      start_file_transfer_pane_navigation(pane, pane->current_directory);
    }
  }
}

static void start_file_transfer_rename(FileTransferPaneState *pane) {
  if (pane == nullptr || pane->window == nullptr ||
      pane->window->destroyed || pane->window->transfer_active ||
      pane->window->browser_action_active ||
      (pane->remote && !pane->window->connection_available)) {
    return;
  }
  const std::vector<FileTransferSelectedItem> items =
      selected_file_transfer_items(pane);
  if (items.size() != 1) {
    return;
  }
  pane->window->browser_action_task.reset();
  pane->window->browser_action_task.emplace(
      run_file_transfer_rename_async(pane, items.front()));
}

static void on_file_transfer_rename_item_activate(
    GtkMenuItem *, gpointer data) {
  start_file_transfer_rename(
      static_cast<FileTransferPaneState *>(data));
}

static std::vector<FileTransferSelectedItem>
normalize_file_transfer_delete_items(
    const FileTransferPaneState *pane,
    const std::vector<FileTransferSelectedItem> &items) {
  std::vector<std::string> paths;
  paths.reserve(items.size());
  for (const FileTransferSelectedItem &item : items) {
    paths.push_back(item.path);
  }
  const std::vector<std::string> normalized_paths =
      normalize_file_transfer_delete_paths(
          pane->remote ? FileTransferEndpoint::remote
                       : FileTransferEndpoint::local,
          std::move(paths));
  std::vector<FileTransferSelectedItem> result;
  result.reserve(normalized_paths.size());
  for (const std::string &path : normalized_paths) {
    const auto item = std::find_if(
        items.begin(), items.end(), [&path](const auto &candidate) {
          return candidate.path == path;
        });
    if (item == items.end()) {
      throw std::logic_error(
          "Normalized deletion path has no selected item");
    }
    result.push_back(*item);
  }
  return result;
}

static std::string file_transfer_delete_confirmation_message(
    const std::vector<FileTransferSelectedItem> &items) {
  std::string item_list;
  for (const FileTransferSelectedItem &item : items) {
    if (!item_list.empty()) {
      item_list += '\n';
    }
    item_list += "• " + item.name;
  }
  return std::string(
             _("The following items will be permanently deleted:")) +
         "\n\n" + item_list + "\n\n" +
         _("Directories and their contents will be deleted. This cannot be "
           "undone.");
}

static cardio::promise<bool> prompt_file_transfer_delete_async(
    FileTransferWindow *window,
    const std::vector<FileTransferSelectedItem> &items,
    cardio::cancellation cancellation) {
  const char *title = g_dngettext(
      GETTEXT_PACKAGE, "Delete selected item?", "Delete selected items?",
      items.size());
  const InlinePromptResponse response = co_await prompt_inline_async(
      window->prompt,
      {
          .title = title,
          .message = file_transfer_delete_confirmation_message(items),
          .accept_label = _("Delete"),
          .cancel_label = _("Cancel"),
          .input_required = false,
          .echo = false,
          .cancel_visible = true,
      },
      cancellation);
  cancellation.throw_if_cancellation_requested();
  co_return response.accepted;
}

static cardio::promise<void> run_file_transfer_delete_async(
    FileTransferPaneState *pane,
    std::vector<FileTransferSelectedItem> selected_items) {
  FileTransferWindow *window = pane->window;
  const cardio::cancellation cancellation =
      window->stop_source.get_cancellation();
  bool cancelled = false;
  std::exception_ptr failure;
  try {
    set_file_transfer_browser_action_phase(window, true, false, {});
    std::vector<FileTransferSelectedItem> items =
        normalize_file_transfer_delete_items(pane, selected_items);
    if (items.empty()) {
      throw std::invalid_argument(
          "At least one file deletion item is required");
    }
    if (!(co_await prompt_file_transfer_delete_async(
            window, items, cancellation))) {
      if (!window->destroyed) {
        set_file_transfer_browser_action_phase(window, false, false, {});
        set_file_transfer_status(window, _("Ready"));
      }
      co_return;
    }
    if (window->destroyed) {
      co_return;
    }

    std::vector<std::string> paths;
    paths.reserve(items.size());
    for (const FileTransferSelectedItem &item : items) {
      paths.push_back(item.path);
    }
    set_file_transfer_browser_action_phase(
        window, true, true, _("Deleting…"));
    co_await delete_file_transfer_items_async(
        window->client,
        pane->remote ? FileTransferEndpoint::remote
                     : FileTransferEndpoint::local,
        std::move(paths), cancellation);
    if (window->destroyed) {
      co_return;
    }
    set_file_transfer_browser_action_phase(window, false, false, {});
    const char *format = g_dngettext(
        GETTEXT_PACKAGE, "Deleted %zu item", "Deleted %zu items",
        items.size());
    set_file_transfer_status(
        window, format_translated_string(format, items.size()));
    start_file_transfer_pane_navigation(pane, pane->current_directory);
  } catch (const cardio::canceled_exception &) {
    cancelled = true;
  } catch (...) {
    failure = std::current_exception();
  }

  if (window->destroyed) {
    co_return;
  }
  if (cancelled) {
    set_file_transfer_browser_action_phase(window, false, false, {});
    set_file_transfer_status(window, _("Delete cancelled"));
  } else if (failure != nullptr) {
    const std::string message = exception_text(failure);
    set_file_transfer_browser_action_phase(window, true, false, {});
    set_file_transfer_status(window, _("Delete failed"));
    co_await show_file_transfer_browser_action_error_async(
        window, _("Failed to delete selected items"), message,
        cancellation);
    if (!window->destroyed) {
      set_file_transfer_browser_action_phase(window, false, false, {});
      start_file_transfer_pane_navigation(pane, pane->current_directory);
    }
  }
}

static void start_file_transfer_delete(FileTransferPaneState *pane) {
  if (pane == nullptr || pane->window == nullptr ||
      pane->window->destroyed || pane->window->transfer_active ||
      pane->window->browser_action_active ||
      (pane->remote && !pane->window->connection_available)) {
    return;
  }
  std::vector<FileTransferSelectedItem> items =
      selected_file_transfer_items(pane);
  if (items.empty()) {
    return;
  }
  pane->window->browser_action_task.reset();
  pane->window->browser_action_task.emplace(
      run_file_transfer_delete_async(pane, std::move(items)));
}

static void on_file_transfer_delete_item_activate(
    GtkMenuItem *, gpointer data) {
  start_file_transfer_delete(
      static_cast<FileTransferPaneState *>(data));
}

static void refresh_file_transfer_panes(FileTransferWindow *window) {
  start_file_transfer_pane_navigation(
      &window->local, window->local.current_directory);
  if (window->connection_available) {
    start_file_transfer_pane_navigation(
        &window->remote, window->remote.current_directory);
  }
}

static cardio::promise<void> run_file_transfer_window_transfer_async(
    FileTransferWindow *window, FileTransferDirection direction,
    std::vector<std::string> sources,
    std::string destination) {
  const std::size_t selected_count = sources.size();
  const cardio::cancellation cancellation =
      window->transfer_cancel_source->get_cancellation();
  bool succeeded = false;
  try {
    co_await run_file_transfer_async(
        window->client,
        {
            .direction = direction,
            .source_paths = std::move(sources),
            .destination_directory = std::move(destination),
            .callbacks =
                {
                    .conflict =
                        [window](
                            const FileTransferConflict &conflict,
                            cardio::cancellation callback_cancellation) {
                          return prompt_file_transfer_conflict_async(
                              window, conflict,
                              std::move(callback_cancellation));
                        },
                    .failure =
                        [window](
                            const FileTransferFailure &failure,
                            cardio::cancellation callback_cancellation) {
                          return prompt_file_transfer_failure_async(
                              window, failure,
                              std::move(callback_cancellation));
                        },
                    .progress =
                        [window](
                            const FileTransferProgress &progress) {
                          update_file_transfer_progress(window,
                                                        progress);
                        },
                },
        },
        cancellation);
    succeeded = true;
  } catch (const cardio::canceled_exception &) {
    if (!window->destroyed) {
      set_file_transfer_status(window, _("Transfer cancelled"));
    }
  } catch (...) {
    if (!window->destroyed) {
      const std::string message =
          exception_text(std::current_exception());
      if (message.find("canceled") != std::string::npos ||
          message.find("cancelled") != std::string::npos) {
        set_file_transfer_status(window, _("Transfer cancelled"));
      } else {
        set_file_transfer_status(window, _("Transfer failed"));
        show_file_transfer_error(window, message);
      }
    }
  }

  if (window->destroyed) {
    co_return;
  }
  window->transfer_cancel_source.reset();
  set_file_transfer_active(window, false);
  if (succeeded) {
    const char *format =
        direction == FileTransferDirection::send
            ? g_dngettext(GETTEXT_PACKAGE, "Sent %zu item", "Sent %zu items",
                          selected_count)
            : g_dngettext(GETTEXT_PACKAGE, "Received %zu item",
                          "Received %zu items", selected_count);
    set_file_transfer_status(window,
                    format_translated_string(format, selected_count));
  }
  refresh_file_transfer_panes(window);
}

static void start_file_transfer_transfer(
    FileTransferWindow *window, FileTransferDirection direction) {
  if (window == nullptr || window->destroyed ||
      window->transfer_active || window->browser_action_active ||
      !window->connection_available) {
    return;
  }
  FileTransferPaneState *source =
      direction == FileTransferDirection::send
          ? &window->local
          : &window->remote;
  const std::vector<std::string> sources =
      selected_file_transfer_paths(source);
  if (sources.empty()) {
    return;
  }
  const std::string destination =
      direction == FileTransferDirection::send
          ? window->remote.current_directory
          : window->local.current_directory;
  if (destination.empty()) {
    return;
  }

  window->transfer_task.reset();
  window->transfer_cancel_source.emplace();
  set_file_transfer_active(window, true);
  window->transfer_task.emplace(
      run_file_transfer_window_transfer_async(
          window, direction, sources, destination));
}

static void on_file_transfer_item_activate(
    GtkMenuItem *, gpointer data) {
  auto *pane = static_cast<FileTransferPaneState *>(data);
  start_file_transfer_transfer(
      pane->window,
      pane->remote ? FileTransferDirection::receive
                   : FileTransferDirection::send);
}

static void on_file_transfer_cancel_clicked(
    GtkButton *, gpointer data) {
  auto *window = static_cast<FileTransferWindow *>(data);
  if (window == nullptr ||
      !window->transfer_cancel_source.has_value()) {
    return;
  }
  if (window->transfer_cancel_source->cancel()) {
    gtk_widget_set_sensitive(
        window->transfer_cancel_button, FALSE);
  }
}

static gboolean run_closed_callback_idle(gpointer data) {
  auto *callback = static_cast<std::function<void()> *>(data);
  if (*callback) {
    (*callback)();
  }
  delete callback;
  return G_SOURCE_REMOVE;
}

static void on_file_transfer_window_destroy(GtkWidget *, gpointer data) {
  auto *window = static_cast<FileTransferWindow *>(data);
  if (window == nullptr || window->destroyed) {
    return;
  }
  cancel_inline_prompt(window->prompt);
  clear_file_transfer_window_colors(window);
  window->destroyed = true;
  window->window = nullptr;
  window->header_bar = nullptr;
  window->root_overlay = nullptr;
  window->paned = nullptr;
  window->status_bar = nullptr;
  (void)window->stop_source.cancel();
  if (window->transfer_cancel_source.has_value()) {
    (void)window->transfer_cancel_source->cancel();
  }
  stop_file_transfer_pulse(window);
  if (window->closed) {
    g_idle_add(run_closed_callback_idle,
               new std::function<void()>(window->closed));
  }
}

static GtkWidget *create_toolbar_button(
    const char *label, const char *accessible_id) {
  GtkWidget *button = gtk_button_new_with_label(label);
  gestament_gtk_assign_accessible_id(button, accessible_id);
  return button;
}

struct FileTransferTreeCellRendererText {
  GtkCellRendererText parent_instance;
};

struct FileTransferTreeCellRendererTextClass {
  GtkCellRendererTextClass parent_class;
};

static void render_file_transfer_tree_cell_text(
    GtkCellRenderer *renderer, cairo_t *cr, GtkWidget *widget,
    const GdkRectangle *background_area,
    const GdkRectangle *cell_area, GtkCellRendererState flags);

G_DEFINE_TYPE(FileTransferTreeCellRendererText,
              file_transfer_tree_cell_renderer_text,
              GTK_TYPE_CELL_RENDERER_TEXT)

static int get_file_transfer_tree_cell_ink_offset(
    GtkCellRenderer *renderer, GtkWidget *widget,
    const GdkRectangle *cell_area) {
  char *text_value = nullptr;
  g_object_get(renderer, "text", &text_value, nullptr);
  FileTransferGCharPtr text(text_value);
  if (text == nullptr || text.get()[0] == '\0') {
    return 0;
  }

  FileTransferGObjectPtr<PangoLayout> layout(
      gtk_widget_create_pango_layout(widget, text.get()));
  PangoRectangle ink_rect = {};
  PangoRectangle logical_rect = {};
  pango_layout_get_pixel_extents(
      layout.get(), &ink_rect, &logical_rect);

  int vertical_padding = 0;
  gtk_cell_renderer_get_padding(
      renderer, nullptr, &vertical_padding);
  float vertical_alignment = 0.5F;
  gtk_cell_renderer_get_alignment(
      renderer, nullptr, &vertical_alignment);
  const int content_height = std::max(
      0, cell_area->height - 2 * vertical_padding);
  const int logical_height = std::min(
      logical_rect.height, content_height);
  const int available_height = std::max(
      0, cell_area->height -
             (logical_height + 2 * vertical_padding));
  const int aligned_offset = std::max(
      0, static_cast<int>(
             vertical_alignment * available_height));
  const int ink_center_twice =
      2 * (aligned_offset + vertical_padding + ink_rect.y) +
      ink_rect.height;
  return (ink_center_twice - cell_area->height) / 2;
}

static void render_file_transfer_tree_cell_text(
    GtkCellRenderer *renderer, cairo_t *cr, GtkWidget *widget,
    const GdkRectangle *background_area,
    const GdkRectangle *cell_area, GtkCellRendererState flags) {
  const int vertical_offset = get_file_transfer_tree_cell_ink_offset(
      renderer, widget, cell_area);
  GdkRectangle adjusted_cell_area = *cell_area;
  adjusted_cell_area.y -= vertical_offset;

  // GtkCellRendererText aligns the layout's logical rectangle. Font
  // fallback can make its visible ink asymmetric within that rectangle,
  // so move the rendered value by its measured ink center instead.
  cairo_save(cr);
  cairo_rectangle(
      cr, cell_area->x, cell_area->y, cell_area->width,
      cell_area->height);
  cairo_clip(cr);
  GTK_CELL_RENDERER_CLASS(
      file_transfer_tree_cell_renderer_text_parent_class)
      ->render(renderer, cr, widget, background_area,
               &adjusted_cell_area, flags);
  cairo_restore(cr);
}

static void file_transfer_tree_cell_renderer_text_class_init(
    FileTransferTreeCellRendererTextClass *renderer_class) {
  GTK_CELL_RENDERER_CLASS(renderer_class)->render =
      render_file_transfer_tree_cell_text;
}

static void file_transfer_tree_cell_renderer_text_init(
    FileTransferTreeCellRendererText *) {}

struct FileTransferTreeRowMetrics {
  int height = 0;
  int vertical_padding = 0;
  float vertical_alignment = 0.5F;
};

static FileTransferTreeRowMetrics get_file_transfer_tree_row_metrics(
    GtkWidget *tree) {
  static constexpr char row_height_sample[] = "AgÁgjあ漢Ⅳ";
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  g_object_ref_sink(renderer);
  g_object_set(renderer, "text", row_height_sample, nullptr);
  int minimum_height = 0;
  int natural_height = 0;
  gtk_cell_renderer_get_preferred_height(
      renderer, tree, &minimum_height, &natural_height);
  int vertical_padding = 0;
  gtk_cell_renderer_get_padding(
      renderer, nullptr, &vertical_padding);
  g_object_unref(renderer);

  PangoLayout *layout =
      gtk_widget_create_pango_layout(tree, row_height_sample);
  PangoRectangle ink_rect = {};
  PangoRectangle logical_rect = {};
  pango_layout_get_pixel_extents(
      layout, &ink_rect, &logical_rect);
  g_object_unref(layout);
  const int ink_center_twice =
      ink_rect.y * 2 + ink_rect.height;
  const int logical_center_twice =
      logical_rect.y * 2 + logical_rect.height;
  const int ink_center_offset =
      (ink_center_twice - logical_center_twice) / 2;
  const int requested_padding_correction =
      ink_center_offset >= 0
          ? ink_center_offset
          : -ink_center_offset;
  const int padding_correction = std::min(
      vertical_padding, requested_padding_correction);
  // GtkCellRendererText centers the logical rectangle, whose baseline can
  // leave the visible ink low or high. Keep one spare pixel on the opposite
  // side instead of enlarging the row with symmetric blank padding.
  const float vertical_alignment =
      padding_correction == 0
          ? 0.5F
          : (ink_center_offset > 0 ? 0.0F : 1.0F);

  int expander_size = 0;
  int horizontal_separator = 0;
  gtk_widget_style_get(
      tree, "expander-size", &expander_size,
      "horizontal-separator", &horizontal_separator, nullptr);
  const int effective_expander_size =
      expander_size + horizontal_separator / 2;
  return {
      .height = std::max(
          natural_height - padding_correction,
          effective_expander_size),
      .vertical_padding =
          vertical_padding - padding_correction,
      .vertical_alignment = vertical_alignment,
  };
}

static void append_file_transfer_tree_column(
    GtkWidget *tree, const char *title, int model_column,
    bool expand, const FileTransferTreeRowMetrics &row_metrics) {
  GtkCellRenderer *renderer = GTK_CELL_RENDERER(g_object_new(
      file_transfer_tree_cell_renderer_text_get_type(), nullptr));
  int horizontal_padding = 0;
  gtk_cell_renderer_get_padding(
      renderer, &horizontal_padding, nullptr);
  gtk_cell_renderer_set_padding(
      renderer, horizontal_padding,
      row_metrics.vertical_padding);
  float horizontal_alignment = 0.0F;
  gtk_cell_renderer_get_alignment(
      renderer, &horizontal_alignment, nullptr);
  gtk_cell_renderer_set_alignment(
      renderer, horizontal_alignment,
      row_metrics.vertical_alignment);
  gtk_cell_renderer_set_fixed_size(
      renderer, -1, row_metrics.height);
  GtkTreeViewColumn *column =
      gtk_tree_view_column_new_with_attributes(
          title, renderer, "text", model_column, nullptr);
  gtk_tree_view_column_set_sizing(
      column, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_expand(column, expand ? TRUE : FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

static GtkWidget *create_file_transfer_tree(FileTransferPaneState *pane,
                                   const char *accessible_id) {
  pane->store = gtk_tree_store_new(
      file_transfer_tree_column_count, G_TYPE_STRING, G_TYPE_STRING,
      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN,
      G_TYPE_BOOLEAN);
  GtkWidget *tree = gtk_tree_view_new_with_model(
      GTK_TREE_MODEL(pane->store));
  gestament_gtk_assign_accessible_id(tree, accessible_id);
  gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tree), TRUE);
  gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(tree), TRUE);
  gtk_tree_view_set_search_column(
      GTK_TREE_VIEW(tree), file_transfer_tree_name_column);
  const FileTransferTreeRowMetrics row_metrics =
      get_file_transfer_tree_row_metrics(tree);
  append_file_transfer_tree_column(
      tree, _("Name"), file_transfer_tree_name_column, true, row_metrics);
  append_file_transfer_tree_column(
      tree, _("Size"), file_transfer_tree_size_column, false, row_metrics);
  append_file_transfer_tree_column(
      tree, _("Modified"), file_transfer_tree_modified_column, false,
      row_metrics);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(tree));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  g_signal_connect(tree, "row-expanded",
                   G_CALLBACK(on_file_transfer_row_expanded), pane);
  g_signal_connect(tree, "row-activated",
                   G_CALLBACK(on_file_transfer_row_activated), pane);
  g_signal_connect(tree, "button-press-event",
                   G_CALLBACK(on_file_transfer_tree_button_press), pane);
  return tree;
}

static GtkWidget *create_file_transfer_pane(
    FileTransferWindow *window, FileTransferPaneState *pane, bool remote) {
  pane->window = window;
  pane->remote = remote;
  pane->frame = gtk_frame_new(remote ? _("Remote") : _("Local"));
  gestament_gtk_assign_accessible_id(
      pane->frame,
      remote ? "file_transfer_remote_group" : "file_transfer_local_group");
  gtk_widget_set_hexpand(pane->frame, TRUE);
  gtk_widget_set_vexpand(pane->frame, TRUE);
  gtk_widget_set_margin_start(
      pane->frame,
      remote ? file_transfer_pane_spacing / 2 : file_transfer_content_padding);
  gtk_widget_set_margin_end(
      pane->frame,
      remote ? file_transfer_content_padding : file_transfer_pane_spacing / 2);
  gtk_widget_set_margin_top(pane->frame, file_transfer_content_padding);
  gtk_widget_set_margin_bottom(pane->frame, file_transfer_content_padding);

  GtkWidget *box =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, file_transfer_control_spacing);
  gtk_container_set_border_width(
      GTK_CONTAINER(box), file_transfer_content_padding);
  gtk_container_add(GTK_CONTAINER(pane->frame), box);

  GtkWidget *toolbar =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, file_transfer_control_spacing);
  gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, TRUE, 0);
  pane->path_entry = gtk_entry_new();
  gestament_gtk_assign_accessible_id(
      pane->path_entry,
      remote ? "file_transfer_remote_path_entry"
             : "file_transfer_local_path_entry");
  gtk_widget_set_hexpand(pane->path_entry, TRUE);
  gtk_box_pack_start(GTK_BOX(toolbar), pane->path_entry, TRUE, TRUE, 0);
  pane->up_button = create_toolbar_button(
      _("Up"), remote ? "file_transfer_remote_up_button"
                    : "file_transfer_local_up_button");
  gtk_box_pack_start(GTK_BOX(toolbar), pane->up_button, FALSE, TRUE, 0);
  pane->refresh_button = create_toolbar_button(
      _("Refresh"), remote ? "file_transfer_remote_refresh_button"
                         : "file_transfer_local_refresh_button");
  gtk_box_pack_start(
      GTK_BOX(toolbar), pane->refresh_button, FALSE, TRUE, 0);

  GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
      GTK_POLICY_AUTOMATIC);
  gestament_gtk_assign_accessible_id(
      gtk_scrolled_window_get_vscrollbar(
          GTK_SCROLLED_WINDOW(scroller)),
      remote ? "file_transfer_remote_vertical_scrollbar"
             : "file_transfer_local_vertical_scrollbar");
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
  pane->tree = create_file_transfer_tree(
      pane, remote ? "file_transfer_remote_tree" : "file_transfer_local_tree");
  gtk_container_add(GTK_CONTAINER(scroller), pane->tree);

  pane->menu = gtk_menu_new();
  gtk_menu_attach_to_widget(
      GTK_MENU(pane->menu), pane->tree, nullptr);
  pane->transfer_item = gtk_menu_item_new_with_label(
      remote ? _("Receive") : _("Send"));
  gestament_gtk_assign_accessible_id(
      pane->transfer_item,
      remote ? "file_transfer_receive_item" : "file_transfer_send_item");
  gtk_menu_shell_append(GTK_MENU_SHELL(pane->menu),
                        pane->transfer_item);
  g_signal_connect(
      pane->transfer_item, "activate",
      G_CALLBACK(on_file_transfer_item_activate), pane);
  gtk_menu_shell_append(GTK_MENU_SHELL(pane->menu),
                        gtk_separator_menu_item_new());
  pane->rename_item = gtk_menu_item_new_with_label(_("Rename"));
  gestament_gtk_assign_accessible_id(
      pane->rename_item,
      remote ? "file_transfer_remote_rename_item"
             : "file_transfer_local_rename_item");
  gtk_menu_shell_append(GTK_MENU_SHELL(pane->menu), pane->rename_item);
  g_signal_connect(
      pane->rename_item, "activate",
      G_CALLBACK(on_file_transfer_rename_item_activate), pane);
  pane->delete_item = gtk_menu_item_new_with_label(_("Delete"));
  gestament_gtk_assign_accessible_id(
      pane->delete_item,
      remote ? "file_transfer_remote_delete_item"
             : "file_transfer_local_delete_item");
  gtk_menu_shell_append(GTK_MENU_SHELL(pane->menu), pane->delete_item);
  g_signal_connect(
      pane->delete_item, "activate",
      G_CALLBACK(on_file_transfer_delete_item_activate), pane);
  gtk_widget_show_all(pane->menu);

  g_signal_connect(
      pane->path_entry, "activate",
      G_CALLBACK(on_file_transfer_path_activate), pane);
  g_signal_connect(
      pane->up_button, "clicked",
      G_CALLBACK(on_file_transfer_up_clicked), pane);
  g_signal_connect(
      pane->refresh_button, "clicked",
      G_CALLBACK(on_file_transfer_refresh_clicked), pane);
  return pane->frame;
}

static void apply_file_transfer_window_style(FileTransferWindow *window) {
  static constexpr char css[] =
      ".file-transfer-dim { background-color: rgba(0, 0, 0, 0.42); }"
      ".file-transfer-progress { background-color: @theme_bg_color; "
      "padding: 10px; }";
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, css, -1, nullptr);
  GtkStyleContext *dim_context =
      gtk_widget_get_style_context(window->dim_overlay);
  gtk_style_context_add_class(dim_context, "file-transfer-dim");
  gtk_style_context_add_provider(
      dim_context, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  GtkStyleContext *progress_context =
      gtk_widget_get_style_context(window->transfer_overlay);
  gtk_style_context_add_class(progress_context, "file-transfer-progress");
  gtk_style_context_add_provider(
      progress_context, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static void clear_file_transfer_window_colors(FileTransferWindow *window) {
  if (window == nullptr) {
    return;
  }

  if (window->exterior_background_provider != nullptr) {
    gtk_style_context_remove_provider(
        gtk_widget_get_style_context(window->header_bar),
        GTK_STYLE_PROVIDER(window->exterior_background_provider));
    gtk_style_context_remove_provider(
        gtk_widget_get_style_context(window->status_bar),
        GTK_STYLE_PROVIDER(window->exterior_background_provider));
    g_clear_object(&window->exterior_background_provider);
  }
  if (window->exterior_component_background_provider != nullptr) {
    GdkScreen *screen = gtk_widget_get_screen(window->window);
    if (screen != nullptr) {
      gtk_style_context_remove_provider_for_screen(
          screen,
          GTK_STYLE_PROVIDER(
              window->exterior_component_background_provider));
    }
    g_clear_object(
        &window->exterior_component_background_provider);
  }
  gtk_style_context_remove_class(
      gtk_widget_get_style_context(window->header_bar),
      file_transfer_exterior_component_style_class);
  gtk_style_context_remove_class(
      gtk_widget_get_style_context(window->status_bar),
      file_transfer_exterior_component_style_class);
  if (window->background_provider != nullptr) {
    remove_widget_tree_background_provider(
        window->paned, window->background_provider);
    remove_widget_tree_background_provider(
        window->transfer_overlay,
        window->background_provider);
    remove_widget_tree_background_provider(
        window->prompt_panel, window->background_provider);
    g_clear_object(&window->background_provider);
  }
  if (window->component_background_provider != nullptr) {
    remove_widget_tree_background_provider(
        window->paned, window->component_background_provider);
    remove_widget_tree_background_provider(
        window->transfer_overlay,
        window->component_background_provider);
    remove_widget_tree_background_provider(
        window->prompt_panel, window->component_background_provider);
    g_clear_object(&window->component_background_provider);
  }
  if (window->popup_component_background_provider != nullptr) {
    GdkScreen *screen = gtk_widget_get_screen(window->window);
    if (screen != nullptr) {
      gtk_style_context_remove_provider_for_screen(
          screen,
          GTK_STYLE_PROVIDER(
              window->popup_component_background_provider));
    }
    g_clear_object(
        &window->popup_component_background_provider);
  }
}

std::shared_ptr<FileTransferWindow>
create_file_transfer_window(FileTransferWindowOptions options) {
  if (options.protocol_name.empty()) {
    throw std::invalid_argument("File transfer protocol name is required");
  }
  auto state = std::make_shared<FileTransferWindow>();
  state->closed = std::move(options.closed);
  state->local.current_directory =
      std::move(options.local_directory);
  state->remote.current_directory =
      std::move(options.remote_directory);

  state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gestament_gtk_assign_accessible_id(
      state->window, "file_transfer_window");
  const std::string title =
      options.connection_name.empty()
          ? options.protocol_name
          : options.connection_name + " — " + options.protocol_name;
  gtk_window_set_title(GTK_WINDOW(state->window), title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(state->window), 1040, 640);

  state->header_bar = gtk_header_bar_new();
  gestament_gtk_assign_accessible_id(
      state->header_bar, "file_transfer_header_bar");
  gtk_header_bar_set_title(
      GTK_HEADER_BAR(state->header_bar), title.c_str());
  gtk_header_bar_set_show_close_button(
      GTK_HEADER_BAR(state->header_bar), TRUE);
  gtk_window_set_titlebar(
      GTK_WINDOW(state->window), state->header_bar);

  state->root_overlay = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(state->window),
                    state->root_overlay);
  GtkWidget *content =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(state->root_overlay), content);

  state->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gestament_gtk_assign_accessible_id(
      state->paned, "file_transfer_root_paned");
  gtk_widget_set_hexpand(state->paned, TRUE);
  gtk_widget_set_vexpand(state->paned, TRUE);
  gtk_box_pack_start(GTK_BOX(content), state->paned, TRUE, TRUE, 0);
  GtkWidget *local =
      create_file_transfer_pane(state.get(), &state->local, false);
  GtkWidget *remote =
      create_file_transfer_pane(state.get(), &state->remote, true);
  gtk_paned_pack1(GTK_PANED(state->paned), local, TRUE, FALSE);
  gtk_paned_pack2(GTK_PANED(state->paned), remote, TRUE, FALSE);
  gtk_paned_set_position(GTK_PANED(state->paned), 520);

  state->status_bar = gtk_event_box_new();
  gtk_event_box_set_visible_window(
      GTK_EVENT_BOX(state->status_bar), TRUE);
  gestament_gtk_assign_accessible_id(
      state->status_bar, "file_transfer_status_bar");
  GtkWidget *status_content =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL, file_transfer_control_spacing);
  gtk_widget_set_margin_start(
      status_content, file_transfer_content_padding);
  gtk_widget_set_margin_end(
      status_content, file_transfer_content_padding);
  gtk_widget_set_margin_top(
      status_content, file_transfer_control_spacing);
  gtk_widget_set_margin_bottom(
      status_content, file_transfer_control_spacing);
  gtk_container_add(
      GTK_CONTAINER(state->status_bar), status_content);
  gtk_box_pack_start(
      GTK_BOX(content), state->status_bar, FALSE, TRUE, 0);
  state->status_label = gtk_label_new(_("Connecting"));
  gestament_gtk_assign_accessible_id(
      state->status_label, "file_transfer_status_label");
  gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0F);
  gtk_box_pack_start(
      GTK_BOX(status_content), state->status_label, TRUE, TRUE, 0);

  state->dim_overlay = gtk_event_box_new();
  gestament_gtk_assign_accessible_id(
      state->dim_overlay, "file_transfer_dim_overlay");
  gtk_widget_set_no_show_all(state->dim_overlay, TRUE);
  gtk_widget_set_visible(state->dim_overlay, FALSE);
  gtk_widget_set_halign(state->dim_overlay, GTK_ALIGN_FILL);
  gtk_widget_set_valign(state->dim_overlay, GTK_ALIGN_FILL);
  gtk_overlay_add_overlay(GTK_OVERLAY(state->root_overlay),
                          state->dim_overlay);

  state->transfer_overlay =
      gtk_frame_new(nullptr);
  gestament_gtk_assign_accessible_id(
      state->transfer_overlay, "file_transfer_overlay");
  gtk_widget_set_no_show_all(state->transfer_overlay, TRUE);
  gtk_widget_set_visible(state->transfer_overlay, FALSE);
  gtk_widget_set_halign(state->transfer_overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(state->transfer_overlay, GTK_ALIGN_START);
  gtk_widget_set_margin_top(state->transfer_overlay, 18);
  GtkWidget *transfer_box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL, file_transfer_control_spacing);
  gtk_container_set_border_width(
      GTK_CONTAINER(transfer_box), file_transfer_content_padding);
  gtk_container_add(GTK_CONTAINER(state->transfer_overlay),
                    transfer_box);
  state->transfer_label = gtk_label_new(_("Preparing transfer…"));
  gestament_gtk_assign_accessible_id(
      state->transfer_label, "file_transfer_label");
  gtk_label_set_xalign(GTK_LABEL(state->transfer_label), 0.0F);
  gtk_box_pack_start(
      GTK_BOX(transfer_box), state->transfer_label, FALSE, TRUE, 0);
  state->transfer_progress = gtk_progress_bar_new();
  gestament_gtk_assign_accessible_id(
      state->transfer_progress, "file_transfer_progress");
  gtk_widget_set_size_request(state->transfer_progress, 280, -1);
  gtk_box_pack_start(
      GTK_BOX(transfer_box), state->transfer_progress, FALSE, TRUE, 0);
  state->transfer_cancel_button =
      gtk_button_new_with_label(_("Cancel"));
  gestament_gtk_assign_accessible_id(
      state->transfer_cancel_button,
      "file_transfer_cancel_button");
  gtk_widget_set_halign(
      state->transfer_cancel_button, GTK_ALIGN_END);
  gtk_box_pack_start(
      GTK_BOX(transfer_box), state->transfer_cancel_button,
      FALSE, TRUE, 0);
  gtk_overlay_add_overlay(GTK_OVERLAY(state->root_overlay),
                          state->transfer_overlay);

  const InlinePromptWidgets prompt_widgets =
      create_inline_prompt_widgets("file_transfer_prompt");
  state->prompt_panel = prompt_widgets.panel;
  state->prompt_background = prompt_widgets.background;
  state->prompt = create_inline_prompt_controller(prompt_widgets);
  gtk_overlay_add_overlay(GTK_OVERLAY(state->root_overlay),
                          state->prompt_panel);

  apply_file_transfer_window_style(state.get());
  set_file_transfer_window_colors(state, options.colors);
  g_signal_connect(
      state->transfer_cancel_button, "clicked",
      G_CALLBACK(on_file_transfer_cancel_clicked), state.get());
  g_signal_connect(state->window, "destroy",
                   G_CALLBACK(on_file_transfer_window_destroy), state.get());
  update_file_transfer_sensitivity(state.get());
  return state;
}

GtkWidget *file_transfer_window_widget(
    const std::shared_ptr<FileTransferWindow> &window) noexcept {
  return window == nullptr ? nullptr : window->window;
}

void show_file_transfer_window(const std::shared_ptr<FileTransferWindow> &window) {
  if (window == nullptr || window->window == nullptr ||
      window->destroyed) {
    return;
  }
  window->shown = true;
  gtk_widget_show_all(window->window);
  update_file_transfer_overlay_presentation(window.get());
  if (!window->local_load_started) {
    window->local_load_started = true;
    start_file_transfer_pane_navigation(
        &window->local, window->local.current_directory);
  }
  if (window->connection_state == FileTransferConnectionState::ready &&
      !window->remote_load_started) {
    window->remote_load_started = true;
    start_file_transfer_pane_navigation(
        &window->remote, window->remote.current_directory);
  }
}

void attach_file_transfer_window_client(
    const std::shared_ptr<FileTransferWindow> &window,
    std::shared_ptr<RemoteFileClient> client) {
  if (window == nullptr || window->destroyed) {
    return;
  }
  if (client == nullptr) {
    throw std::invalid_argument("Remote file client is required");
  }
  if (window->client != nullptr) {
    throw std::logic_error("Remote file client is already attached");
  }
  window->client = std::move(client);
  window->connection_available = true;
  window->connection_state = FileTransferConnectionState::ready;
  set_file_transfer_status(window.get(), _("Ready"));
  update_file_transfer_overlay_presentation(window.get());
  update_file_transfer_sensitivity(window.get());
  if (window->shown && !window->remote_load_started) {
    window->remote_load_started = true;
    start_file_transfer_pane_navigation(
        &window->remote, window->remote.current_directory);
  }
}

cardio::promise<InlinePromptResponse> prompt_file_transfer_window_async(
    const std::shared_ptr<FileTransferWindow> &window,
    InlinePromptRequest request, cardio::cancellation cancellation) {
  if (window == nullptr || window->destroyed || window->prompt == nullptr ||
      cancellation.is_cancellation_requested()) {
    co_return InlinePromptResponse{};
  }
  window->connection_state = FileTransferConnectionState::authenticating;
  set_file_transfer_status(window.get(), _("Authenticating"));
  update_file_transfer_overlay_presentation(window.get());
  InlinePromptResponse response = co_await prompt_inline_async(
      window->prompt, std::move(request), std::move(cancellation));
  if (!window->destroyed &&
      window->connection_state ==
          FileTransferConnectionState::authenticating) {
    window->connection_state = FileTransferConnectionState::connecting;
    set_file_transfer_status(window.get(), _("Connecting"));
    update_file_transfer_overlay_presentation(window.get());
  }
  co_return response;
}

cardio::promise<void> show_file_transfer_window_connection_error_async(
    const std::shared_ptr<FileTransferWindow> &window, std::string title,
    std::string message, cardio::cancellation cancellation) {
  if (window == nullptr || window->destroyed || window->prompt == nullptr ||
      cancellation.is_cancellation_requested()) {
    co_return;
  }
  window->connection_available = false;
  window->connection_state = FileTransferConnectionState::failed;
  set_file_transfer_status(window.get(), _("Connection failed"));
  update_file_transfer_sensitivity(window.get());
  update_file_transfer_overlay_presentation(window.get());
  const InlinePromptResponse response = co_await prompt_inline_async(
      window->prompt,
      {
          .title = std::move(title),
          .message = std::move(message),
          .accept_label = _("Close"),
          .cancel_label = _("Cancel"),
          .input_required = false,
          .echo = false,
          .cancel_visible = false,
      },
      std::move(cancellation));
  if (response.accepted && !window->destroyed && window->window != nullptr) {
    gtk_widget_destroy(window->window);
  }
}

void set_file_transfer_window_connection_available(
    const std::shared_ptr<FileTransferWindow> &window, bool available) {
  if (window == nullptr || window->destroyed ||
      window->connection_available == available) {
    return;
  }
  window->connection_available = available;
  window->connection_state = available
                                 ? FileTransferConnectionState::ready
                                 : FileTransferConnectionState::disconnected;
  if (!available) {
    if (window->transfer_cancel_source.has_value()) {
      (void)window->transfer_cancel_source->cancel();
    }
    set_file_transfer_status(window.get(), _("Disconnected"));
  } else {
    set_file_transfer_status(window.get(), _("Ready"));
  }
  update_file_transfer_overlay_presentation(window.get());
  update_file_transfer_sensitivity(window.get());
}

void set_file_transfer_window_colors(
    const std::shared_ptr<FileTransferWindow> &window,
    const GeneralColorSettings &settings) {
  if (window == nullptr || window->destroyed) {
    return;
  }

  clear_file_transfer_window_colors(window.get());
  if (settings.exterior_background.has_value()) {
    window->exterior_background_provider =
        create_widget_background_provider(
            settings.exterior_background.value(),
            "File transfer exterior");
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(window->header_bar),
        GTK_STYLE_PROVIDER(window->exterior_background_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(window->status_bar),
        GTK_STYLE_PROVIDER(window->exterior_background_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    window->exterior_component_background_provider =
        create_scoped_widget_component_background_provider(
            settings.exterior_background.value(),
            file_transfer_exterior_component_style_class,
            "File transfer exterior controls");
    GdkScreen *screen = gtk_widget_get_screen(window->window);
    if (window->exterior_component_background_provider != nullptr &&
        screen != nullptr) {
      gtk_style_context_add_class(
          gtk_widget_get_style_context(window->header_bar),
          file_transfer_exterior_component_style_class);
      gtk_style_context_add_class(
          gtk_widget_get_style_context(window->status_bar),
          file_transfer_exterior_component_style_class);
      gtk_style_context_add_provider_for_screen(
          screen,
          GTK_STYLE_PROVIDER(
              window->exterior_component_background_provider),
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 3);
    } else {
      g_clear_object(
          &window->exterior_component_background_provider);
    }
  }
  if (settings.background.has_value()) {
    window->background_provider =
        create_widget_background_provider(
            settings.background.value(), "File transfer browser");
    add_widget_tree_background_provider(
        window->paned, window->background_provider);
    add_widget_tree_background_provider_at_priority(
        window->transfer_overlay,
        window->background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    add_widget_tree_background_provider_at_priority(
        window->prompt_panel, window->background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    window->component_background_provider =
        create_widget_component_background_provider(
            settings.background.value(), "File transfer controls");
    add_widget_tree_background_provider_at_priority(
        window->paned, window->component_background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    add_widget_tree_background_provider_at_priority(
        window->transfer_overlay,
        window->component_background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    add_widget_tree_background_provider_at_priority(
        window->prompt_panel, window->component_background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    window->popup_component_background_provider =
        create_widget_popup_component_background_provider(
            settings.background.value(), "File transfer popups");
    GdkScreen *screen = gtk_widget_get_screen(window->window);
    if (window->popup_component_background_provider != nullptr &&
        screen != nullptr) {
      gtk_style_context_add_provider_for_screen(
          screen,
          GTK_STYLE_PROVIDER(
              window->popup_component_background_provider),
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    } else {
      g_clear_object(
          &window->popup_component_background_provider);
    }
  }
}

void present_file_transfer_window(
    const std::shared_ptr<FileTransferWindow> &window) {
  if (window == nullptr || window->window == nullptr ||
      window->destroyed) {
    return;
  }
  gtk_window_present(GTK_WINDOW(window->window));
}

} // namespace elder_terms
