#include "terminal-view-io.h"

#include <vte/vte.h>

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace elder_terms {

static bool is_plain_return_key(const GdkEventKey *event) {
  if (event == nullptr ||
      (event->keyval != GDK_KEY_Return &&
       event->keyval != GDK_KEY_KP_Enter)) {
    return false;
  }
  constexpr guint action_modifiers =
      GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK |
      GDK_HYPER_MASK | GDK_META_MASK;
  return (event->state & action_modifiers) == 0;
}

static std::optional<TerminalSpecialKey>
terminal_special_key(const GdkEventKey *event) {
  if (event == nullptr) {
    return std::nullopt;
  }
  switch (event->keyval) {
  case GDK_KEY_Return:
    return TerminalSpecialKey::enter;
  case GDK_KEY_KP_Enter:
    return TerminalSpecialKey::keypad_enter;
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Left_Tab:
    return TerminalSpecialKey::tab;
  case GDK_KEY_BackSpace:
    return TerminalSpecialKey::backspace;
  case GDK_KEY_Escape:
    return TerminalSpecialKey::escape;
  case GDK_KEY_space:
    return TerminalSpecialKey::space;
  default:
    return std::nullopt;
  }
}

static TerminalKeyModifiers terminal_key_modifiers(const GdkEventKey *event) {
  if (event == nullptr) {
    return {};
  }
  return {
      .shift = (event->state & GDK_SHIFT_MASK) != 0 ||
               event->keyval == GDK_KEY_ISO_Left_Tab,
      .alt = (event->state & GDK_MOD1_MASK) != 0,
      .control = (event->state & GDK_CONTROL_MASK) != 0,
      .super = (event->state & GDK_SUPER_MASK) != 0,
      .hyper = (event->state & GDK_HYPER_MASK) != 0,
      .meta = (event->state & GDK_META_MASK) != 0,
  };
}

static bool is_line_ending_commit(const std::string &text) {
  return text == "\r" || text == "\n" || text == "\r\n";
}

static std::string return_code_text(TerminalReturnCode return_code) {
  if (return_code == TerminalReturnCode::cr) {
    return "\r";
  }
  if (return_code == TerminalReturnCode::lf) {
    return "\n";
  }
  return return_code == TerminalReturnCode::crlf ? "\r\n" : std::string();
}

static VteEraseBinding
backspace_binding(TerminalBackspaceCode backspace_code) {
  if (backspace_code == TerminalBackspaceCode::automatic) {
    return VTE_ERASE_AUTO;
  }
  return backspace_code == TerminalBackspaceCode::bs
             ? VTE_ERASE_ASCII_BACKSPACE
             : VTE_ERASE_ASCII_DELETE;
}

TerminalViewIo::TerminalViewIo(
    GtkWidget *terminal, const TerminalTextSettings &text_settings,
    TerminalViewOutputCallback output_callback)
    : terminal(terminal),
      text_codec(std::make_unique<TerminalTextCodec>(text_settings)),
      output_callback(std::move(output_callback)),
      return_code(text_settings.return_code) {
  vte_terminal_set_backspace_binding(
      VTE_TERMINAL(terminal), backspace_binding(text_settings.backspace_code));
  vte_terminal_set_delete_binding(VTE_TERMINAL(terminal), VTE_ERASE_AUTO);
}

TerminalViewIo::~TerminalViewIo() {
  disconnect_user_input();
}

void TerminalViewIo::on_terminal_commit(VteTerminal *, const gchar *text,
                                        guint size, gpointer user_data) {
  auto *self = static_cast<TerminalViewIo *>(user_data);
  if (text != nullptr && size != 0) {
    std::string committed(text, static_cast<std::size_t>(size));
    bool clear_pending = false;
    if (self->pending_modified_special_key.has_value() &&
        is_legacy_modified_special_key_commit(
            self->pending_modified_special_key->key, committed)) {
      committed = self->pending_modified_special_key->encoded_sequence;
      clear_pending = true;
    } else if (self->return_key_pending &&
               self->return_code != TerminalReturnCode::automatic &&
               is_line_ending_commit(committed)) {
      committed = return_code_text(self->return_code);
      clear_pending = true;
    } else if (self->return_key_pending) {
      clear_pending = true;
    }
    if (clear_pending) {
      self->clear_pending_key_state();
    }
    (void)self->send_utf8_text(committed);
  }
}

gboolean TerminalViewIo::on_terminal_key_press(GtkWidget *,
                                               GdkEventKey *event,
                                               gpointer user_data) {
  auto *self = static_cast<TerminalViewIo *>(user_data);
  self->clear_pending_key_state();
  self->return_key_pending = is_plain_return_key(event);
  if (self->keyboard_protocol_state.modified_special_keys_enabled()) {
    const std::optional<TerminalSpecialKey> key = terminal_special_key(event);
    if (key.has_value()) {
      std::optional<std::string> encoded =
          encode_modified_special_key(*key, terminal_key_modifiers(event));
      if (encoded.has_value()) {
        self->pending_modified_special_key = {
            .key = *key,
            .encoded_sequence = std::move(*encoded),
        };
      }
    }
  }
  if (self->return_key_pending ||
      self->pending_modified_special_key.has_value()) {
    self->pending_key_clear_source_id =
        g_idle_add(TerminalViewIo::clear_pending_key, self);
  }
  return GDK_EVENT_PROPAGATE;
}

gboolean TerminalViewIo::clear_pending_key(gpointer user_data) {
  auto *self = static_cast<TerminalViewIo *>(user_data);
  self->return_key_pending = false;
  self->pending_modified_special_key.reset();
  self->pending_key_clear_source_id = 0;
  return G_SOURCE_REMOVE;
}

void TerminalViewIo::clear_pending_key_state() {
  if (pending_key_clear_source_id != 0) {
    g_source_remove(pending_key_clear_source_id);
    pending_key_clear_source_id = 0;
  }
  return_key_pending = false;
  pending_modified_special_key.reset();
}

void TerminalViewIo::feed(std::span<const unsigned char> bytes) {
  if (bytes.empty()) {
    return;
  }

  const TerminalTextConversionResult conversion = text_codec->decode(bytes);
  if (conversion.used_replacement && !decode_warning_reported) {
    std::cerr << "Warning: terminal output contained invalid bytes for the "
                 "selected encoding"
              << '\n';
    decode_warning_reported = true;
  }
  if (!conversion.bytes.empty()) {
    // XYZMODEM payloads bypass TerminalViewIo::feed(), so only cooked bytes on
    // the terminal presentation path can affect keyboard protocol state.
    keyboard_protocol_state.observe(std::span<const unsigned char>(
        conversion.bytes.data(), conversion.bytes.size()));
  }
  if (output_callback) {
    output_callback(
        bytes, std::span<const unsigned char>(conversion.bytes.data(),
                                              conversion.bytes.size()));
  }
  if (!conversion.bytes.empty()) {
    vte_terminal_feed(VTE_TERMINAL(terminal),
                      reinterpret_cast<const char *>(conversion.bytes.data()),
                      static_cast<gssize>(conversion.bytes.size()));
  }
}

bool TerminalViewIo::send_utf8_text(const std::string &text) {
  if (!input_callback || text.empty()) {
    return false;
  }

  const auto *data = reinterpret_cast<const unsigned char *>(text.data());
  const TerminalTextConversionResult conversion = text_codec->encode(
      std::span<const unsigned char>(data, text.size()));
  if (conversion.used_replacement && !encode_warning_reported) {
    std::cerr << "Warning: terminal input contained text that is not "
                 "representable in the selected encoding"
              << '\n';
    encode_warning_reported = true;
  }
  if (conversion.bytes.empty()) {
    return false;
  }
  input_callback(std::span<const unsigned char>(conversion.bytes.data(),
                                                conversion.bytes.size()));
  return true;
}

bool TerminalViewIo::apply_text_settings(
    const TerminalTextSettings &settings) {
  try {
    auto next_codec = std::make_unique<TerminalTextCodec>(settings);
    vte_terminal_set_backspace_binding(
        VTE_TERMINAL(terminal), backspace_binding(settings.backspace_code));
    vte_terminal_set_delete_binding(VTE_TERMINAL(terminal), VTE_ERASE_AUTO);
    text_codec = std::move(next_codec);
    return_code = settings.return_code;
    decode_warning_reported = false;
    encode_warning_reported = false;
    return true;
  } catch (const std::system_error &error) {
    std::cerr << "Warning: failed to apply terminal text settings: "
              << error.what() << '\n';
    return false;
  }
}

TerminalViewGridSize TerminalViewIo::grid_size() const {
  return {
      .columns = vte_terminal_get_column_count(VTE_TERMINAL(terminal)),
      .rows = vte_terminal_get_row_count(VTE_TERMINAL(terminal)),
  };
}

void TerminalViewIo::connect_user_input(TerminalViewInputCallback callback) {
  disconnect_user_input();
  input_callback = std::move(callback);
  key_press_handler_id =
      g_signal_connect(terminal, "key-press-event",
                       G_CALLBACK(TerminalViewIo::on_terminal_key_press), this);
  commit_handler_id =
      g_signal_connect(terminal, "commit",
                       G_CALLBACK(TerminalViewIo::on_terminal_commit), this);
}

void TerminalViewIo::disconnect_user_input() {
  clear_pending_key_state();
  if (key_press_handler_id != 0) {
    g_signal_handler_disconnect(terminal, key_press_handler_id);
    key_press_handler_id = 0;
  }
  if (commit_handler_id != 0) {
    g_signal_handler_disconnect(terminal, commit_handler_id);
    commit_handler_id = 0;
  }
  input_callback = {};
}

} // namespace elder_terms
