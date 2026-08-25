#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace elder_terms {

struct TerminalBellPlayer;

/**
 * Creates a lazy libcanberra terminal BEL player.
 *
 * @returns Newly allocated player without opening an audio connection.
 */
TerminalBellPlayer *create_terminal_bell_player();

/**
 * Starts playback of a custom terminal BEL sound file.
 *
 * @param player Terminal BEL player.
 * @param sound_file Absolute path to a supported sound file.
 * @returns No value when playback was accepted, or an error description.
 */
std::optional<std::string>
play_terminal_bell_file(TerminalBellPlayer *player,
                        const std::filesystem::path &sound_file);

/**
 * Cancels the current custom terminal BEL playback.
 *
 * @param player Terminal BEL player, or null.
 */
void cancel_terminal_bell_playback(TerminalBellPlayer *player);

/**
 * Cancels playback, closes libcanberra, and destroys the player.
 *
 * @param player Terminal BEL player, or null.
 */
void destroy_terminal_bell_player(TerminalBellPlayer *player);

} // namespace elder_terms
