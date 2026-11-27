// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#pragma once

#include "terminal-connection.h"
#include <gio/gio.h>

namespace elder_terms {
/** Properties and checkpoint of one paused terminal. The batch FD list owns its master. */
struct DockPageImage {
  std::string id;                  /**< Stable session UUID. */
  std::string config;              /**< Original connection settings path. */
  std::string startup;             /**< Startup settings path. */
  std::string title;               /**< Current title supplied by the shell. */
  std::string launcher;            /**< Unique name of the originating launcher. */
  std::string settings;            /**< Effective connection settings, including unsaved changes. */
  std::string log;                 /**< Open log path, or an empty string if inactive. */
  std::string sftp_origin;         /**< SSH process hosting shared SFTP operations. */
  std::vector<std::uint8_t> checkpoint; /**< Serialized boxrig terminal and parser state. */
  bool eof;                        /**< Whether boxrig input was finalized. */
  int descriptor;                  /**< FD list index of the PTY master, serial port or network proxy socket. */
  int control_descriptor;          /**< Network proxy control socket index, or -1 for a direct device. */
  std::string encoding;            /**< Terminal text encoding. */
  unsigned child;                  /**< Original local shell PID. */
  unsigned capacity;               /**< Bounded communication queue size in bytes. */
  unsigned chunk;                  /**< Preferred maximum I/O chunk size in bytes. */
  std::shared_ptr<GVariant> host_state; /**< Frozen title history and macro line context. */
  int decoder_descriptor = -1; /**< Optional converter FD for incoming text. */
  int encoder_descriptor = -1; /**< Optional converter FD for outgoing text. */
};
/** Prepared group in tab order. A staged receiver may start reading only after commit. */
struct DockBatch {
  std::vector<DockPageImage> pages;
  unsigned active = 0;
  std::shared_ptr<GUnixFDList> descriptors{g_unix_fd_list_new(), g_object_unref};
};
/** Receives and restores a staged batch when its owner commits. */
using DockDecision = std::function<void(bool)>;
/** Retains the origin's paused sessions until a commit, rollback, or peer exit. */
struct DockPrepared { DockBatch batch; DockDecision finish; };
/** Starts process-local drag negotiation on the desktop session bus. */
void initialize_terminal_dock_bus();
/** Releases the process registration after all pending operations finish. */
void shutdown_terminal_dock_bus();
/** Advertises a visible product window for peer-aware native pointer gestures. */
void publish_terminal_dock_window(bool visible);
/** Returns the borrowed desktop session bus, or nullptr when unavailable. */
GDBusConnection *terminal_dock_connection();
/** Checks a peer tray's availability without offering or pausing any session. */
void probe_terminal_window_drop(const std::string &peer, guint32 xid, DockDecision ready);
/** Delivers a native drop offer to the specific window under the pointer. */
void send_terminal_window_drop(const std::string &peer, guint32 xid, const std::string &token);
/** Claims a stable session address, replacing its paused owner during staging. */
bool track_terminal_dock_page(const std::string &id, std::string launcher, std::function<void(guint, const std::string &)> present);
/** Releases the current or queued D-Bus ownership on close or rollback. */
void untrack_terminal_dock_page(const std::string &id);
/** Associates the original launcher entry with its stable session address. */
void announce_terminal_dock_page(const std::string &id, const char *launch_id, const std::string &launcher);
/** Creates a single-use capability attached to GTK drag data. */
std::string terminal_dock_offer(std::function<cardio::promise<DockPrepared>()> prepare);
/** Requests one batch and commits only after stage returns its completion callback. */
cardio::promise<bool> terminal_dock_receive(std::string token,
    std::function<DockDecision(const DockBatch &)> stage);
}
