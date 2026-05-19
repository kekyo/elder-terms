#pragma once

#include <glib.h>

#include <elder-terms/settings.h>

#include "../terminal-session-callbacks.h"

namespace elder_terms {

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
   * Applies runtime-editable connection settings.
   *
   * @param profile Updated connection profile.
   */
  virtual void apply_connection_profile(const TerminalConnectionProfile &profile) {
    (void)profile;
  }
};

} // namespace elder_terms
