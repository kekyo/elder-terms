// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#include "file-transfer-job-view.h"
#include <elder-terms/http-url.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <gestament/gtk.h>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

struct JobRow {
  std::shared_ptr<FileTransferJob> job;
  GtkWidget *root;
  GtkWidget *expander;
  GtkWidget *state;
  GtkWidget *progress;
  GtkWidget *cancel;
  GtkWidget *retry;
  GtkWidget *details;
  std::function<void(const FileTransferJobSnapshot &)> retry_action;
  bool pulse = false;
};

struct FileTransferJobView {
  GtkWidget *root;
  GtkWidget *expander;
  GtkWidget *content;
  GtkWidget *summary;
  GtkWidget *rows;
  GtkWidget *clear;
  std::map<std::uint64_t, std::unique_ptr<JobRow>> entries;
  std::function<void(const FileTransferJobSnapshot &)> retry;
  std::function<void()> clear_action;
  std::function<void(GtkWidget *)> style;
  guint pulse_source = 0;
  bool seen_job = false;
  bool destroyed = false;
};

template<typename... Args>
static std::string format_job_text(const char *format, Args... args) {
  auto *text = g_strdup_printf(format, args...);
  std::string result = text;
  g_free(text);
  return result;
}

static const char *job_state_text(FileTransferJobState state) {
  switch (state) {
  case FileTransferJobState::queued: return _("Waiting");
  case FileTransferJobState::discovering: return _("Discovering…");
  case FileTransferJobState::transferring: return _("Transferring");
  case FileTransferJobState::awaiting_decision: return _("Awaiting decision");
  case FileTransferJobState::cancelling: return _("Cancelling…");
  case FileTransferJobState::completed: return _("Completed");
  case FileTransferJobState::failed: return _("Failed");
  case FileTransferJobState::cancelled: return _("Cancelled");
  }
  return "";
}

static std::string job_progress_text(const FileTransferJobSnapshot &job) {
  const auto &p = job.progress;
  if (job.result) return format_job_text(_("%llu copied, %llu skipped"),
      static_cast<unsigned long long>(job.result->succeeded_items),
      static_cast<unsigned long long>(job.result->skipped_items));
  if (job.state == FileTransferJobState::queued) return _("Waiting");
  if (!p.discovery_complete) {
    if (!p.transferred_bytes) return _("Total unknown");
    return format_job_text(_("%llu bytes · %s"),
        static_cast<unsigned long long>(p.transferred_bytes), _("Total unknown"));
  }
  const auto count = format_job_text(_("%llu / %llu items"),
      static_cast<unsigned long long>(p.completed_items + p.skipped_items),
      static_cast<unsigned long long>(p.total_items));
  if (p.total_bytes && *p.total_bytes > 0)
    return format_job_text("%.0f%% · %s", std::min(100.0, 100.0 * p.transferred_bytes / *p.total_bytes), count.c_str());
  return format_job_text(_("%llu bytes · %s"),
      static_cast<unsigned long long>(p.transferred_bytes), count.c_str());
}

static std::string job_time_text(std::chrono::system_clock::time_point time) {
  const auto seconds = std::chrono::system_clock::to_time_t(time);
  auto *date = g_date_time_new_from_unix_local(seconds);
  if (!date) return _("Unknown");
  auto *text = g_date_time_format(date, "%Y-%m-%d %H:%M:%S");
  std::string result = text ? text : _("Unknown");
  g_free(text);
  g_date_time_unref(date);
  return result;
}

