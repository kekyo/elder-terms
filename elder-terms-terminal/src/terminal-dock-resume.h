// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace elder_terms {

/** Completes a committed batch while retaining every successfully resumed page.
 * @param count Number of staged pages in their original order.
 * @param preferred Index of the page selected before the move.
 * @param resume Starts one staged page and transfers its descriptor ownership on success.
 * @param close Releases a page whose resume failed.
 * @param dock Places successful pages in a visible destination, returning false if unavailable.
 * @param show Makes one successful page visible in its own window when docking failed.
 * @remarks The source has already committed. A failed page cannot be rolled back to it.
 */
template <typename Resume, typename Close, typename Dock, typename Show>
void finish_terminal_dock_resumes(std::size_t count, std::size_t preferred,
    Resume resume, Close close, Dock dock, Show show) {
  std::vector<std::size_t> successful;
  successful.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    if (resume(index)) successful.push_back(index);
    else close(index);
  }
  if (successful.empty()) return;
  const auto selected = std::find(successful.begin(), successful.end(), preferred) != successful.end()
      ? preferred : successful.front();
  if (!dock(successful, selected))
    for (const auto index : successful) show(index);
}

}
