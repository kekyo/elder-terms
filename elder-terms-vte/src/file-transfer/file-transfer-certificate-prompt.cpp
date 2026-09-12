#include "file-transfer-certificate-prompt.h"

#include <utility>
#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static std::string certificate_display_text(const std::string &text) {
  if (text.empty()) return _("Unavailable");
  auto *valid = g_utf8_make_valid(text.data(), text.size());
  std::string result;
  unsigned count = 0;
  for (const char *cursor = valid; *cursor; cursor = g_utf8_next_char(cursor), ++count) {
    if (count == 256) { result += "…"; break; }
    if (count && count % 48 == 0) result += '\n';
    result.append(cursor, g_utf8_next_char(cursor) - cursor);
  }
  g_free(valid);
  return result;
}

cardio::promise<bool> confirm_file_transfer_certificate_async(
    std::shared_ptr<FileTransferWindow> window, TlsCertificateFailure failure,
    std::string title, std::string connection_label,
    cardio::cancellation cancellation) {
  std::string message = _("The server certificate could not be validated.");
  message += "\n\n" + certificate_display_text(failure.address) + ":" + std::to_string(failure.port);
  message += "\n";
  message += connection_label;
  message += "\n" + certificate_display_text(failure.reason);
  message += "\n\n";
  message += _("Allow only this certificate and validation failure for this connection? Exceptions are not saved.");
  std::string details = std::string(_("Subject:")) + " " + certificate_display_text(failure.subject);
  details += "\n" + std::string(_("Issuer:")) + " " + certificate_display_text(failure.issuer);
  details += "\n" + std::string(_("Valid from:")) + " " + certificate_display_text(failure.not_before);
  details += "\n" + std::string(_("Valid until:")) + " " + certificate_display_text(failure.not_after);
  details += "\nSHA-256:\n" + failure.sha256.substr(0, 48) + "\n" + failure.sha256.substr(48);
  message += "\n\n" + details;
  InlinePromptRequest request{
      .title = std::move(title), .message = std::move(message),
      .accept_label = _("Allow for this connection"),
      .cancel_label = _("Abort connection"), .input_required = false, .echo = false,
      .cancel_visible = true, .default_cancel = true};
  auto pending = confirm_file_transfer_window_async(
      window, std::move(request), cancellation);
  const auto response = co_await pending;
  co_return response.accepted && !cancellation.is_cancellation_requested();
}

} // namespace elder_terms
