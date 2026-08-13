#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include <elder-terms/settings/macro-settings.h>

namespace elder_terms {

/**
 * Callback invoked with expanded text for a send macro action.
 */
using TerminalMacroSendCallback = std::function<void(std::string)>;

/**
 * Callback invoked with an expanded command and argument vector.
 */
using TerminalMacroCommandCallback =
    std::function<void(std::string, std::vector<std::string>)>;

/**
 * Actions emitted by a terminal macro runner.
 */
struct TerminalMacroRunnerCallbacks {
  /** Sends expanded UTF-8 text through the active terminal session. */
  TerminalMacroSendCallback send;
  /** Spawns one expanded command without a shell. */
  TerminalMacroCommandCallback command;
};

/**
 * Opaque state for matching ordered macros against received terminal lines.
 */
struct TerminalMacroRunnerState;

/**
 * Creates a terminal macro runner.
 *
 * @param rules Ordered rules, with the first rule having highest priority.
 * @param callbacks Action callbacks invoked after successful matches.
 * @returns New runner state owned by the caller.
 */
TerminalMacroRunnerState *create_terminal_macro_runner(
    std::vector<MacroRule> rules, TerminalMacroRunnerCallbacks callbacks);

/**
 * Feeds decoded UTF-8 terminal output to the macro runner.
 *
 * @param state Runner state created by create_terminal_macro_runner.
 * @param bytes Decoded terminal output, including control sequences.
 *
 * @remarks Matching is limited to one logical line. LF starts a new line and
 * a CR immediately preceding LF is omitted. At most one action executes for
 * each line.
 */
void feed_terminal_macro_runner(TerminalMacroRunnerState *state,
                                std::span<const unsigned char> bytes);

/**
 * Replaces all ordered rules and clears the current logical line.
 *
 * @param state Runner state created by create_terminal_macro_runner.
 * @param rules Replacement rules in priority order.
 */
void replace_terminal_macro_runner_rules(TerminalMacroRunnerState *state,
                                         std::vector<MacroRule> rules);

/**
 * Releases a terminal macro runner.
 *
 * @param state Runner state to destroy.
 */
void destroy_terminal_macro_runner(TerminalMacroRunnerState *state);

} // namespace elder_terms
