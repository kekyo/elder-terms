#pragma once

#include <functional>
#include <memory>
#include <string>

#include <gtk/gtk.h>

#include <elder-terms/settings.h>

#include "../inline-prompt.h"
#include "remote-file-client.h"

namespace elder_terms {

/**
 * Construction options for one independent remote file browser window.
 */
struct FileTransferWindowOptions {
  /** User-visible connection name. */
  std::string connection_name;
  /** User-visible protocol name, such as SFTP or FTP. */
  std::string protocol_name;
  /** Initial native local filesystem directory. */
  std::string local_directory;
  /** Initial remote directory. */
  std::string remote_directory;
  /** Initial exterior and browser background colors. */
  GeneralColorSettings colors;
  /** Called asynchronously after the GTK window is destroyed. */
  std::function<void()> closed;
};

/**
 * Opaque state for one dual-pane remote file browser window.
 */
struct FileTransferWindow;

/**
 * Creates a VTE-independent dual-pane remote file browser.
 *
 * @param options Protocol label, paths, colors, and lifecycle callback.
 * @returns Owned file-transfer window state.
 */
std::shared_ptr<FileTransferWindow>
create_file_transfer_window(FileTransferWindowOptions options);

/**
 * Returns the top-level GTK widget.
 *
 * @param window File-transfer window state.
 * @returns Top-level GtkWindow widget, or null for invalid state.
 */
GtkWidget *file_transfer_window_widget(
    const std::shared_ptr<FileTransferWindow> &window) noexcept;

/**
 * Shows the file-transfer window and starts loading both directory trees.
 *
 * @param window File-transfer window state.
 */
void show_file_transfer_window(const std::shared_ptr<FileTransferWindow> &window);

/**
 * Attaches an authenticated remote service and starts remote browsing.
 *
 * @param window File-transfer window waiting for its remote service.
 * @param client Initialized remote service used by browsing and transfers.
 * @remarks A remote service may only be attached once.
 */
void attach_file_transfer_window_client(
    const std::shared_ptr<FileTransferWindow> &window,
    std::shared_ptr<RemoteFileClient> client);

/**
 * Collects authentication input inside the file-transfer surface.
 *
 * @param window File-transfer window hosting the prompt.
 * @param request Presentation and input requirements.
 * @param cancellation Authentication cancellation signal.
 * @returns Accepted response, or a rejected response after cancellation.
 */
cardio::promise<InlinePromptResponse> prompt_file_transfer_window_async(
    const std::shared_ptr<FileTransferWindow> &window,
    InlinePromptRequest request, cardio::cancellation cancellation);

/**
 * Shows a connection failure inside the file-transfer surface until closed.
 *
 * @param window File-transfer window hosting the error.
 * @param title Short connection failure heading.
 * @param message Detailed failure text.
 * @param cancellation Application shutdown cancellation signal.
 */
cardio::promise<void> show_file_transfer_window_connection_error_async(
    const std::shared_ptr<FileTransferWindow> &window, std::string title,
    std::string message, cardio::cancellation cancellation);

/**
 * Enables or disables remote operations after transport state changes.
 *
 * @param window File-transfer window state.
 * @param available True while the remote transport is usable.
 */
void set_file_transfer_window_connection_available(
    const std::shared_ptr<FileTransferWindow> &window, bool available);

/**
 * Applies optional exterior and browser backgrounds at runtime.
 *
 * @param window File-transfer window state.
 * @param settings Effective optional RGB colors.
 */
void set_file_transfer_window_colors(
    const std::shared_ptr<FileTransferWindow> &window,
    const GeneralColorSettings &settings);

/**
 * Presents an existing file-transfer window above other application windows.
 *
 * @param window File-transfer window state.
 */
void present_file_transfer_window(const std::shared_ptr<FileTransferWindow> &window);

} // namespace elder_terms
