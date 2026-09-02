#include "inline-prompt.h"

#include <memory>
#include <stdexcept>
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
static constexpr const char *inline_prompt_monospace_message_style_class =
    "inline-prompt-monospace-message";
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
    "}"
    ".inline-prompt-monospace-message {"
    "  color: #ffffff;"
    "  font-family: monospace;"
    "}";

struct InlinePromptPendingRequest {
  std::shared_ptr<cardio::promise_source<InlinePromptResponse>> source;
  cardio::cancellation_registration cancellation_registration;
  bool input_required = false;
  bool secondary_input_required = false;
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
  if (widgets.monospace_message_label != nullptr) {
    GtkStyleContext *monospace_message_context =
        gtk_widget_get_style_context(widgets.monospace_message_label);
    gtk_style_context_add_class(
        monospace_message_context,
        inline_prompt_monospace_message_style_class);
    gtk_style_context_add_provider(
        monospace_message_context, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  for (GtkWidget *label : {widgets.entry_label,
                           widgets.secondary_entry_label}) {
    if (label == nullptr) {
      continue;
    }
    GtkStyleContext *context = gtk_widget_get_style_context(label);
    gtk_style_context_add_class(context,
                                inline_prompt_message_style_class);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
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

  GtkWidget *monospace_message = gtk_label_new("");
  gestament_gtk_assign_accessible_id(
      monospace_message,
      accessible_id(accessible_id_prefix,
                    "monospace_message_label").c_str());
  gtk_label_set_xalign(GTK_LABEL(monospace_message), 0.0F);
  gtk_label_set_selectable(GTK_LABEL(monospace_message), TRUE);
  gtk_widget_set_direction(monospace_message, GTK_TEXT_DIR_LTR);
  gtk_widget_set_visible(monospace_message, FALSE);
  gtk_widget_set_no_show_all(monospace_message, TRUE);
  gtk_box_pack_start(GTK_BOX(content), monospace_message, FALSE, TRUE, 0);

  GtkWidget *entry_label = gtk_label_new("");
  gestament_gtk_assign_accessible_id(
      entry_label,
      accessible_id(accessible_id_prefix, "entry_label").c_str());
  gtk_label_set_xalign(GTK_LABEL(entry_label), 0.0F);
  gtk_widget_set_visible(entry_label, FALSE);
  gtk_widget_set_no_show_all(entry_label, TRUE);
  gtk_box_pack_start(GTK_BOX(content), entry_label, FALSE, TRUE, 0);

  GtkWidget *entry = gtk_entry_new();
  gestament_gtk_assign_accessible_id(
      entry, accessible_id(accessible_id_prefix, "entry").c_str());
  gtk_widget_set_visible(entry, FALSE);
  gtk_widget_set_no_show_all(entry, TRUE);
  gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, TRUE, 0);

  GtkWidget *secondary_entry_label = gtk_label_new("");
  gestament_gtk_assign_accessible_id(
      secondary_entry_label,
      accessible_id(accessible_id_prefix,
                    "secondary_entry_label").c_str());
  gtk_label_set_xalign(GTK_LABEL(secondary_entry_label), 0.0F);
  gtk_widget_set_visible(secondary_entry_label, FALSE);
  gtk_widget_set_no_show_all(secondary_entry_label, TRUE);
  gtk_box_pack_start(GTK_BOX(content), secondary_entry_label,
                     FALSE, TRUE, 0);

  GtkWidget *secondary_entry = gtk_entry_new();
  gestament_gtk_assign_accessible_id(
      secondary_entry,
      accessible_id(accessible_id_prefix, "secondary_entry").c_str());
  gtk_widget_set_visible(secondary_entry, FALSE);
  gtk_widget_set_no_show_all(secondary_entry, TRUE);
  gtk_entry_set_visibility(GTK_ENTRY(secondary_entry), FALSE);
  gtk_entry_set_activates_default(GTK_ENTRY(secondary_entry), TRUE);
  gtk_widget_set_hexpand(secondary_entry, TRUE);
  gtk_box_pack_start(GTK_BOX(content), secondary_entry, FALSE, TRUE, 0);

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

  GtkWidget *alternative = gtk_button_new_with_label("");
  gestament_gtk_assign_accessible_id(
      alternative,
      accessible_id(accessible_id_prefix, "alternative_button").c_str());
  gtk_widget_set_visible(alternative, FALSE);
  gtk_widget_set_no_show_all(alternative, TRUE);
  gtk_container_add(GTK_CONTAINER(actions), alternative);
  gtk_box_reorder_child(GTK_BOX(actions), alternative, 1);

  InlinePromptWidgets widgets{
      .panel = panel,
      .background = background,
      .title_label = title,
      .message_label = message,
      .monospace_message_label = monospace_message,
      .entry_label = entry_label,
      .entry = entry,
      .secondary_entry_label = secondary_entry_label,
      .secondary_entry = secondary_entry,
      .cancel_button = cancel,
      .accept_button = accept,
      .alternative_button = alternative,
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
  if (controller->widgets.monospace_message_label != nullptr) {
    gtk_label_set_text(
        GTK_LABEL(controller->widgets.monospace_message_label), "");
    gtk_widget_set_visible(controller->widgets.monospace_message_label,
                           FALSE);
    gtk_widget_set_no_show_all(controller->widgets.monospace_message_label,
                               TRUE);
  }
  if (controller->widgets.entry_label != nullptr) {
    gtk_widget_set_visible(controller->widgets.entry_label, FALSE);
    gtk_widget_set_no_show_all(controller->widgets.entry_label, TRUE);
  }
  if (controller->widgets.secondary_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(controller->widgets.secondary_entry), "");
    gtk_widget_set_visible(controller->widgets.secondary_entry, FALSE);
    gtk_widget_set_no_show_all(controller->widgets.secondary_entry, TRUE);
  }
  if (controller->widgets.secondary_entry_label != nullptr) {
    gtk_widget_set_visible(controller->widgets.secondary_entry_label, FALSE);
    gtk_widget_set_no_show_all(controller->widgets.secondary_entry_label,
                               TRUE);
  }
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
  std::string secondary_text;
  if (controller->request->input_required) {
    const char *entry_text =
        gtk_entry_get_text(GTK_ENTRY(controller->widgets.entry));
    text = entry_text == nullptr ? std::string() : std::string(entry_text);
  }
  if (controller->request->secondary_input_required) {
    const char *entry_text = gtk_entry_get_text(
        GTK_ENTRY(controller->widgets.secondary_entry));
    secondary_text =
        entry_text == nullptr ? std::string() : std::string(entry_text);
  }
  complete_inline_prompt(
      controller,
      {
          .accepted = true,
          .text = std::move(text),
          .secondary_text = std::move(secondary_text),
      });
}

static void on_inline_prompt_cancel_clicked(GtkButton *, gpointer data) {
  complete_inline_prompt(static_cast<InlinePromptController *>(data), {});
}

static void on_inline_prompt_alternative_clicked(GtkButton *, gpointer data) {
  complete_inline_prompt(
      static_cast<InlinePromptController *>(data),
      {
          .accepted = false,
          .text = {},
          .secondary_text = {},
          .alternative = true,
      });
}

static void on_inline_prompt_entry_activated(GtkEntry *entry, gpointer data) {
  auto *controller = static_cast<InlinePromptController *>(data);
  if (controller == nullptr || controller->request == nullptr) {
    return;
  }
  if (entry == GTK_ENTRY(controller->widgets.entry) &&
      controller->request->secondary_input_required) {
    gtk_widget_grab_focus(controller->widgets.secondary_entry);
    return;
  }
  on_inline_prompt_accept_clicked(nullptr, controller);
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
  if (widgets.alternative_button != nullptr) {
    g_signal_connect(widgets.alternative_button, "clicked",
                     G_CALLBACK(on_inline_prompt_alternative_clicked),
                     controller.get());
  }
  g_signal_connect(widgets.entry, "activate",
                   G_CALLBACK(on_inline_prompt_entry_activated),
                   controller.get());
  if (widgets.secondary_entry != nullptr) {
    g_signal_connect(widgets.secondary_entry, "activate",
                     G_CALLBACK(on_inline_prompt_entry_activated),
                     controller.get());
  }
  return controller;
}

cardio::promise<InlinePromptResponse> prompt_inline_async(
    const std::shared_ptr<InlinePromptController> &controller,
    InlinePromptRequest request, cardio::cancellation cancellation) {
  if (controller == nullptr || cancellation.is_cancellation_requested()) {
    co_return InlinePromptResponse{};
  }
  if (request.alternative_visible &&
      controller->widgets.alternative_button == nullptr) {
    throw std::invalid_argument(
        "Inline prompt alternative button is unavailable");
  }
  if (!request.monospace_message.empty() &&
      controller->widgets.monospace_message_label == nullptr) {
    throw std::invalid_argument(
        "Inline prompt monospace message label is unavailable");
  }
  if (request.secondary_input_required &&
      controller->widgets.secondary_entry == nullptr) {
    throw std::invalid_argument(
        "Inline prompt secondary entry is unavailable");
  }

  cancel_inline_prompt(controller);
  auto pending = std::make_shared<InlinePromptPendingRequest>();
  pending->source =
      std::make_shared<cardio::promise_source<InlinePromptResponse>>();
  pending->input_required = request.input_required;
  pending->secondary_input_required = request.secondary_input_required;
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
  if (controller->widgets.monospace_message_label != nullptr) {
    gtk_label_set_text(
        GTK_LABEL(controller->widgets.monospace_message_label),
        request.monospace_message.c_str());
  }
  gtk_button_set_label(GTK_BUTTON(controller->widgets.accept_button),
                       request.accept_label.c_str());
  gtk_button_set_label(GTK_BUTTON(controller->widgets.cancel_button),
                       request.cancel_label.c_str());
  if (controller->widgets.alternative_button != nullptr) {
    gtk_button_set_label(GTK_BUTTON(controller->widgets.alternative_button),
                         request.alternative_label.c_str());
  }
  gtk_entry_set_text(GTK_ENTRY(controller->widgets.entry),
                     request.initial_text.c_str());
  gtk_entry_set_visibility(GTK_ENTRY(controller->widgets.entry), request.echo);
  gtk_entry_set_activates_default(GTK_ENTRY(controller->widgets.entry),
                                  !request.secondary_input_required);
  if (controller->widgets.entry_label != nullptr) {
    gtk_label_set_text(GTK_LABEL(controller->widgets.entry_label),
                       request.input_label.c_str());
  }
  if (controller->widgets.secondary_entry != nullptr) {
    gtk_entry_set_text(GTK_ENTRY(controller->widgets.secondary_entry),
                       request.secondary_initial_text.c_str());
    gtk_entry_set_visibility(GTK_ENTRY(controller->widgets.secondary_entry),
                             request.secondary_echo);
  }
  if (controller->widgets.secondary_entry_label != nullptr) {
    gtk_label_set_text(GTK_LABEL(controller->widgets.secondary_entry_label),
                       request.secondary_input_label.c_str());
  }

  gtk_widget_set_no_show_all(controller->widgets.panel, FALSE);
  gtk_widget_show_all(controller->widgets.panel);
  gtk_widget_set_no_show_all(controller->widgets.panel, TRUE);
  if (controller->widgets.monospace_message_label != nullptr) {
    const bool visible = !request.monospace_message.empty();
    gtk_widget_set_visible(controller->widgets.monospace_message_label,
                           visible);
    gtk_widget_set_no_show_all(controller->widgets.monospace_message_label,
                               !visible);
  }
  gtk_widget_set_visible(controller->widgets.entry, request.input_required);
  gtk_widget_set_no_show_all(controller->widgets.entry,
                             !request.input_required);
  if (controller->widgets.entry_label != nullptr) {
    const bool visible =
        request.input_required && !request.input_label.empty();
    gtk_widget_set_visible(controller->widgets.entry_label, visible);
    gtk_widget_set_no_show_all(controller->widgets.entry_label, !visible);
  }
  if (controller->widgets.secondary_entry != nullptr) {
    gtk_widget_set_visible(controller->widgets.secondary_entry,
                           request.secondary_input_required);
    gtk_widget_set_no_show_all(controller->widgets.secondary_entry,
                               !request.secondary_input_required);
  }
  if (controller->widgets.secondary_entry_label != nullptr) {
    const bool visible = request.secondary_input_required &&
                         !request.secondary_input_label.empty();
    gtk_widget_set_visible(controller->widgets.secondary_entry_label,
                           visible);
    gtk_widget_set_no_show_all(controller->widgets.secondary_entry_label,
                               !visible);
  }
  gtk_widget_set_visible(controller->widgets.cancel_button,
                         request.cancel_visible);
  gtk_widget_set_no_show_all(controller->widgets.cancel_button,
                             !request.cancel_visible);
  if (controller->widgets.alternative_button != nullptr) {
    gtk_widget_set_visible(controller->widgets.alternative_button,
                           request.alternative_visible);
    gtk_widget_set_no_show_all(controller->widgets.alternative_button,
                               !request.alternative_visible);
  }
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
