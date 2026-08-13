#pragma once

#include <string>

namespace elder_terms {

/**
 * Identifies the SSH operation requiring user input.
 */
enum class SshUserPromptKind {
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
  /** True when the panel must collect a text response. */
  bool input_required = true;
  /** True when entered text may be displayed instead of masked. */
  bool echo = false;
};

/**
 * Result of one SSH overlay question.
 */
struct SshUserPromptResponse {
  /** True when the user accepted or submitted the prompt. */
  bool accepted = false;
  /** Text collected from the prompt entry, if any. */
  std::string text;
};

} // namespace elder_terms
