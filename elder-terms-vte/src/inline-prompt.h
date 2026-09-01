#pragma once

#include <memory>
#include <string>

#include <cardio.h>
#include <gtk/gtk.h>

namespace elder_terms {

/**
 * Widgets composing one inline question panel.
 */
struct InlinePromptWidgets {
  /** Frame positioned above the host surface. */
  GtkWidget *panel;
  /** Opaque panel background. */
  GtkWidget *background;
  /** Short prompt heading. */
  GtkWidget *title_label;
  /** Full prompt message. */
  GtkWidget *message_label;
  /** Optional text response entry. */
  GtkWidget *entry;
  /** Button rejecting the prompt. */
  GtkWidget *cancel_button;
  /** Button accepting the prompt. */
  GtkWidget *accept_button;
  /** Optional button selecting a third response, or null for two choices. */
  GtkWidget *alternative_button = nullptr;
};

/**
 * Presentation and input requirements for one inline question.
 */
struct InlinePromptRequest {
  /** Short prompt heading. */
  std::string title;
  /** Full prompt message. */
  std::string message;
  /** Text displayed by the accepting button. */
  std::string accept_label;
  /** Text displayed by the rejecting button. */
  std::string cancel_label;
  /** True when the prompt collects a text response. */
  bool input_required;
  /** True when entered text may be displayed. */
  bool echo;
  /** True when the rejecting button is visible. */
  bool cancel_visible;
  /** Text displayed by the optional third-response button. */
  std::string alternative_label = {};
  /** True when the third-response button is visible. */
  bool alternative_visible = false;
};

/**
 * User response collected by an inline question.
 */
struct InlinePromptResponse {
  /** True when the user accepted the prompt. */
  bool accepted = false;
  /** Submitted text, or an empty string when no input was required. */
  std::string text;
  /** True when the optional third response was selected. */
  bool alternative = false;
};

/**
 * Stable state owning signal handlers and the pending inline response.
 */
struct InlinePromptController;

/**
 * Creates a standard inline question panel.
 *
 * @param accessible_id_prefix Prefix used for every test-accessible widget ID.
 * @returns Unowned widgets whose panel owns all descendants.
 */
InlinePromptWidgets
create_inline_prompt_widgets(const std::string &accessible_id_prefix);

/**
 * Binds common asynchronous prompt behavior to an existing widget set.
 *
 * @param widgets Complete inline question widget set.
 * @returns Shared controller that must outlive the widgets while they are used.
 */
std::shared_ptr<InlinePromptController>
create_inline_prompt_controller(InlinePromptWidgets widgets);

/**
 * Displays one question and waits asynchronously for its response.
 *
 * @param controller Bound prompt controller.
 * @param request Presentation and input requirements.
 * @param cancellation Cancellation signal for the pending question.
 * @returns Accepted response, or a rejected response after cancellation.
 * @throws std::invalid_argument when a third response is requested from a
 * controller without an alternative button.
 */
cardio::promise<InlinePromptResponse> prompt_inline_async(
    const std::shared_ptr<InlinePromptController> &controller,
    InlinePromptRequest request, cardio::cancellation cancellation);

/**
 * Rejects and hides the active question, if any.
 *
 * @param controller Bound prompt controller.
 */
void cancel_inline_prompt(
    const std::shared_ptr<InlinePromptController> &controller);

} // namespace elder_terms