static std::string job_details_text(const FileTransferJobSnapshot &job) {
  auto text = format_job_text(_("Protocol: %s\nDestination: %s"), job.protocol.c_str(),
      file_transfer_job_path_label(job, job.destination_directory).c_str());
  for (const auto &source : job.source_paths) text += "\n" + file_transfer_job_path_label(job, source);
  if (job.started_at) text += "\n" + format_job_text(_("Started: %s"), job_time_text(*job.started_at).c_str());
  if (job.finished_at) text += "\n" + format_job_text(_("Finished: %s"), job_time_text(*job.finished_at).c_str());
  if (job.result) {
    const auto &result = *job.result;
    const auto remaining = result.unprocessed_items ? std::to_string(*result.unprocessed_items) : _("Unknown");
    text += "\n" + format_job_text(_("Copied: %llu; skipped: %llu; failed: %llu; unprocessed: %s"),
        static_cast<unsigned long long>(result.succeeded_items), static_cast<unsigned long long>(result.skipped_items),
        static_cast<unsigned long long>(result.failed_items), remaining.c_str());
    for (const auto &failure : result.failures)
      text += "\n" + file_transfer_job_path_label(job, failure.source_path) + "\n" + failure.message;
    for (const auto &path : result.temporary_paths)
      text += "\n" + format_job_text(_("Temporary file remaining: %s"), file_transfer_job_path_label(job, path).c_str());
  }
  return text;
}

static std::unique_ptr<JobRow> create_job_row(FileTransferJobView *view, const std::shared_ptr<FileTransferJob> &job) {
  auto row = std::make_unique<JobRow>();
  row->job = job;
  row->retry_action = view->retry;
  const auto &snapshot = file_transfer_job_snapshot(job);
  const auto prefix = "file_transfer_job_" + std::to_string(snapshot.id);
  row->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(row->root), header, FALSE, TRUE, 0);
  auto name = snapshot.source_paths.empty() ? std::string() :
      std::filesystem::path(file_transfer_job_path_label(snapshot, snapshot.source_paths.front())).filename().string();
  if (!snapshot.source_paths.empty() &&
      (snapshot.source_paths.front().starts_with("http://") || snapshot.source_paths.front().starts_with("https://")))
    name = http_resource_name(snapshot.source_paths.front());
  if (snapshot.source_paths.size() > 1) name += " (+" + std::to_string(snapshot.source_paths.size() - 1) + ")";
  name = std::string(snapshot.direction == FileTransferDirection::send ? _("Send") : _("Receive")) + " · " + name;
  auto *title = gtk_label_new(name.c_str());
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(GTK_LABEL(title), 28);
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  row->expander = gtk_expander_new(nullptr);
  gestament_gtk_assign_accessible_id(row->expander, (prefix + "_row").c_str());
  gtk_expander_set_label_widget(GTK_EXPANDER(row->expander), title);
  gtk_expander_set_label_fill(GTK_EXPANDER(row->expander), TRUE);
  gtk_box_pack_start(GTK_BOX(header), row->expander, TRUE, TRUE, 0);
  row->state = gtk_label_new("");
  gestament_gtk_assign_accessible_id(row->state, (prefix + "_state").c_str());
  gtk_box_pack_start(GTK_BOX(header), row->state, FALSE, TRUE, 0);
  row->progress = gtk_progress_bar_new();
  gestament_gtk_assign_accessible_id(row->progress, (prefix + "_progress").c_str());
  gtk_widget_set_size_request(row->progress, 180, -1);
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(row->progress), TRUE);
  gtk_box_pack_start(GTK_BOX(header), row->progress, FALSE, TRUE, 0);
  row->cancel = gtk_button_new_with_label(_("Cancel"));
  gestament_gtk_assign_accessible_id(row->cancel, (prefix + "_cancel").c_str());
  gtk_box_pack_start(GTK_BOX(header), row->cancel, FALSE, FALSE, 0);
  g_signal_connect(row->cancel, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    cancel_file_transfer_job(static_cast<JobRow *>(data)->job);
  }), row.get());
  row->retry = gtk_button_new_with_label(_("Retry as new job"));
  gestament_gtk_assign_accessible_id(row->retry, (prefix + "_retry").c_str());
  gtk_box_pack_start(GTK_BOX(header), row->retry, FALSE, FALSE, 0);
  g_signal_connect(row->retry, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *row = static_cast<JobRow *>(data);
    if (row->retry_action) row->retry_action(file_transfer_job_snapshot(row->job));
  }), row.get());
  row->details = gtk_label_new("");
  gestament_gtk_assign_accessible_id(row->details, (prefix + "_details").c_str());
  gtk_label_set_xalign(GTK_LABEL(row->details), 0);
  gtk_label_set_selectable(GTK_LABEL(row->details), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(row->details), TRUE);
  gtk_label_set_line_wrap_mode(GTK_LABEL(row->details), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(row->details), 80);
  gtk_widget_set_margin_start(row->details, 20);
  gtk_box_pack_start(GTK_BOX(row->root), row->details, FALSE, TRUE, 0);
  gtk_widget_set_no_show_all(row->details, TRUE);
  g_signal_connect(row->expander, "notify::expanded", G_CALLBACK(+[](GtkExpander *expander, GParamSpec *, gpointer data) {
    gtk_widget_set_visible(static_cast<JobRow *>(data)->details, gtk_expander_get_expanded(expander));
  }), row.get());
  gtk_box_pack_start(GTK_BOX(view->rows), row->root, FALSE, TRUE, 0);
  if (view->style) view->style(row->root);
  gtk_widget_show_all(row->root);
  return row;
}

