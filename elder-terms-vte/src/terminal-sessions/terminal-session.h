#pragma once

#include <glib.h>

#include <memory>
#include <string>

#include <elder-terms/settings.h>

#include "../terminal-session-callbacks.h"
#include "../terminal-text-send.h"
#include "../terminal-transfer.h"

namespace elder_terms {

class AuthenticatedSshTransport;

/**
 * Abstract terminal connection session backend.
 */
class TerminalSession {
public:
  /**
   * Destroys the terminal session backend.
   */
  virtual ~TerminalSession() = default;

  /**
   * Starts the terminal session backend.
   *
   * @returns True when the backend start request was accepted.
   */
  virtual bool start() = 0;

  /**
   * Requests the backend to stop pending work and release live connections.
   */
  virtual void stop() = 0;

  /**
   * Notifies the backend that the VTE grid size changed.
   *
   * @param columns Current VTE column count.
   * @param rows Current VTE row count.
   */
  virtual void resize(glong columns, glong rows) = 0;

  /**
   * Returns the current backend-specific connection detail.
   *
   * @returns Status text describing the current session endpoint.
   */
  virtual std::string connection_detail() const = 0;

  /**
   * Returns whether this backend can start X/Y/ZMODEM transfers.
   *
   * @returns True when file transfers are supported.
   */
  virtual bool supports_transfer() const {
    return false;
  }

  /**
   * Returns whether this backend can send finite text files.
   *
   * @returns True when text send operations are supported.
   */
  virtual bool supports_text_send() const {
    return false;
  }

  /**
   * Returns whether a transfer is currently active.
   *
   * @returns True while a transfer task is active.
   */
  virtual bool transfer_in_progress() const {
    return false;
  }

  /**
   * Starts one X/Y/ZMODEM transfer request.
   *
   * @param request Transfer request.
   * @returns True when the request was accepted.
   */
  virtual bool start_transfer(TerminalTransferRequest request) {
    (void)request;
    return false;
  }

  /**
   * Starts one text file send request.
   *
   * @param request Text send request.
   * @returns True when the request was accepted.
   */
  virtual bool start_text_send(TerminalTextSendRequest request) {
    (void)request;
    return false;
  }

  /**
   * Requests cancellation of the active file transfer or text send.
   *
   * @returns True when an active operation accepted the cancellation request.
   */
  virtual bool cancel_transfer() {
    return false;
  }

  /**
   * Enables or disables ZMODEM auto-start detection.
   *
   * @param enabled True when remote ZMODEM preambles should auto-start
   * transfers.
   */
  virtual void set_zmodem_autostart(bool enabled) {
    (void)enabled;
  }

  /**
   * Returns the authenticated SSH transport owned by this backend.
   *
   * @returns Shared authenticated transport, or null for non-SSH and
   * disconnected sessions.
   */
  virtual std::shared_ptr<AuthenticatedSshTransport>
  authenticated_ssh_transport() const {
    return nullptr;
  }

  /**
   * Applies runtime-editable connection settings.
   *
   * @param profile Updated connection profile.
   */
  virtual void apply_connection_profile(const TerminalConnectionProfile &profile) {
    (void)profile;
  }
};

} // namespace elder_terms
