// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/
#pragma once

#include "file-transfer-job.h"
#include <gtk/gtk.h>

namespace elder_terms {
/** GTK presentation of a window-owned transfer history. */
struct FileTransferJobView;
/**
 * Creates the collapsible list, summary, and per-job controls.
 * @param retry Explicit new-request handler. @param clear Removes finished history.
 * @param style Applies the owner's colors to newly created rows.
 * @returns View retained by both its owner and its root widget until destruction.
 */
std::shared_ptr<FileTransferJobView> create_file_transfer_job_view(
    std::function<void(const FileTransferJobSnapshot &)> retry, std::function<void()> clear,
    std::function<void(GtkWidget *)> style);
/** @param view Job view. @returns Borrowed root widget for a vertical paned container. */
GtkWidget *file_transfer_job_view_widget(const std::shared_ptr<FileTransferJobView> &view);
/** @param view Job view. @returns Borrowed folding control. */
GtkWidget *file_transfer_job_view_expander(const std::shared_ptr<FileTransferJobView> &view);
/** @param view Job view. @param jobs Current history. @param connected Whether explicit retry is available. */
void update_file_transfer_job_view(const std::shared_ptr<FileTransferJobView> &view,
    const std::vector<std::shared_ptr<FileTransferJob>> &jobs, bool connected);
} // namespace elder_terms