std::shared_ptr<FileTransferJobView> create_file_transfer_job_view(
    std::function<void(const FileTransferJobSnapshot &)> retry, std::function<void()> clear,
    std::function<void(GtkWidget *)> style) {
  auto view = std::make_shared<FileTransferJobView>();
  view->retry = std::move(retry);
  view->clear_action = std::move(clear);
  view->style = std::move(style);
  view->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gestament_gtk_assign_accessible_id(view->root, "file_transfer_jobs");
  auto *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(view->root), heading, FALSE, TRUE, 0);
  view->expander = gtk_expander_new(_("Transfers"));
  gestament_gtk_assign_accessible_id(view->expander, "file_transfer_jobs_expander");
  gtk_box_pack_start(GTK_BOX(heading), view->expander, FALSE, TRUE, 0);
  gtk_widget_set_margin_start(view->root, 12);
  gtk_widget_set_margin_end(view->root, 12);
  view->summary = gtk_label_new(_("No transfers"));
  gestament_gtk_assign_accessible_id(view->summary, "file_transfer_jobs_summary");
  gtk_label_set_ellipsize(GTK_LABEL(view->summary), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(view->summary), 0);
  gtk_box_pack_start(GTK_BOX(heading), view->summary, TRUE, TRUE, 0);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  view->content = content;
  gtk_box_pack_start(GTK_BOX(view->root), content, TRUE, TRUE, 0);
  gtk_widget_set_no_show_all(content, TRUE);
  g_signal_connect(view->expander, "notify::expanded", G_CALLBACK(+[](GtkExpander *expander, GParamSpec *, gpointer data) {
    auto *view = static_cast<FileTransferJobView *>(data);
    const bool expanded = gtk_expander_get_expanded(expander);
    gtk_widget_set_no_show_all(view->content, !expanded);
    gtk_widget_set_visible(view->content, expanded);
    if (expanded) gtk_widget_show_all(view->content);
  }), view.get());
  auto *scroll = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroll, -1, 90);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
  view->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_container_add(GTK_CONTAINER(scroll), view->rows);
  view->clear = gtk_button_new_with_label(_("Clear finished jobs"));
  gestament_gtk_assign_accessible_id(view->clear, "file_transfer_jobs_clear");
  gtk_widget_set_halign(view->clear, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(content), view->clear, FALSE, FALSE, 0);
  g_signal_connect(view->clear, "clicked", G_CALLBACK(+[](GtkButton *, gpointer data) {
    auto *view = static_cast<FileTransferJobView *>(data);
    if (view->clear_action) view->clear_action();
  }), view.get());
  g_object_set_data_full(G_OBJECT(view->root), "job-view-owner", new std::shared_ptr(view),
      +[](gpointer data) { delete static_cast<std::shared_ptr<FileTransferJobView> *>(data); });
  g_signal_connect(view->root, "destroy", G_CALLBACK(+[](GtkWidget *, gpointer data) {
    auto *view = static_cast<FileTransferJobView *>(data);
    view->destroyed = true;
    if (view->pulse_source) { g_source_remove(view->pulse_source); view->pulse_source = 0; }
  }), view.get());
  gtk_widget_show_all(content);
  gtk_widget_hide(content);
  return view;
}

GtkWidget *file_transfer_job_view_widget(const std::shared_ptr<FileTransferJobView> &view) { return view->root; }
GtkWidget *file_transfer_job_view_expander(const std::shared_ptr<FileTransferJobView> &view) { return view->expander; }

