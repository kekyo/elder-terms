// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

namespace elder_terms {

/**
 * Initializes GTK with the terminal application's rendering defaults.
 * @param argc Address of the main argument count, or nullptr.
 * @param argv Address of the main argument vector, or nullptr.
 * @remarks Call during startup before creating threads or other GTK objects.
 * Explicit GDK_RENDERING environment settings take precedence.
 */
void initialize_terminal_gtk(int *argc, char ***argv);

}
