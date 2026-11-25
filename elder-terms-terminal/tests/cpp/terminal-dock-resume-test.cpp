// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.

#include "../../src/terminal-dock-resume.h"

#include <cassert>
#include <cstddef>
#include <vector>

int main() {
  std::vector<std::size_t> closed;
  std::vector<std::size_t> visible;
  std::vector<std::size_t> docked;
  std::size_t selected = 99;

  elder_terms::finish_terminal_dock_resumes(3, 1,
      [&](std::size_t index) { return index != 1; },
      [&](std::size_t index) { closed.push_back(index); },
      [&](const auto &indices, std::size_t active) {
        docked = indices;
        selected = active;
        return true;
      },
      [&](std::size_t index) { visible.push_back(index); });
  assert((closed == std::vector<std::size_t>{1}));
  assert((docked == std::vector<std::size_t>{0, 2}));
  assert(selected == 0);
  assert(visible.empty());

  docked.clear();
  closed.clear();
  elder_terms::finish_terminal_dock_resumes(3, 2,
      [&](std::size_t index) { return index != 1; },
      [&](std::size_t index) { closed.push_back(index); },
      [&](const auto &indices, std::size_t active) {
        docked = indices;
        selected = active;
        return false;
      },
      [&](std::size_t index) { visible.push_back(index); });
  assert((closed == std::vector<std::size_t>{1}));
  assert((docked == std::vector<std::size_t>{0, 2}));
  assert(selected == 2);
  assert((visible == std::vector<std::size_t>{0, 2}));

  docked.clear();
  closed.clear();
  visible.clear();
  elder_terms::finish_terminal_dock_resumes(2, 0,
      [](std::size_t) { return false; },
      [&](std::size_t index) { closed.push_back(index); },
      [&](const auto &, std::size_t) {
        assert(false);
        return false;
      },
      [&](std::size_t index) { visible.push_back(index); });
  assert((closed == std::vector<std::size_t>{0, 1}));
  assert(visible.empty());
}
