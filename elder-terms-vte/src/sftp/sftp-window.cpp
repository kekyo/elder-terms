#include "sftp-window.h"

#include <algorithm>
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

#include "../widget-background.h"
#include "sftp-transfer-engine.h"

namespace elder_terms {

static constexpr char local_browser_attributes[] =
    G_FILE_ATTRIBUTE_STANDARD_NAME ","
    G_FILE_ATTRIBUTE_STANDARD_TYPE ","
    G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK ","
    G_FILE_ATTRIBUTE_STANDARD_SIZE ","
    G_FILE_ATTRIBUTE_UNIX_MODE ","
    G_FILE_ATTRIBUTE_TIME_ACCESS ","
    G_FILE_ATTRIBUTE_TIME_MODIFIED;
static constexpr guint transfer_progress_pulse_period_ms = 100;
static constexpr const char *sftp_exterior_component_style_class =
    "sftp-exterior-components";
static constexpr int sftp_content_padding = 12;
static constexpr int sftp_pane_spacing = 12;
static constexpr int sftp_control_spacing = 8;

enum SftpTreeColumn {
  sftp_tree_name_column = 0,
  sftp_tree_size_column = 1,
  sftp_tree_modified_column = 2,
  sftp_tree_path_column = 3,
  sftp_tree_type_column = 4,
  sftp_tree_loaded_column = 5,
  sftp_tree_dummy_column = 6,
  sftp_tree_column_count = 7,
};

struct SftpGObjectDeleter {
  void operator()(void *object) const {
    if (object != nullptr) {
      g_object_unref(object);
    }
  }
};

struct SftpGFreeDeleter {
  void operator()(void *value) const {
    g_free(value);
  }
};

template <typename T>
using SftpGObjectPtr =
    std::unique_ptr<T, SftpGObjectDeleter>;
using SftpGCharPtr = std::unique_ptr<char, SftpGFreeDeleter>;

struct SftpWindow;
static void clear_sftp_window_colors(SftpWindow *window);

struct SftpPaneState {
  SftpWindow *window = nullptr;
  bool remote = false;
  GtkWidget *frame = nullptr;
  GtkWidget *path_entry = nullptr;
  GtkWidget *up_button = nullptr;
  GtkWidget *refresh_button = nullptr;
  GtkWidget *tree = nullptr;
  GtkTreeStore *store = nullptr;
  GtkWidget *menu = nullptr;
  GtkWidget *transfer_item = nullptr;
  std::string current_directory;
  std::optional<cardio::promise<void>> task;
  bool busy = false;
  std::uint64_t generation = 0;
};

struct SftpChoiceDialogRequest {
  GtkWidget *dialog = nullptr;
  std::shared_ptr<cardio::promise_source<int>> source;
  cardio::cancellation_registration cancellation_registration;
  bool completed = false;
};

struct SftpWindow {
  GtkWidget *window = nullptr;
  GtkWidget *header_bar = nullptr;
  GtkWidget *root_overlay = nullptr;
  GtkWidget *paned = nullptr;
  GtkWidget *dim_overlay = nullptr;
  GtkWidget *transfer_overlay = nullptr;
  GtkWidget *transfer_label = nullptr;
  GtkWidget *transfer_progress = nullptr;
  GtkWidget *transfer_cancel_button = nullptr;
  GtkWidget *status_bar = nullptr;
  GtkWidget *status_label = nullptr;
  GtkCssProvider *exterior_background_provider = nullptr;
  GtkCssProvider *exterior_component_background_provider = nullptr;
  GtkCssProvider *background_provider = nullptr;
  GtkCssProvider *component_background_provider = nullptr;
  GtkCssProvider *popup_component_background_provider = nullptr;
  SftpPaneState local;
  SftpPaneState remote;
  std::shared_ptr<SftpClient> client;
  std::function<void()> closed;
  cardio::cancellation_source stop_source;
  std::optional<cardio::cancellation_source> transfer_cancel_source;
  std::optional<cardio::promise<void>> transfer_task;
  guint transfer_pulse_source = 0;
  bool initial_load_started = false;
  bool connection_available = true;
  bool transfer_active = false;
  bool destroyed = false;

