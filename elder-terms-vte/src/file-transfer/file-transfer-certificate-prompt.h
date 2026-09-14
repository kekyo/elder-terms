#pragma once

#include "file-transfer-window.h"
#include "../tls/certificate-failure.h"

namespace elder_terms {

/**
 * Shows a copied certificate failure using the shared connection overlay.
 * @param window Window retained until the confirmation ends.
 * @param failure Owned certificate details and validation identity.
 * @param title Protocol-specific translated title.
 * @param connection_label Protocol-specific connection or channel label.
 * @param cancellation Cancellation of the logical connection.
 * @returns True only after explicit approval; cancellation never approves.
 * @remarks The application owns connection shutdown and exception status.
 */
cardio::promise<bool> confirm_file_transfer_certificate_async(
    std::shared_ptr<FileTransferWindow> window, TlsCertificateFailure failure,
    std::string title, std::string connection_label,
    cardio::cancellation cancellation);

} // namespace elder_terms