static int job_rank(FileTransferJobState state) {
  if (state == FileTransferJobState::awaiting_decision) return 0;
  if (!file_transfer_job_finished(state) && state != FileTransferJobState::queued) return 1;
  if (state == FileTransferJobState::failed) return 2;
  return state == FileTransferJobState::queued ? 3 : 4;
}

void update_file_transfer_job_view(const std::shared_ptr<FileTransferJobView> &view,
    const std::vector<std::shared_ptr<FileTransferJob>> &jobs, bool connected) {
  if (!view || view->destroyed) return;
  for (auto it = view->entries.begin(); it != view->entries.end();) {
    if (std::none_of(jobs.begin(), jobs.end(), [&](const auto &job) { return file_transfer_job_snapshot(job).id == it->first; })) {
      gtk_widget_destroy(it->second->root);
      it = view->entries.erase(it);
    } else ++it;
  }
  auto ordered = jobs;
  std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
    return job_rank(file_transfer_job_snapshot(a).state) < job_rank(file_transfer_job_snapshot(b).state);
  });
  std::size_t active = 0, waiting = 0, finished = 0;
  bool pulse = false;
  std::string current;
  int index = 0;
  for (const auto &job : ordered) {
    const auto &snapshot = file_transfer_job_snapshot(job);
    auto &row = view->entries[snapshot.id];
    if (!row) row = create_job_row(view.get(), job);
    gtk_box_reorder_child(GTK_BOX(view->rows), row->root, index++);
    const bool done = file_transfer_job_finished(snapshot.state);
    if (done) ++finished;
    else if (snapshot.state == FileTransferJobState::queued) ++waiting;
    else { ++active; current = std::string(job_state_text(snapshot.state)) + " · " + job_progress_text(snapshot); }
    gtk_label_set_text(GTK_LABEL(row->state), job_state_text(snapshot.state));
    gtk_label_set_text(GTK_LABEL(row->details), job_details_text(snapshot).c_str());
    gtk_widget_set_sensitive(row->cancel, !done && snapshot.state != FileTransferJobState::cancelling);
    const bool retryable = snapshot.state == FileTransferJobState::failed || snapshot.state == FileTransferJobState::cancelled;
    gtk_widget_set_no_show_all(row->retry, !retryable);
    gtk_widget_set_visible(row->retry, retryable);
    gtk_widget_set_sensitive(row->retry, connected);
    const auto &p = snapshot.progress;
    row->pulse = !done && snapshot.state != FileTransferJobState::queued &&
        (!p.discovery_complete || !p.total_bytes);
    pulse |= row->pulse;
    const double fraction = done ? (snapshot.state == FileTransferJobState::completed && snapshot.result->skipped_items == 0 ? 1.0 : 0.0) :
        p.discovery_complete && p.total_bytes && *p.total_bytes ? std::min(1.0, double(p.transferred_bytes) / *p.total_bytes) : 0.0;
    if (!row->pulse) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(row->progress), fraction);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(row->progress), job_progress_text(snapshot).c_str());
  }
  auto summary = format_job_text(_("%zu active, %zu waiting, %zu finished"), active, waiting, finished);
  if (!current.empty()) summary += " · " + current;
  gtk_label_set_text(GTK_LABEL(view->summary), summary.c_str());
  gtk_widget_set_sensitive(view->clear, finished != 0);
  if (!view->seen_job && !jobs.empty()) {
    view->seen_job = true;
    gtk_expander_set_expanded(GTK_EXPANDER(view->expander), TRUE);
  }
  if (!pulse && view->pulse_source) { g_source_remove(view->pulse_source); view->pulse_source = 0; }
  if (pulse && !view->pulse_source) view->pulse_source = g_timeout_add(100, +[](gpointer data) -> gboolean {
    auto *view = static_cast<FileTransferJobView *>(data);
    for (const auto &[id, row] : view->entries) {
      (void)id;
      if (row->pulse) gtk_progress_bar_pulse(GTK_PROGRESS_BAR(row->progress));
    }
    return G_SOURCE_CONTINUE;
  }, view.get());
}

} // namespace elder_terms
