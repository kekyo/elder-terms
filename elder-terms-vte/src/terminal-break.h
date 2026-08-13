#pragma once

#include <functional>
#include <string>

namespace elder_terms {

/**
 * Describes presentation callbacks for one terminal BREAK request.
 */
struct TerminalBreakRequest {
  /** Receives the final user-facing BREAK status. */
  std::function<void(std::string status)> status;
  /** Receives completion after backend cleanup finishes. */
  std::function<void(bool succeeded)> finished;
};

} // namespace elder_terms