  ~SftpWindow() {
    (void)stop_source.cancel();
    if (transfer_cancel_source.has_value()) {
      (void)transfer_cancel_source->cancel();
    }
    if (transfer_pulse_source != 0) {
      g_source_remove(transfer_pulse_source);
      transfer_pulse_source = 0;
    }
    if (window != nullptr && !destroyed) {
      clear_sftp_window_colors(this);
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
    return "Unknown SFTP failure";
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception &exception) {
    return exception.what();
  } catch (...) {
    return "Unknown SFTP failure";
  }
}

static std::string file_size_text(const SftpFileAttributes &attributes) {
  if (attributes.type != SftpFileType::regular) {
    return {};
  }
  SftpGCharPtr formatted(
      g_format_size(static_cast<guint64>(attributes.size)));
  return formatted == nullptr ? std::string()
                              : std::string(formatted.get());
}

static std::string modification_time_text(
    const SftpFileAttributes &attributes) {
  if (attributes.modification_time_unix_seconds <= 0) {
    return {};
  }
  const std::time_t seconds = static_cast<std::time_t>(
      attributes.modification_time_unix_seconds);
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

static int type_sort_rank(SftpFileType type) {
  if (type == SftpFileType::directory) {
    return 0;
  }
  if (type == SftpFileType::regular) {
    return 1;
  }
  if (type == SftpFileType::symbolic_link) {
    return 2;
  }
  return 3;
}

static void sort_browser_entries(
    std::vector<SftpFileAttributes> *entries) {
  std::sort(
      entries->begin(), entries->end(),
      [](const SftpFileAttributes &left,
         const SftpFileAttributes &right) {
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
      store, &child, sftp_tree_name_column, "Loading…",
      sftp_tree_size_column, "", sftp_tree_modified_column, "",
      sftp_tree_path_column, "", sftp_tree_type_column,
      static_cast<int>(SftpFileType::other),
      sftp_tree_loaded_column, TRUE, sftp_tree_dummy_column, TRUE, -1);
}

static void set_browser_row(
    GtkTreeStore *store, GtkTreeIter *iterator,
    const SftpFileAttributes &attributes) {
  const std::string size = file_size_text(attributes);
  const std::string modified = modification_time_text(attributes);
  gtk_tree_store_set(
      store, iterator, sftp_tree_name_column,
      attributes.name.c_str(), sftp_tree_size_column, size.c_str(),
      sftp_tree_modified_column, modified.c_str(),
      sftp_tree_path_column, attributes.path.c_str(),
      sftp_tree_type_column, static_cast<int>(attributes.type),
      sftp_tree_loaded_column,
      attributes.type != SftpFileType::directory,
      sftp_tree_dummy_column, FALSE, -1);
  if (attributes.type == SftpFileType::directory) {
    append_dummy_row(store, iterator);
  }
}

static void append_browser_row(
    GtkTreeStore *store, GtkTreeIter *parent,
    const SftpFileAttributes &attributes) {
  GtkTreeIter iterator;
  gtk_tree_store_append(store, &iterator, parent);
  set_browser_row(store, &iterator, attributes);
}

static void append_browser_entries(
    GtkTreeStore *store, GtkTreeIter *parent,
    std::vector<SftpFileAttributes> entries) {
  sort_browser_entries(&entries);
  for (const SftpFileAttributes &entry : entries) {
    append_browser_row(store, parent, entry);
  }
}

static void replace_dummy_with_browser_entries(
    GtkTreeStore *store, GtkTreeIter *parent,
    std::vector<SftpFileAttributes> entries) {
  sort_browser_entries(&entries);
  GtkTreeIter dummy;
  if (!gtk_tree_model_iter_children(
          GTK_TREE_MODEL(store), &dummy, parent)) {
    for (const SftpFileAttributes &entry : entries) {
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

static SftpFileType local_browser_file_type(GFileInfo *info) {
  if (g_file_info_get_is_symlink(info) != FALSE ||
      g_file_info_get_file_type(info) == G_FILE_TYPE_SYMBOLIC_LINK) {
    return SftpFileType::symbolic_link;
  }
  if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
    return SftpFileType::directory;
  }
  if (g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR) {
    return SftpFileType::regular;
  }
  return SftpFileType::other;
}

static SftpFileAttributes local_browser_attributes_for(
    GFile *child, GFileInfo *info) {
  SftpGCharPtr path(g_file_get_path(child));
  if (path == nullptr) {
    throw std::runtime_error(
        "SFTP local pane only supports native filesystem paths");
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
              : 0,
      .access_time_unix_seconds =
          g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_ACCESS)
              ? static_cast<std::int64_t>(
                    g_file_info_get_attribute_uint64(
                        info, G_FILE_ATTRIBUTE_TIME_ACCESS))
              : 0,
      .modification_time_unix_seconds =
          g_file_info_has_attribute(
              info, G_FILE_ATTRIBUTE_TIME_MODIFIED)
              ? static_cast<std::int64_t>(
                    g_file_info_get_attribute_uint64(
                        info, G_FILE_ATTRIBUTE_TIME_MODIFIED))
              : 0,
  };
}

static cardio::promise<std::vector<SftpFileAttributes>>
list_local_browser_directory_async(
    std::string path, cardio::cancellation cancellation) {
  SftpGObjectPtr<GFile> directory(
      g_file_new_for_path(path.c_str()));
  SftpGObjectPtr<GFileEnumerator> enumerator(
      co_await enumerate_local_browser_async(
          directory.get(), cancellation));
  std::vector<SftpFileAttributes> entries;
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
      SftpGObjectPtr<GFile> child(
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
  SftpGCharPtr canonical(
      g_canonicalize_filename(path.c_str(), nullptr));
  if (canonical == nullptr || canonical.get()[0] == '\0') {
    throw std::runtime_error("Local directory path is empty");
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

static void set_sftp_status(SftpWindow *window,
                            const std::string &text) {
  if (window == nullptr || window->destroyed ||
      window->status_label == nullptr) {
    return;
  }
  gtk_label_set_text(GTK_LABEL(window->status_label), text.c_str());
}

static void update_sftp_sensitivity(SftpWindow *window) {
  if (window == nullptr || window->destroyed) {
    return;
  }
  const bool idle = !window->transfer_active;
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
}

static void set_widget_visible(GtkWidget *widget, bool visible) {
  gtk_widget_set_no_show_all(widget, !visible);
  gtk_widget_set_visible(widget, visible);
  if (visible) {
    gtk_widget_show_all(widget);
  }
}

static gboolean pulse_sftp_transfer_progress(gpointer data) {
  auto *window = static_cast<SftpWindow *>(data);
  if (window == nullptr || window->destroyed ||
      !window->transfer_active ||
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

static void stop_sftp_transfer_pulse(SftpWindow *window) {
  if (window->transfer_pulse_source == 0) {
    return;
  }
  g_source_remove(window->transfer_pulse_source);
  window->transfer_pulse_source = 0;
}

static void set_sftp_transfer_active(SftpWindow *window,
                                     bool active) {
  window->transfer_active = active;
  set_widget_visible(window->dim_overlay, active);
  set_widget_visible(window->transfer_overlay, active);
  gtk_widget_set_sensitive(window->transfer_cancel_button,
                           active ? TRUE : FALSE);
  if (active) {
    gtk_label_set_text(GTK_LABEL(window->transfer_label),
                       "Preparing transfer…");
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), 0.0);
    if (window->transfer_pulse_source == 0) {
      window->transfer_pulse_source = g_timeout_add(
          transfer_progress_pulse_period_ms,
          pulse_sftp_transfer_progress, window);
    }
  } else {
    stop_sftp_transfer_pulse(window);
  }
  update_sftp_sensitivity(window);
}

static void update_sftp_transfer_progress(
    SftpWindow *window, const SftpTransferProgress &progress) {
  if (window == nullptr || window->destroyed ||
      !window->transfer_active) {
    return;
  }
  std::string label = "Transferring";
  if (progress.total_items > 0) {
    label += " " + std::to_string(progress.completed_items) +
             " of " + std::to_string(progress.total_items);
  }
  if (!progress.current_path.empty()) {
    label += " — " +
             std::filesystem::path(progress.current_path)
                 .filename()
                 .string();
  }
  gtk_label_set_text(GTK_LABEL(window->transfer_label),
                     label.c_str());

  if (progress.total_bytes > 0) {
    stop_sftp_transfer_pulse(window);
    const double fraction = std::min(
        1.0,
        static_cast<double>(progress.transferred_bytes) /
            static_cast<double>(progress.total_bytes));
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), fraction);
  } else if (progress.total_items > 0) {
    stop_sftp_transfer_pulse(window);
    const double fraction = std::min(
        1.0,
        static_cast<double>(progress.completed_items) /
            static_cast<double>(progress.total_items));
    gtk_progress_bar_set_fraction(
        GTK_PROGRESS_BAR(window->transfer_progress), fraction);
  }
}

static void complete_choice_dialog(
    SftpChoiceDialogRequest *request, int response) {
  if (request == nullptr || request->completed) {
    return;
  }
  request->completed = true;
  request->cancellation_registration = {};
  if (request->dialog != nullptr) {
    GtkWidget *dialog = std::exchange(request->dialog, nullptr);
    g_signal_handlers_disconnect_by_data(dialog, request);
    gtk_widget_destroy(dialog);
  }
  (void)request->source->try_resolve(response);
}

static void on_choice_dialog_response(
    GtkDialog *, gint response, gpointer data) {
  complete_choice_dialog(
      static_cast<SftpChoiceDialogRequest *>(data), response);
}

static gboolean cancel_choice_dialog_idle(gpointer data) {
  auto *weak =
      static_cast<std::weak_ptr<SftpChoiceDialogRequest> *>(data);
  const std::shared_ptr<SftpChoiceDialogRequest> request =
      weak->lock();
  delete weak;
  if (request != nullptr) {
    complete_choice_dialog(request.get(), GTK_RESPONSE_CANCEL);
  }
  return G_SOURCE_REMOVE;
}

static cardio::promise<int> prompt_sftp_choice_async(
    SftpWindow *window, GtkMessageType type,
    const std::string &title, const std::string &detail,
    std::vector<std::pair<std::string, int>> buttons,
    const char *accessible_id,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  auto request = std::make_shared<SftpChoiceDialogRequest>();
  request->source =
      std::make_shared<cardio::promise_source<int>>();
  request->dialog = gtk_message_dialog_new(
      GTK_WINDOW(window->window),
      static_cast<GtkDialogFlags>(
          GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
      type, GTK_BUTTONS_NONE, "%s", title.c_str());
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(request->dialog), "%s", detail.c_str());
  gestament_gtk_assign_accessible_id(
      request->dialog, accessible_id);
  for (const auto &[label, response] : buttons) {
    gtk_dialog_add_button(
        GTK_DIALOG(request->dialog), label.c_str(), response);
  }
  g_signal_connect(
      request->dialog, "response",
      G_CALLBACK(on_choice_dialog_response), request.get());
  request->cancellation_registration =
      cancellation.on_cancellation_requested(
          [weak = std::weak_ptr<SftpChoiceDialogRequest>(request)]() {
            g_idle_add(cancel_choice_dialog_idle,
                       new std::weak_ptr<SftpChoiceDialogRequest>(
                           weak));
          });
  gtk_widget_show_all(request->dialog);
  const int response = co_await request->source->get_promise();
  request->cancellation_registration = {};
  co_return response;
}

static cardio::promise<SftpConflictAction>
prompt_sftp_conflict_async(
    SftpWindow *window, const SftpTransferConflict &conflict,
    cardio::cancellation cancellation) {
  std::vector<std::pair<std::string, int>> buttons;
  buttons.emplace_back("Cancel", GTK_RESPONSE_CANCEL);
  buttons.emplace_back("Skip", GTK_RESPONSE_NO);
  buttons.emplace_back("Overwrite", GTK_RESPONSE_YES);
  auto response_promise = prompt_sftp_choice_async(
      window, GTK_MESSAGE_QUESTION, "Destination already exists",
      conflict.destination_path +
          "\nThe selected decision applies to all remaining conflicts.",
      std::move(buttons), "sftp_conflict_dialog", cancellation);
  const int response = co_await response_promise;
  if (response == GTK_RESPONSE_YES) {
    co_return SftpConflictAction::overwrite;
  }
  if (response == GTK_RESPONSE_NO) {
    co_return SftpConflictAction::skip;
  }
  co_return SftpConflictAction::cancel;
}

static cardio::promise<SftpFailureAction>
prompt_sftp_failure_async(
    SftpWindow *window, const SftpTransferFailure &failure,
    cardio::cancellation cancellation) {
  std::vector<std::pair<std::string, int>> buttons;
  buttons.emplace_back("Abort", GTK_RESPONSE_CANCEL);
  buttons.emplace_back("Skip", GTK_RESPONSE_NO);
  buttons.emplace_back("Retry", GTK_RESPONSE_YES);
  auto response_promise = prompt_sftp_choice_async(
      window, GTK_MESSAGE_ERROR, "SFTP transfer failed",
      failure.message + "\n\n" + failure.source_path + "\n→ " +
          failure.destination_path,
      std::move(buttons), "sftp_failure_dialog", cancellation);
  const int response = co_await response_promise;
  if (response == GTK_RESPONSE_YES) {
    co_return SftpFailureAction::retry;
  }
  if (response == GTK_RESPONSE_NO) {
    co_return SftpFailureAction::skip;
  }
  co_return SftpFailureAction::abort;
}

static void on_notice_response(GtkDialog *dialog, gint, gpointer) {
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void show_sftp_error(SftpWindow *window,
                            const std::string &message) {
  if (window == nullptr || window->destroyed) {
    return;
  }
  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(window->window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
      "SFTP operation failed");
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s", message.c_str());
  gestament_gtk_assign_accessible_id(
      dialog, "sftp_operation_error_dialog");
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

static cardio::promise<std::vector<SftpFileAttributes>>
list_sftp_pane_directory_async(
    SftpPaneState *pane, std::string path,
    cardio::cancellation cancellation) {
  if (pane->remote) {
    co_return co_await pane->window->client->list_directory_async(
        std::move(path), std::move(cancellation));
  }
  co_return co_await list_local_browser_directory_async(
      std::move(path), std::move(cancellation));
}

static cardio::promise<void> load_sftp_pane_root_async(
    SftpPaneState *pane, std::string requested_path) {
  SftpWindow *window = pane->window;
  const std::uint64_t generation = ++pane->generation;
  const cardio::cancellation cancellation =
      window->stop_source.get_cancellation();
  try {
    std::string path =
        pane->remote
            ? co_await window->client->canonicalize_path_async(
                  std::move(requested_path), cancellation)
            : canonical_local_directory(requested_path);
    std::vector<SftpFileAttributes> entries =
        co_await list_sftp_pane_directory_async(
            pane, path, cancellation);
    if (window->destroyed || generation != pane->generation) {
      pane->busy = false;
      co_return;
    }
    gtk_tree_store_clear(pane->store);
    append_browser_entries(pane->store, nullptr, std::move(entries));
    pane->current_directory = std::move(path);
    gtk_entry_set_text(GTK_ENTRY(pane->path_entry),
                       pane->current_directory.c_str());
  } catch (const cardio::canceled_exception &) {
  } catch (...) {
    if (!window->destroyed) {
      const std::string side = pane->remote ? "remote" : "local";
      set_sftp_status(
          window, "Failed to load " + side + " directory");
      show_sftp_error(window, exception_text(std::current_exception()));
      gtk_entry_set_text(GTK_ENTRY(pane->path_entry),
                         pane->current_directory.c_str());
    }
  }
  pane->busy = false;
}

static void start_sftp_pane_navigation(
    SftpPaneState *pane, std::string path) {
  if (pane == nullptr || pane->window == nullptr ||
      pane->window->destroyed || pane->busy ||
      (pane->remote &&
       !pane->window->connection_available)) {
    return;
  }
  pane->task.reset();
  pane->busy = true;
  pane->task.emplace(
      load_sftp_pane_root_async(pane, std::move(path)));
}

static cardio::promise<void> expand_sftp_directory_async(
    SftpPaneState *pane, GtkTreeRowReference *reference,
    std::string path, std::uint64_t generation) {
  SftpWindow *window = pane->window;
  const cardio::cancellation cancellation =
      window->stop_source.get_cancellation();
  try {
    std::vector<SftpFileAttributes> entries =
        co_await list_sftp_pane_directory_async(
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
          pane->store, &iterator, std::move(entries));
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
            pane->store, &iterator, sftp_tree_loaded_column,
            FALSE, -1);
        clear_tree_children(pane->store, &iterator);
        append_dummy_row(pane->store, &iterator);
        gtk_tree_view_collapse_row(
            GTK_TREE_VIEW(pane->tree), tree_path);
      }
      if (tree_path != nullptr) {
        gtk_tree_path_free(tree_path);
      }
      set_sftp_status(window, "Failed to expand directory");
      show_sftp_error(window, exception_text(std::current_exception()));
    }
  }
  gtk_tree_row_reference_free(reference);
  pane->busy = false;
}

static void on_sftp_row_expanded(
    GtkTreeView *, GtkTreeIter *iterator, GtkTreePath *tree_path,
    gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  if (pane == nullptr || pane->busy ||
      pane->window->destroyed ||
      (pane->remote &&
       !pane->window->connection_available)) {
    return;
  }
  gchar *path = nullptr;
  gboolean loaded = FALSE;
  gboolean dummy = FALSE;
  gint type = static_cast<gint>(SftpFileType::other);
  gtk_tree_model_get(
      GTK_TREE_MODEL(pane->store), iterator,
      sftp_tree_path_column, &path, sftp_tree_type_column, &type,
      sftp_tree_loaded_column, &loaded, sftp_tree_dummy_column,
      &dummy, -1);
  const std::string directory =
      path == nullptr ? std::string() : std::string(path);
  g_free(path);
  if (loaded != FALSE || dummy != FALSE ||
      type != static_cast<gint>(SftpFileType::directory) ||
      directory.empty()) {
    return;
  }

  gtk_tree_store_set(
      pane->store, iterator, sftp_tree_loaded_column, TRUE, -1);
  GtkTreeRowReference *reference =
      gtk_tree_row_reference_new(
          GTK_TREE_MODEL(pane->store), tree_path);
  if (reference == nullptr) {
    gtk_tree_store_set(
        pane->store, iterator, sftp_tree_loaded_column, FALSE, -1);
    return;
  }
  pane->task.reset();
  pane->busy = true;
  pane->task.emplace(expand_sftp_directory_async(
      pane, reference, directory, pane->generation));
}

static void on_sftp_path_activate(GtkEntry *entry, gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  const char *text = gtk_entry_get_text(entry);
  start_sftp_pane_navigation(
      pane, text == nullptr ? std::string() : std::string(text));
}

static void on_sftp_up_clicked(GtkButton *, gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  const std::string parent =
      pane->remote
          ? remote_parent_directory(pane->current_directory)
          : local_parent_directory(pane->current_directory);
  start_sftp_pane_navigation(pane, parent);
}

static void on_sftp_refresh_clicked(GtkButton *, gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  start_sftp_pane_navigation(pane, pane->current_directory);
}

static void on_sftp_row_activated(
    GtkTreeView *, GtkTreePath *path, GtkTreeViewColumn *,
    gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  GtkTreeIter iterator;
  if (!gtk_tree_model_get_iter(
          GTK_TREE_MODEL(pane->store), &iterator, path)) {
    return;
  }
  gchar *item_path = nullptr;
  gboolean dummy = FALSE;
  gint type = static_cast<gint>(SftpFileType::other);
  gtk_tree_model_get(
      GTK_TREE_MODEL(pane->store), &iterator,
      sftp_tree_path_column, &item_path,
      sftp_tree_type_column, &type, sftp_tree_dummy_column,
      &dummy, -1);
  const std::string directory =
      item_path == nullptr ? std::string() : std::string(item_path);
  g_free(item_path);
  if (dummy == FALSE &&
      type == static_cast<gint>(SftpFileType::directory) &&
      !directory.empty()) {
    start_sftp_pane_navigation(pane, directory);
  }
}

static std::vector<std::string>
selected_sftp_paths(SftpPaneState *pane) {
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(pane->tree));
  GtkTreeModel *model = nullptr;
  GList *rows =
      gtk_tree_selection_get_selected_rows(selection, &model);
  std::vector<std::string> paths;
  for (GList *item = rows; item != nullptr; item = item->next) {
    auto *tree_path = static_cast<GtkTreePath *>(item->data);
    GtkTreeIter iterator;
    if (!gtk_tree_model_get_iter(model, &iterator, tree_path)) {
      continue;
    }
    gchar *path = nullptr;
    gboolean dummy = FALSE;
    gtk_tree_model_get(
        model, &iterator, sftp_tree_path_column, &path,
        sftp_tree_dummy_column, &dummy, -1);
    if (dummy == FALSE && path != nullptr && path[0] != '\0') {
      paths.emplace_back(path);
    }
    g_free(path);
  }
  g_list_free_full(rows,
                   reinterpret_cast<GDestroyNotify>(
                       gtk_tree_path_free));
  return paths;
}

static gboolean on_sftp_tree_button_press(
    GtkWidget *widget, GdkEventButton *event, gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  if (event->type != GDK_BUTTON_PRESS || event->button != 3 ||
      pane == nullptr || pane->window->transfer_active ||
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
  const std::vector<std::string> paths =
      selected_sftp_paths(pane);
  if (paths.empty()) {
    return FALSE;
  }
  gtk_menu_popup_at_pointer(GTK_MENU(pane->menu),
                            reinterpret_cast<GdkEvent *>(event));
  return TRUE;
}

static void refresh_sftp_panes(SftpWindow *window) {
  start_sftp_pane_navigation(
      &window->local, window->local.current_directory);
  if (window->connection_available) {
    start_sftp_pane_navigation(
        &window->remote, window->remote.current_directory);
  }
}

static cardio::promise<void> run_sftp_window_transfer_async(
    SftpWindow *window, SftpTransferDirection direction,
    std::vector<std::string> sources,
    std::string destination) {
  const std::size_t selected_count = sources.size();
  const cardio::cancellation cancellation =
      window->transfer_cancel_source->get_cancellation();
  bool succeeded = false;
  try {
    co_await run_sftp_transfer_async(
        window->client,
        {
            .direction = direction,
            .source_paths = std::move(sources),
            .destination_directory = std::move(destination),
            .callbacks =
                {
                    .conflict =
                        [window](
                            const SftpTransferConflict &conflict,
                            cardio::cancellation callback_cancellation) {
                          return prompt_sftp_conflict_async(
                              window, conflict,
                              std::move(callback_cancellation));
                        },
                    .failure =
                        [window](
                            const SftpTransferFailure &failure,
                            cardio::cancellation callback_cancellation) {
                          return prompt_sftp_failure_async(
                              window, failure,
                              std::move(callback_cancellation));
                        },
                    .progress =
                        [window](
                            const SftpTransferProgress &progress) {
                          update_sftp_transfer_progress(window,
                                                        progress);
                        },
                },
        },
        cancellation);
    succeeded = true;
  } catch (const cardio::canceled_exception &) {
    if (!window->destroyed) {
      set_sftp_status(window, "Transfer cancelled");
    }
  } catch (...) {
    if (!window->destroyed) {
      const std::string message =
          exception_text(std::current_exception());
      if (message.find("canceled") != std::string::npos ||
          message.find("cancelled") != std::string::npos) {
        set_sftp_status(window, "Transfer cancelled");
      } else {
        set_sftp_status(window, "Transfer failed");
        show_sftp_error(window, message);
      }
    }
  }

  if (window->destroyed) {
    co_return;
  }
  window->transfer_cancel_source.reset();
  set_sftp_transfer_active(window, false);
  if (succeeded) {
    const char *verb =
        direction == SftpTransferDirection::send ? "Sent" : "Received";
    set_sftp_status(
        window, std::string(verb) + " " +
                    std::to_string(selected_count) +
                    (selected_count == 1 ? " item" : " items"));
  }
  refresh_sftp_panes(window);
}

static void start_sftp_transfer(
    SftpWindow *window, SftpTransferDirection direction) {
  if (window == nullptr || window->destroyed ||
      window->transfer_active || !window->connection_available) {
    return;
  }
  SftpPaneState *source =
      direction == SftpTransferDirection::send
          ? &window->local
          : &window->remote;
  const std::vector<std::string> sources =
      selected_sftp_paths(source);
  if (sources.empty()) {
    return;
  }
  const std::string destination =
      direction == SftpTransferDirection::send
          ? window->remote.current_directory
          : window->local.current_directory;
  if (destination.empty()) {
    return;
  }

  window->transfer_task.reset();
  window->transfer_cancel_source.emplace();
  set_sftp_transfer_active(window, true);
  window->transfer_task.emplace(
      run_sftp_window_transfer_async(
          window, direction, sources, destination));
}

static void on_sftp_transfer_item_activate(
    GtkMenuItem *, gpointer data) {
  auto *pane = static_cast<SftpPaneState *>(data);
  start_sftp_transfer(
      pane->window,
      pane->remote ? SftpTransferDirection::receive
                   : SftpTransferDirection::send);
}

static void on_sftp_transfer_cancel_clicked(
    GtkButton *, gpointer data) {
  auto *window = static_cast<SftpWindow *>(data);
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

static void on_sftp_window_destroy(GtkWidget *, gpointer data) {
  auto *window = static_cast<SftpWindow *>(data);
  if (window == nullptr || window->destroyed) {
    return;
  }
  clear_sftp_window_colors(window);
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
  stop_sftp_transfer_pulse(window);
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

static int get_sftp_tree_row_height(GtkWidget *tree) {
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  g_object_ref_sink(renderer);
  g_object_set(renderer, "text", "Ag", nullptr);
  int minimum_height = 0;
  int natural_height = 0;
  gtk_cell_renderer_get_preferred_height(
      renderer, tree, &minimum_height, &natural_height);
  g_object_unref(renderer);

  int expander_size = 0;
  int horizontal_separator = 0;
  gtk_widget_style_get(
      tree, "expander-size", &expander_size,
      "horizontal-separator", &horizontal_separator, nullptr);
  const int effective_expander_size =
      expander_size + horizontal_separator / 2;
  return std::max(natural_height, effective_expander_size);
}

static void append_sftp_tree_column(
    GtkWidget *tree, const char *title, int model_column,
    bool expand, int row_height) {
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_cell_renderer_set_fixed_size(renderer, -1, row_height);
  GtkTreeViewColumn *column =
      gtk_tree_view_column_new_with_attributes(
          title, renderer, "text", model_column, nullptr);
  gtk_tree_view_column_set_sizing(
      column, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_expand(column, expand ? TRUE : FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
}

static GtkWidget *create_sftp_tree(SftpPaneState *pane,
                                   const char *accessible_id) {
  pane->store = gtk_tree_store_new(
      sftp_tree_column_count, G_TYPE_STRING, G_TYPE_STRING,
      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_BOOLEAN,
      G_TYPE_BOOLEAN);
  GtkWidget *tree = gtk_tree_view_new_with_model(
      GTK_TREE_MODEL(pane->store));
  gestament_gtk_assign_accessible_id(tree, accessible_id);
  gtk_tree_view_set_enable_tree_lines(GTK_TREE_VIEW(tree), TRUE);
  gtk_tree_view_set_rubber_banding(GTK_TREE_VIEW(tree), TRUE);
  gtk_tree_view_set_search_column(
      GTK_TREE_VIEW(tree), sftp_tree_name_column);
  const int row_height = get_sftp_tree_row_height(tree);
  append_sftp_tree_column(
      tree, "Name", sftp_tree_name_column, true, row_height);
  append_sftp_tree_column(
      tree, "Size", sftp_tree_size_column, false, row_height);
  append_sftp_tree_column(
      tree, "Modified", sftp_tree_modified_column, false,
      row_height);
  gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(tree), TRUE);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(
      GTK_TREE_VIEW(tree));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  g_signal_connect(tree, "row-expanded",
                   G_CALLBACK(on_sftp_row_expanded), pane);
  g_signal_connect(tree, "row-activated",
                   G_CALLBACK(on_sftp_row_activated), pane);
  g_signal_connect(tree, "button-press-event",
                   G_CALLBACK(on_sftp_tree_button_press), pane);
  return tree;
}

static GtkWidget *create_sftp_pane(
    SftpWindow *window, SftpPaneState *pane, bool remote) {
  pane->window = window;
  pane->remote = remote;
  pane->frame = gtk_frame_new(remote ? "Remote" : "Local");
  gestament_gtk_assign_accessible_id(
      pane->frame,
      remote ? "sftp_remote_group" : "sftp_local_group");
  gtk_widget_set_hexpand(pane->frame, TRUE);
  gtk_widget_set_vexpand(pane->frame, TRUE);
  gtk_widget_set_margin_start(
      pane->frame,
      remote ? sftp_pane_spacing / 2 : sftp_content_padding);
  gtk_widget_set_margin_end(
      pane->frame,
      remote ? sftp_content_padding : sftp_pane_spacing / 2);
  gtk_widget_set_margin_top(pane->frame, sftp_content_padding);
  gtk_widget_set_margin_bottom(pane->frame, sftp_content_padding);

  GtkWidget *box =
      gtk_box_new(GTK_ORIENTATION_VERTICAL, sftp_control_spacing);
  gtk_container_set_border_width(
      GTK_CONTAINER(box), sftp_content_padding);
  gtk_container_add(GTK_CONTAINER(pane->frame), box);

  GtkWidget *toolbar =
      gtk_box_new(GTK_ORIENTATION_HORIZONTAL, sftp_control_spacing);
  gtk_box_pack_start(GTK_BOX(box), toolbar, FALSE, TRUE, 0);
  pane->path_entry = gtk_entry_new();
  gestament_gtk_assign_accessible_id(
      pane->path_entry,
      remote ? "sftp_remote_path_entry"
             : "sftp_local_path_entry");
  gtk_widget_set_hexpand(pane->path_entry, TRUE);
  gtk_box_pack_start(GTK_BOX(toolbar), pane->path_entry, TRUE, TRUE, 0);
  pane->up_button = create_toolbar_button(
      "Up", remote ? "sftp_remote_up_button"
                    : "sftp_local_up_button");
  gtk_box_pack_start(GTK_BOX(toolbar), pane->up_button, FALSE, TRUE, 0);
  pane->refresh_button = create_toolbar_button(
      "Refresh", remote ? "sftp_remote_refresh_button"
                         : "sftp_local_refresh_button");
  gtk_box_pack_start(
      GTK_BOX(toolbar), pane->refresh_button, FALSE, TRUE, 0);

  GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(
      GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
      GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
  pane->tree = create_sftp_tree(
      pane, remote ? "sftp_remote_tree" : "sftp_local_tree");
  gtk_container_add(GTK_CONTAINER(scroller), pane->tree);

  pane->menu = gtk_menu_new();
  gtk_menu_attach_to_widget(
      GTK_MENU(pane->menu), pane->tree, nullptr);
  pane->transfer_item = gtk_menu_item_new_with_label(
      remote ? "Receive" : "Send");
  gestament_gtk_assign_accessible_id(
      pane->transfer_item,
      remote ? "sftp_receive_item" : "sftp_send_item");
  gtk_menu_shell_append(GTK_MENU_SHELL(pane->menu),
                        pane->transfer_item);
  g_signal_connect(
      pane->transfer_item, "activate",
      G_CALLBACK(on_sftp_transfer_item_activate), pane);
  gtk_widget_show_all(pane->menu);

  g_signal_connect(
      pane->path_entry, "activate",
      G_CALLBACK(on_sftp_path_activate), pane);
  g_signal_connect(
      pane->up_button, "clicked",
      G_CALLBACK(on_sftp_up_clicked), pane);
  g_signal_connect(
      pane->refresh_button, "clicked",
      G_CALLBACK(on_sftp_refresh_clicked), pane);
  return pane->frame;
}

static void apply_sftp_window_style(SftpWindow *window) {
  static constexpr char css[] =
      ".sftp-dim { background-color: rgba(0, 0, 0, 0.42); }"
      ".sftp-progress { background-color: @theme_bg_color; "
      "padding: 10px; }";
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, css, -1, nullptr);
  GtkStyleContext *dim_context =
      gtk_widget_get_style_context(window->dim_overlay);
  gtk_style_context_add_class(dim_context, "sftp-dim");
  gtk_style_context_add_provider(
      dim_context, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  GtkStyleContext *progress_context =
      gtk_widget_get_style_context(window->transfer_overlay);
  gtk_style_context_add_class(progress_context, "sftp-progress");
  gtk_style_context_add_provider(
      progress_context, GTK_STYLE_PROVIDER(provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

static void clear_sftp_window_colors(SftpWindow *window) {
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
      sftp_exterior_component_style_class);
  gtk_style_context_remove_class(
      gtk_widget_get_style_context(window->status_bar),
      sftp_exterior_component_style_class);
  if (window->background_provider != nullptr) {
    remove_widget_tree_background_provider(
        window->paned, window->background_provider);
    remove_widget_tree_background_provider(
        window->transfer_overlay,
        window->background_provider);
    g_clear_object(&window->background_provider);
  }
  if (window->component_background_provider != nullptr) {
    remove_widget_tree_background_provider(
        window->paned, window->component_background_provider);
    remove_widget_tree_background_provider(
        window->transfer_overlay,
        window->component_background_provider);
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

std::shared_ptr<SftpWindow>
create_sftp_window(SftpWindowOptions options) {
  if (options.client == nullptr) {
    throw std::invalid_argument("SFTP client is required");
  }
  auto state = std::make_shared<SftpWindow>();
  state->client = std::move(options.client);
  state->closed = std::move(options.closed);
  state->local.current_directory =
      std::move(options.local_directory);
  state->remote.current_directory =
      std::move(options.remote_directory);

  state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gestament_gtk_assign_accessible_id(
      state->window, "sftp_window");
  const std::string title =
      options.connection_name.empty()
          ? "SFTP"
          : options.connection_name + " — SFTP";
  gtk_window_set_title(GTK_WINDOW(state->window), title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(state->window), 1040, 640);

  state->header_bar = gtk_header_bar_new();
  gestament_gtk_assign_accessible_id(
      state->header_bar, "sftp_header_bar");
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
      state->paned, "sftp_root_paned");
  gtk_widget_set_hexpand(state->paned, TRUE);
  gtk_widget_set_vexpand(state->paned, TRUE);
  gtk_box_pack_start(GTK_BOX(content), state->paned, TRUE, TRUE, 0);
  GtkWidget *local =
      create_sftp_pane(state.get(), &state->local, false);
  GtkWidget *remote =
      create_sftp_pane(state.get(), &state->remote, true);
  gtk_paned_pack1(GTK_PANED(state->paned), local, TRUE, FALSE);
  gtk_paned_pack2(GTK_PANED(state->paned), remote, TRUE, FALSE);
  gtk_paned_set_position(GTK_PANED(state->paned), 520);

  state->status_bar = gtk_event_box_new();
  gtk_event_box_set_visible_window(
      GTK_EVENT_BOX(state->status_bar), TRUE);
  gestament_gtk_assign_accessible_id(
      state->status_bar, "sftp_status_bar");
  GtkWidget *status_content =
      gtk_box_new(
          GTK_ORIENTATION_HORIZONTAL, sftp_control_spacing);
  gtk_widget_set_margin_start(
      status_content, sftp_content_padding);
  gtk_widget_set_margin_end(
      status_content, sftp_content_padding);
  gtk_widget_set_margin_top(
      status_content, sftp_control_spacing);
  gtk_widget_set_margin_bottom(
      status_content, sftp_control_spacing);
  gtk_container_add(
      GTK_CONTAINER(state->status_bar), status_content);
  gtk_box_pack_start(
      GTK_BOX(content), state->status_bar, FALSE, TRUE, 0);
  state->status_label = gtk_label_new("Ready");
  gestament_gtk_assign_accessible_id(
      state->status_label, "sftp_status_label");
  gtk_label_set_xalign(GTK_LABEL(state->status_label), 0.0F);
  gtk_box_pack_start(
      GTK_BOX(status_content), state->status_label, TRUE, TRUE, 0);

  state->dim_overlay = gtk_event_box_new();
  gestament_gtk_assign_accessible_id(
      state->dim_overlay, "sftp_dim_overlay");
  gtk_widget_set_no_show_all(state->dim_overlay, TRUE);
  gtk_widget_set_visible(state->dim_overlay, FALSE);
  gtk_widget_set_halign(state->dim_overlay, GTK_ALIGN_FILL);
  gtk_widget_set_valign(state->dim_overlay, GTK_ALIGN_FILL);
  gtk_overlay_add_overlay(GTK_OVERLAY(state->root_overlay),
                          state->dim_overlay);

  state->transfer_overlay =
      gtk_frame_new(nullptr);
  gestament_gtk_assign_accessible_id(
      state->transfer_overlay, "sftp_transfer_overlay");
  gtk_widget_set_no_show_all(state->transfer_overlay, TRUE);
  gtk_widget_set_visible(state->transfer_overlay, FALSE);
  gtk_widget_set_halign(state->transfer_overlay, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(state->transfer_overlay, GTK_ALIGN_START);
  gtk_widget_set_margin_top(state->transfer_overlay, 18);
  GtkWidget *transfer_box =
      gtk_box_new(
          GTK_ORIENTATION_VERTICAL, sftp_control_spacing);
  gtk_container_set_border_width(
      GTK_CONTAINER(transfer_box), sftp_content_padding);
  gtk_container_add(GTK_CONTAINER(state->transfer_overlay),
                    transfer_box);
  state->transfer_label = gtk_label_new("Preparing transfer…");
  gtk_label_set_xalign(GTK_LABEL(state->transfer_label), 0.0F);
  gtk_box_pack_start(
      GTK_BOX(transfer_box), state->transfer_label, FALSE, TRUE, 0);
  state->transfer_progress = gtk_progress_bar_new();
  gestament_gtk_assign_accessible_id(
      state->transfer_progress, "sftp_transfer_progress");
  gtk_widget_set_size_request(state->transfer_progress, 280, -1);
  gtk_box_pack_start(
      GTK_BOX(transfer_box), state->transfer_progress, FALSE, TRUE, 0);
  state->transfer_cancel_button =
      gtk_button_new_with_label("Cancel");
  gestament_gtk_assign_accessible_id(
      state->transfer_cancel_button,
      "sftp_transfer_cancel_button");
  gtk_widget_set_halign(
      state->transfer_cancel_button, GTK_ALIGN_END);
  gtk_box_pack_start(
      GTK_BOX(transfer_box), state->transfer_cancel_button,
      FALSE, TRUE, 0);
  gtk_overlay_add_overlay(GTK_OVERLAY(state->root_overlay),
                          state->transfer_overlay);

  apply_sftp_window_style(state.get());
  set_sftp_window_colors(state, options.colors);
  g_signal_connect(
      state->transfer_cancel_button, "clicked",
      G_CALLBACK(on_sftp_transfer_cancel_clicked), state.get());
  g_signal_connect(state->window, "destroy",
                   G_CALLBACK(on_sftp_window_destroy), state.get());
  update_sftp_sensitivity(state.get());
  return state;
}

GtkWidget *sftp_window_widget(
    const std::shared_ptr<SftpWindow> &window) noexcept {
  return window == nullptr ? nullptr : window->window;
}

void show_sftp_window(const std::shared_ptr<SftpWindow> &window) {
  if (window == nullptr || window->window == nullptr ||
      window->destroyed) {
    return;
  }
  gtk_widget_show_all(window->window);
  set_widget_visible(window->dim_overlay, false);
  set_widget_visible(window->transfer_overlay, false);
  if (!window->initial_load_started) {
    window->initial_load_started = true;
    start_sftp_pane_navigation(
        &window->local, window->local.current_directory);
    start_sftp_pane_navigation(
        &window->remote, window->remote.current_directory);
  }
}

void set_sftp_window_connection_available(
    const std::shared_ptr<SftpWindow> &window, bool available) {
  if (window == nullptr || window->destroyed ||
      window->connection_available == available) {
    return;
  }
  window->connection_available = available;
  if (!available) {
    if (window->transfer_cancel_source.has_value()) {
      (void)window->transfer_cancel_source->cancel();
    }
    set_sftp_status(window.get(), "Disconnected");
  }
  update_sftp_sensitivity(window.get());
}

void set_sftp_window_colors(
    const std::shared_ptr<SftpWindow> &window,
    const GeneralColorSettings &settings) {
  if (window == nullptr || window->destroyed) {
    return;
  }

  clear_sftp_window_colors(window.get());
  if (settings.exterior_background.has_value()) {
    window->exterior_background_provider =
        create_widget_background_provider(
            settings.exterior_background.value(),
            "SFTP exterior");
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
            sftp_exterior_component_style_class,
            "SFTP exterior controls");
    GdkScreen *screen = gtk_widget_get_screen(window->window);
    if (window->exterior_component_background_provider != nullptr &&
        screen != nullptr) {
      gtk_style_context_add_class(
          gtk_widget_get_style_context(window->header_bar),
          sftp_exterior_component_style_class);
      gtk_style_context_add_class(
          gtk_widget_get_style_context(window->status_bar),
          sftp_exterior_component_style_class);
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
            settings.background.value(), "SFTP browser");
    add_widget_tree_background_provider(
        window->paned, window->background_provider);
    add_widget_tree_background_provider_at_priority(
        window->transfer_overlay,
        window->background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    window->component_background_provider =
        create_widget_component_background_provider(
            settings.background.value(), "SFTP controls");
    add_widget_tree_background_provider_at_priority(
        window->paned, window->component_background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    add_widget_tree_background_provider_at_priority(
        window->transfer_overlay,
        window->component_background_provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    window->popup_component_background_provider =
        create_widget_popup_component_background_provider(
            settings.background.value(), "SFTP popups");
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

void present_sftp_window(
    const std::shared_ptr<SftpWindow> &window) {
  if (window == nullptr || window->window == nullptr ||
      window->destroyed) {
    return;
  }
  gtk_window_present(GTK_WINDOW(window->window));
}

} // namespace elder_terms
