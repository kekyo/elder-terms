#pragma once

#include <string>

namespace elder_terms {

/**
 * Identifies the SSH operation requiring user input.
 */
enum class SshUserPromptKind {
  /** User name selected before opening the SSH transport. */
  username,
  /** Confirmation before saving an unknown server host key. */
  host_key,
  /** Password authentication requested by the server. */
  password,
  /** A server-provided keyboard-interactive question. */
  keyboard_interactive,
  /** Passphrase required to unlock a configured private key. */
  private_key_passphrase,
};

/**
 * Describes one SSH question that must be rendered inside the terminal
 * surface.
 */
struct SshUserPrompt {
  /** Prompt category. */
  SshUserPromptKind kind = SshUserPromptKind::password;
  /** Short heading displayed by the overlay panel. */
  std::string title;
  /** Full question and security context displayed to the user. */
  std::string message;
  /** Optional preformatted security context displayed in a monospace font. */
  std::string monospace_message = {};
  /** Initial text displayed in the response entry. */
  std::string initial_text;
  /** True when the panel must collect a text response. */
  bool input_required = true;
  /** True when entered text may be displayed instead of masked. */
  bool echo = false;
  /** True when the ordinary accepting action may be displayed. */
  bool accept_visible = true;
  /** True when the user may replace a changed per-user host-key entry. */
  bool host_key_reset_available = false;
};

/**
 * Result of one SSH overlay question.
 */
struct SshUserPromptResponse {
  /** True when the user accepted or submitted the prompt. */
  bool accepted = false;
  /** Text collected from the prompt entry, if any. */
  std::string text;
  /** True when the changed host-key reset action was selected. */
  bool reset_host_key = false;
};

} // namespace elder_terms
