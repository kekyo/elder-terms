#include "terminal-bell-player.h"

#include <cstdint>
#include <string>

#include <canberra.h>

namespace elder_terms {

static constexpr std::uint32_t terminal_bell_playback_id = 1;

struct TerminalBellPlayer {
  ca_context *context = nullptr;
};

static std::optional<std::string> canberra_error(const char *operation,
                                                 int result) {
  if (result == CA_SUCCESS) {
    return std::nullopt;
  }
  const char *description = ca_strerror(result);
  return std::string(operation) + ": " +
         (description == nullptr ? "unknown libcanberra error" : description);
}

static std::optional<std::string>
open_terminal_bell_player(TerminalBellPlayer *player) {
  if (player->context != nullptr) {
    return std::nullopt;
  }

  int result = ca_context_create(&player->context);
  std::optional<std::string> error =
      canberra_error("ca_context_create", result);
  if (error.has_value()) {
    player->context = nullptr;
    return error;
  }

  result = ca_context_change_props(
      player->context, CA_PROP_APPLICATION_NAME, "elder-terms",
      CA_PROP_APPLICATION_ID, "net.kekyo.elder-terms-vte",
      CA_PROP_APPLICATION_ICON_NAME, "elder-terms", nullptr);
  error = canberra_error("ca_context_change_props", result);
  if (!error.has_value()) {
    result = ca_context_open(player->context);
    error = canberra_error("ca_context_open", result);
  }
  if (error.has_value()) {
    ca_context_destroy(player->context);
    player->context = nullptr;
  }
  return error;
}

TerminalBellPlayer *create_terminal_bell_player() {
  return new TerminalBellPlayer();
}

std::optional<std::string>
play_terminal_bell_file(TerminalBellPlayer *player,
                        const std::filesystem::path &sound_file) {
  if (player == nullptr) {
    return std::string("terminal bell player is unavailable");
  }
  const std::optional<std::string> open_error =
      open_terminal_bell_player(player);
  if (open_error.has_value()) {
    return open_error;
  }

  const std::string file_name = sound_file.string();
  const int result = ca_context_play(
      player->context, terminal_bell_playback_id, CA_PROP_MEDIA_FILENAME,
      file_name.c_str(), CA_PROP_EVENT_DESCRIPTION, "Terminal bell",
      CA_PROP_MEDIA_ROLE, "event", CA_PROP_CANBERRA_CACHE_CONTROL, "volatile",
      nullptr);
  const std::optional<std::string> error =
      canberra_error("ca_context_play", result);
  if (error.has_value()) {
    ca_context_destroy(player->context);
    player->context = nullptr;
  }
  return error;
}

void cancel_terminal_bell_playback(TerminalBellPlayer *player) {
  if (player == nullptr || player->context == nullptr) {
    return;
  }
  (void)ca_context_cancel(player->context, terminal_bell_playback_id);
}

void destroy_terminal_bell_player(TerminalBellPlayer *player) {
  if (player == nullptr) {
    return;
  }
  if (player->context != nullptr) {
    (void)ca_context_cancel(player->context, terminal_bell_playback_id);
    ca_context_destroy(player->context);
  }
  delete player;
}

} // namespace elder_terms
