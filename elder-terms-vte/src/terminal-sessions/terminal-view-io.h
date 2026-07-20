#pragma once

#include <gtk/gtk.h>
#include <vte/vte.h>

#include <functional>
#include <memory>
#include <span>

#include <elder-terms/settings.h>

#include "terminal-text-codec.h"

namespace elder_terms {

/**
 * Current VTE terminal grid size.
 */
struct TerminalViewGridSize {
  /** Current VTE column count. */
  glong columns = 0;
  /** Current VTE row count. */
  glong rows = 0;
};

/**
 * Callback invoked with bytes committed by the VTE terminal.
 */
using TerminalViewInputCallback =
    std::function<void(std::span<const unsigned char>)>;

/**
 * Synchronous adapter for VTE terminal input and output.
 *
 * @remarks
 * This class owns only the signal connection to the VTE widget. Transport
 * backends remain responsible for asynchronous read and write work.
 */
class TerminalViewIo {
private:
  GtkWidget *terminal = nullptr;
  std::unique_ptr<TerminalTextCodec> text_codec;
  TerminalViewInputCallback input_callback;
  gulong commit_handler_id = 0;
  bool decode_warning_reported = false;
  bool encode_warning_reported = false;

  static void on_terminal_commit(VteTerminal *terminal, const gchar *text,
                                 guint size, gpointer user_data);

public:
  /**
   * Creates a VTE terminal I/O adapter.
   *
   * @param terminal VTE terminal widget.
   * @param text_settings Effective text conversion and special-code settings.
   */
  TerminalViewIo(GtkWidget *terminal,
                 const TerminalTextSettings &text_settings);

  /**
   * Disconnects the owned VTE signal connection.
   */
  ~TerminalViewIo();

  TerminalViewIo(const TerminalViewIo &) = delete;
  TerminalViewIo &operator=(const TerminalViewIo &) = delete;

  /**
   * Feeds received backend bytes into the VTE terminal.
   *
   * @param bytes Backend output bytes.
   */
  void feed(std::span<const unsigned char> bytes);

  /**
   * Applies runtime-editable text conversion and special-code settings.
   *
   * @param settings Updated effective settings.
   * @returns True when new iconv converters were created and applied.
   *
   * @remarks The previous codec remains active when initialization fails.
   */
  bool apply_text_settings(const TerminalTextSettings &settings);

  /**
   * Reads the current VTE grid size.
   *
   * @returns Current terminal grid size.
   */
  TerminalViewGridSize grid_size() const;

  /**
   * Connects user input notifications from the VTE terminal.
   *
   * @param callback Callback receiving committed VTE input bytes.
   */
  void connect_user_input(TerminalViewInputCallback callback);

  /**
   * Disconnects user input notifications.
   */
  void disconnect_user_input();
};

} // namespace elder_terms
