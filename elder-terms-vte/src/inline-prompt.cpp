#include "inline-prompt.h"

#include <memory>
#include <utility>

#include <gestament/gtk.h>

#define GETTEXT_PACKAGE "elder-terms"
#include <glib/gi18n-lib.h>

namespace elder_terms {

static constexpr const char *inline_prompt_background_style_class =
    "inline-prompt-background";
static constexpr const char *inline_prompt_title_style_class =
    "inline-prompt-title";
static constexpr const char *inline_prompt_message_style_class =
    "inline-prompt-message";
static constexpr const char *inline_prompt_css =
    ".inline-prompt-background {"
    "  background-color: rgba(48, 48, 48, 0.96);"
    "}"
    ".inline-prompt-title {"
    "  color: #ffffff;"
    "  font-weight: bold;"
    "}"
    ".inline-prompt-message {"
    "  color: #ffffff;"
    "}";

struct InlinePromptPendingRequest {
  std::shared_ptr<cardio::promise_source<InlinePromptResponse>> source;
  cardio::cancellation_registration cancellation_registration;
  bool input_required = false;
};

struct InlinePromptController {
  InlinePromptWidgets widgets;
  std::shared_ptr<InlinePromptPendingRequest> request;
};

static std::string accessible_id(const std::string &prefix,
                                 const char *suffix) {
  return prefix + "_" + suffix;
}

static void apply_inline_prompt_style(const InlinePromptWidgets &widgets) {
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(provider, inline_prompt_css, -1, nullptr);

  GtkStyleContext *background_context =
      gtk_widget_get_style_context(widgets.background);
  gtk_style_context_add_class(background_context,
                              inline_prompt_background_style_class);
  gtk_style_context_add_provider(background_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *title_context =
      gtk_widget_get_style_context(widgets.title_label);
  gtk_style_context_add_class(title_context, inline_prompt_title_style_class);
  gtk_style_context_add_provider(title_context, GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkStyleContext *message_context =
      gtk_widget_get_style_context(widgets.message_label);
  gtk_style_context_add_class(message_context,
                              inline_prompt_message_style_class);
  gtk_style_context_add_provider(message_context,
                                 GTK_STYLE_PROVIDER(provider),
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

InlinePromptWidgets
create_inline_prompt_widgets(const std::string &accessible_id_prefix) {
  GtkWidget *panel = gtk_frame_new(nullptr);
  gestament_gtk_assign_accessible_id(
      panel, accessible_id(accessible_id_prefix, "panel").c_str());
  gtk_widget_set_visible(panel, FALSE);
  gtk_widget_set_no_show_all(panel, TRUE);
  gtk_widget_set_halign(panel, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(panel, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(panel, 380, -1);
  gtk_frame_set_shadow_type(GTK_FRAME(panel), GTK_SHADOW_OUT);

  GtkWidget *background = gtk_event_box_new();
  gestament_gtk_assign_accessible_id(
      background,
      accessible_id(accessible_id_prefix, "background").c_str());
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(background), TRUE);
  gtk_container_add(GTK_CONTAINER(panel), background);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 14);
  gtk_widget_set_margin_bottom(content, 14);
  gtk_container_add(GTK_CONTAINER(background), content);

  GtkWidget *title = gtk_label_new("");
  gestament_gtk_assign_accessible_id(
      title,
      accessible_id(accessible_id_prefix, "title_label").c_str());
  gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
  gtk_box_pack_start(GTK_BOX(content), title, FALSE, TRUE, 0);

  GtkWidget *message = gtk_label_new("");
  gestament_gtk_assign_accessible_id(
      message,
      accessible_id(accessible_id_prefix, "message_label").c_str());
  gtk_label_set_xalign(GTK_LABEL(message), 0.0F);
  gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(message), 56);
  gtk_label_set_selectable(GTK_LABEL(message), TRUE);
  gtk_box_pack_start(GTK_BOX(content), message, FALSE, TRUE, 0);

  GtkWidget *entry = gtk_entry_new();
  gestament_gtk_assign_accessible_id(
      entry, accessible_id(accessible_id_prefix, "entry").c_str());
  gtk_widget_set_visible(entry, FALSE);
  gtk_widget_set_no_show_all(entry, TRUE);
  gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, TRUE, 0);

  GtkWidget *actions = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(actions), 8);
  gtk_button_box_set_layout(GTK_BUTTON_BOX(actions), GTK_BUTTONBOX_END);
  gtk_box_pack_start(GTK_BOX(content), actions, FALSE, TRUE, 0);

  GtkWidget *cancel = gtk_button_new_with_label(_("Cancel"));
  gestament_gtk_assign_accessible_id(
      cancel,
      accessible_id(accessible_id_prefix, "cancel_button").c_str());
  gtk_container_add(GTK_CONTAINER(actions), cancel);

  GtkWidget *accept = gtk_button_new_with_label(_("OK"));
  gestament_gtk_assign_accessible_id(
      accept,
      accessible_id(accessible_id_prefix, "accept_button").c_str());
  gtk_widget_set_can_default(accept, TRUE);
  gtk_widget_set_receives_default(accept, TRUE);
  gtk_container_add(GTK_CONTAINER(actions), accept);

  InlinePromptWidgets widgets{
      .panel = panel,
      .background = background,
      .title_label = title,
      .message_label = message,
      .entry = entry,
      .cancel_button = cancel,
      .accept_button = accept,
  };
  return widgets;
}

static void hide_inline_prompt(InlinePromptController *controller) {
  if (controller == nullptr || controller->widgets.panel == nullptr) {
    return;
  }
  gtk_entry_set_text(GTK_ENTRY(controller->widgets.entry), "");
  gtk_widget_set_visible(controller->widgets.entry, FALSE);
  gtk_widget_set_no_show_all(controller->widgets.entry, TRUE);
  gtk_widget_set_visible(controller->widgets.panel, FALSE);
  gtk_widget_set_no_show_all(controller->widgets.panel, TRUE);
}

static void complete_inline_prompt(InlinePromptController *controller,
                                   InlinePromptResponse response) {
  if (controller == nullptr || controller->request == nullptr) {
    return;
  }
  std::shared_ptr<InlinePromptPendingRequest> request =
      std::exchange(controller->request, nullptr);
  request->cancellation_registration = {};
  hide_inline_prompt(controller);
  (void)request->source->try_resolve(std::move(response));
}

static void on_inline_prompt_accept_clicked(GtkButton *, gpointer data) {
  auto *controller = static_cast<InlinePromptController *>(data);
  if (controller == nullptr || controller->request == nullptr) {
    return;
  }
  std::string text;
  if (controller->request->input_required) {
    const char *entry_text =
        gtk_entry_get_text(GTK_ENTRY(controller->widgets.entry));
    text = entry_text == nullptr ? std::string() : std::string(entry_text);
  }
  complete_inline_prompt(
      controller, {.accepted = true, .text = std::move(text)});
}

static void on_inline_prompt_cancel_clicked(GtkButton *, gpointer data) {
  complete_inline_prompt(static_cast<InlinePromptController *>(data), {});
}

static void on_inline_prompt_entry_activated(GtkEntry *, gpointer data) {
  on_inline_prompt_accept_clicked(nullptr, data);
}

std::shared_ptr<InlinePromptController>
create_inline_prompt_controller(InlinePromptWidgets widgets) {
  auto controller = std::make_shared<InlinePromptController>(
      InlinePromptController{
          .widgets = widgets,
          .request = nullptr,
      });
  apply_inline_prompt_style(widgets);
  g_signal_connect(widgets.accept_button, "clicked",
                   G_CALLBACK(on_inline_prompt_accept_clicked),
                   controller.get());
  g_signal_connect(widgets.cancel_button, "clicked",
                   G_CALLBACK(on_inline_prompt_cancel_clicked),
                   controller.get());
  g_signal_connect(widgets.entry, "activate",
                   G_CALLBACK(on_inline_prompt_entry_activated),
                   controller.get());
  return controller;
}

cardio::promise<InlinePromptResponse> prompt_inline_async(
    const std::shared_ptr<InlinePromptController> &controller,
    InlinePromptRequest request, cardio::cancellation cancellation) {
  if (controller == nullptr || cancellation.is_cancellation_requested()) {
    co_return InlinePromptResponse{};
  }

  cancel_inline_prompt(controller);
  auto pending = std::make_shared<InlinePromptPendingRequest>();
  pending->source =
      std::make_shared<cardio::promise_source<InlinePromptResponse>>();
  pending->input_required = request.input_required;
  cardio::promise<InlinePromptResponse> response =
      pending->source->get_promise();
  controller->request = pending;

  const std::weak_ptr<InlinePromptController> weak_controller = controller;
  const std::weak_ptr<InlinePromptPendingRequest> weak_pending = pending;
  const std::shared_ptr<cardio::promise_source<InlinePromptResponse>> source =
      pending->source;
  pending->cancellation_registration =
      cancellation.on_cancellation_requested(
          [weak_controller, weak_pending, source]() {
            const std::shared_ptr<InlinePromptController> current_controller =
                weak_controller.lock();
            const std::shared_ptr<InlinePromptPendingRequest> current_pending =
                weak_pending.lock();
            if (current_controller != nullptr && current_pending != nullptr &&
                current_controller->request == current_pending) {
              complete_inline_prompt(current_controller.get(), {});
              return;
            }
            (void)source->try_resolve({});
          });

  gtk_label_set_text(GTK_LABEL(controller->widgets.title_label),
                     request.title.c_str());
  gtk_label_set_text(GTK_LABEL(controller->widgets.message_label),
                     request.message.c_str());
  gtk_button_set_label(GTK_BUTTON(controller->widgets.accept_button),
                       request.accept_label.c_str());
  gtk_button_set_label(GTK_BUTTON(controller->widgets.cancel_button),
                       request.cancel_label.c_str());
  gtk_entry_set_text(GTK_ENTRY(controller->widgets.entry), "");
  gtk_entry_set_visibility(GTK_ENTRY(controller->widgets.entry), request.echo);

  gtk_widget_set_no_show_all(controller->widgets.panel, FALSE);
  gtk_widget_show_all(controller->widgets.panel);
  gtk_widget_set_no_show_all(controller->widgets.panel, TRUE);
  gtk_widget_set_visible(controller->widgets.entry, request.input_required);
  gtk_widget_set_no_show_all(controller->widgets.entry,
                             !request.input_required);
  gtk_widget_set_visible(controller->widgets.cancel_button,
                         request.cancel_visible);
  gtk_widget_set_no_show_all(controller->widgets.cancel_button,
                             !request.cancel_visible);
  gtk_widget_grab_focus(request.input_required
                            ? controller->widgets.entry
                            : controller->widgets.accept_button);

  co_return co_await response;
}

void cancel_inline_prompt(
    const std::shared_ptr<InlinePromptController> &controller) {
  if (controller == nullptr) {
    return;
  }
  complete_inline_prompt(controller.get(), {});
}

} // namespace elder_terms
