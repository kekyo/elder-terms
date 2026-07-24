#pragma once

#include <functional>
#include <memory>
#include <string>

#include <gtk/gtk.h>

#include "sftp-client.h"

namespace elder_terms {

/**
 * Construction options for one independent SFTP browser window.
 */
struct SftpWindowOptions {
  /** User-visible connection name. */
  std::string connection_name;
  /** Initial native local filesystem directory. */
  std::string local_directory;
  /** Initial remote SFTP directory. */
  std::string remote_directory;
  /** Initialized SFTP subsystem used by browsing and transfers. */
  std::shared_ptr<SftpClient> client;
  /** Called asynchronously after the GTK window is destroyed. */
  std::function<void()> closed;
};

/**
 * Opaque state for one dual-pane SFTP browser window.
 */
struct SftpWindow;

/**
 * Creates a VTE-independent dual-pane SFTP browser.
 *
 * @param options Initial paths, SFTP client, and lifecycle callback.
 * @returns Owned SFTP window state.
 */
std::shared_ptr<SftpWindow>
create_sftp_window(SftpWindowOptions options);

/**
 * Returns the top-level GTK widget.
 *
 * @param window SFTP window state.
 * @returns Top-level GtkWindow widget, or null for invalid state.
 */
GtkWidget *sftp_window_widget(
    const std::shared_ptr<SftpWindow> &window) noexcept;

/**
 * Shows the SFTP window and starts loading both directory trees.
 *
 * @param window SFTP window state.
 */
void show_sftp_window(const std::shared_ptr<SftpWindow> &window);

/**
 * Enables or disables remote operations after transport state changes.
 *
 * @param window SFTP window state.
 * @param available True while the authenticated SSH transport is usable.
 */
void set_sftp_window_connection_available(
    const std::shared_ptr<SftpWindow> &window, bool available);

/**
 * Presents an existing SFTP window above other application windows.
 *
 * @param window SFTP window state.
 */
void present_sftp_window(const std::shared_ptr<SftpWindow> &window);

} // namespace elder_terms
