#pragma once

#include <cardio.h>
#include <string>

namespace elder_terms {

/**
 * Requests selection of a local file in the desktop file manager.
 * @param path Local path, preserving the selected symbolic link itself.
 * @param parent_window Portal window identifier, or empty when unavailable.
 * @param cancellation Cancellation tied to the originating window's lifetime.
 * @return True when handled (including user cancellation), false when the
 *         caller should open the containing directory using its association.
 * @throws cardio::canceled_exception When the originating window closes.
 * @throws std::exception When a request fails without a safe fallback.
 */
cardio::promise<bool> try_reveal_local_file_async(
    std::string path, std::string parent_window, cardio::cancellation cancellation);

} // namespace elder_terms
