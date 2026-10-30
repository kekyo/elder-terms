// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <elder-terms/settings/macro-settings.h>

#include <boxrig/gtk3.h>
#include <string_view>
#include <optional>
#include <elder-terms/settings.h>
#include "terminal-connection.h"
#include "terminal-hyperlink-action.h"

namespace elder_terms {

/** Owns a terminal, its model, and the GTK attachment on the GTK thread. */
struct TerminalRuntime;

/** Application integration retained when a failed GTK attachment is replaced. */
struct TerminalRuntimeHostCallbacks {
  /** Installs application input routing, widget identity and UI signal handlers. */
  std::function<void(BoxrigGtk3TerminalWidget *)> attached;
  /** Receives a copied UTF-8 window title for application-specific composition. */
  std::function<void(std::string)> title;
  /** Handles one terminal bell request using the application's audio policy. */
  std::function<void()> bell;
  /** Optional application pointer routing before selection and terminal reporting. */
  std::function<boxrig_input_mouse_route(const boxrig_mouse_event *, const boxrig_ui_hit_test_result *)> mouse = {};
};

/**
 * Selects files and presentation callbacks for an automatically detected transfer.
 * @param direction Requested local transfer direction.
 * @param cancellation Cancel and join any pending dialog when requested.
 * @return Owned transfer request, or no value when selection is cancelled.
 * @remarks Called after widget input stops. Protocol and direction are supplied
 * by the runtime; display and bounded binary reception remain available.
 */
using TerminalZmodemAutoStart = std::function<cardio::promise<std::optional<TerminalTransferRequest>>(
    TerminalTransferDirection direction, cardio::cancellation cancellation)>;

/**
 * Creates a terminal with OpenGL preferred rendering and the standard scroll container.
 * @param window Borrowed top-level window; must outlive the runtime.
 * @param content Borrowed content box, excluding client-side window decorations.
 * @param attachment Borrowed box receiving the standard scroll container; may
 * be content itself or a descendant inside an overlay.
 * @param ambiguous_width Column width fixed for the lifetime of this terminal.
 * @return An owned runtime, or nullptr after a reported initialization failure.
 */
TerminalRuntime *create_terminal_runtime(GtkWindow *window, GtkBox *content, GtkBox *attachment,
    TerminalAmbiguousWidth ambiguous_width);

/**
 * Replaces application hooks for native attachment and terminal host events.
 * @param runtime Live runtime.
 * @param callbacks Hooks valid until replacement or runtime destruction.
 * @remarks Invokes attached immediately for the current widget and again on
 * replacement. With no title hook, window titles are applied to the GTK window.
 * Hooks must not destroy the runtime during the callback.
 */
void terminal_runtime_set_host_callbacks(TerminalRuntime *runtime, TerminalRuntimeHostCallbacks callbacks);

/**
 * Cancels queued frames, destroys the GTK attachment, then releases model and core.
 * @param runtime Owned runtime, or nullptr.
 */
void destroy_terminal_runtime(TerminalRuntime *runtime);

/**
 * Gets the GTK terminal for native focus, accessibility, and portable UI operations.
 * @param runtime Live runtime.
 * @return Borrowed widget; owned by the runtime and its GTK container.
 */
BoxrigGtk3TerminalWidget *terminal_runtime_widget(TerminalRuntime *runtime);

/**
 * Routes an unmodified Escape press to selection cancellation when appropriate.
 * @param runtime Live runtime owning the view and pointer gesture.
 * @param event Normalized native key transition before terminal encoding.
 * @return APPLICATION when cancellation consumes Escape, TERMINAL for IME,
 * modified keys or absent selections, or ERROR if the view cannot be updated.
 * @remarks Lock modifiers do not prevent cancellation. Existing clipboards
 * remain intact; a cancelled held pointer is consumed through its release.
 * The boxrig key router captures subsequent repeat and release transitions.
 */
boxrig_input_key_route terminal_runtime_route_selection_key(TerminalRuntime *runtime, const boxrig_key_event &event);

/**
 * Replaces ordered link rules and the application action handler.
 * @param runtime Live runtime; current recognition is invalidated atomically.
 * @param rules Screen and OSC 8 rules, or an empty list to disable links.
 * @param activate Called on Ctrl+click with the resolved action or rejection.
 * @remarks The callback decides how to launch commands or display errors.
 */
void terminal_runtime_set_hyperlink_rules(TerminalRuntime *runtime,
    std::vector<HyperlinkActionRule> rules, std::function<void(TerminalHyperlinkResolution)> activate);

/**
 * Reports whether the pointer currently resolves to an executable link.
 * @param runtime Live runtime.
 * @return True when the current pointer and recognition revisions are valid.
 */
bool terminal_runtime_has_link_hover(const TerminalRuntime *runtime);

/**
 * Retrieves an owned link target for the terminal context menu.
 * @param runtime Live runtime owning the current recognition rules.
 * @param hit Borrowed current pointer hit from the attached widget.
 * @return Full matched text or OSC 8 URI, or no value when no link is detected.
 * @remarks Disabled recognition and stale hits never produce a target.
 */
std::optional<std::string> terminal_runtime_link_text(TerminalRuntime *runtime, const boxrig_ui_hit_test_result &hit);

/**
 * Replaces received-line macros and discards work from the previous rule set.
 * @param runtime Live communication and presentation owner.
 * @param rules Ordered patterns and actions; an empty list disables macros.
 * @param follow_return_code Normalize expanded sends with the applied Enter policy.
 * @param command Receives an expanded executable and arguments without a shell.
 * @param error Receives compile, expansion, or send diagnostics.
 * @remarks Matching and sending are bounded and cancelled on disconnect. Raw
 * logging and display retain their original receive notification boundaries.
 */
void terminal_runtime_set_macro_rules(TerminalRuntime *runtime, std::vector<MacroRule> rules,
    bool follow_return_code, std::function<void(std::string, std::vector<std::string>)> command,
    std::function<void(std::string)> error);

/**
 * Feeds fixed UTF-8 driver output and schedules presentation.
 * @param runtime Live runtime.
 * @param text Complete, bounded terminal output.
 * @return Whether the complete output was consumed.
 * @remarks Streaming connections use a session transport, not this driver helper.
 */
bool terminal_runtime_feed(TerminalRuntime *runtime, std::string_view text);

/**
 * Requests a visual zoom step and schedules its layout transaction.
 * @param runtime Live runtime.
 * @param direction Requested visual step direction.
 * @return The widget's status; BUSY leaves the request unaccepted.
 */
boxrig_ui_status terminal_runtime_visual_zoom(TerminalRuntime *runtime, boxrig_font_zoom_direction direction);

/**
 * Reports whether the last frame completed its layout and presentation work.
 * @param runtime Live runtime.
 * @return False while native allocation, grid completion, or another frame is pending.
 */
bool terminal_runtime_is_settled(const TerminalRuntime *runtime);

/**
 * Installs a notification after frame processing, for application UI updates.
 * @param runtime Live runtime.
 * @param callback Called on the GTK thread; nullptr disables notification.
 * @param context Borrowed callback state, valid until removed.
 */
void terminal_runtime_set_changed_callback(
    TerminalRuntime *runtime, void (*callback)(void *), void *context);

/** Prepares application-owned presentation data before a component frame.
 * @param runtime Live runtime.
 * @param callback Runs on the GTK thread after received input and model synchronization;
 * an empty function removes it. The callback must not render or destroy the runtime. */
void terminal_runtime_set_prepare_frame_callback(TerminalRuntime *runtime, std::function<void()> callback);

/**
 * Schedules model synchronization and a frame after application-side UI changes.
 * @param runtime Live runtime whose core, font, colors or chrome changed.
 * @remarks This only schedules dispatch; it never reenters rendering.
 */
void terminal_runtime_refresh(TerminalRuntime *runtime);

/**
 * Reports whether a manual connection request can be accepted.
 * @param runtime Live runtime whose pending operations are inspected.
 * @return True when idle, closed, or retaining a channel whose carrier is lost.
 * @remarks A retained channel is closed and joined before its replacement opens.
 */
bool terminal_runtime_can_connect(const TerminalRuntime *runtime);

/**
 * Starts a local command while retaining this runtime's display and history.
 * @param runtime Live runtime.
 * @param command Owned command and communication limits.
 * @return Whether a new connection generation was accepted.
 */
bool terminal_runtime_connect_local(TerminalRuntime *runtime, TerminalLocalCommand command);

/**
 * Starts a remote or serial channel after invalidating old widget input.
 * @param runtime Live runtime without a connected peer or with a lost carrier.
 * @param request Reusable channel factory and stream policy.
 * @return Whether a new connection generation was accepted.
 * @remarks Joins an old carrier-disconnected channel before opening the new one.
 */
bool terminal_runtime_connect_transport(TerminalRuntime *runtime, TerminalTransportRequest request);

/**
 * Requests a normal-state grid and schedules its layout work.
 * @param runtime Live runtime with an active widget.
 * @param rows Preferred nonzero rows.
 * @param columns Preferred nonzero columns.
 * @return Status of accepting the preference; peer completion is asynchronous.
 */
boxrig_ui_status terminal_runtime_set_preferred_grid(TerminalRuntime *runtime, std::size_t rows, std::size_t columns);

/**
 * Requests disconnection and waits for transport and child recovery.
 * @param runtime Runtime kept alive until completion.
 * @return Completion of all borrowed communication work.
 * @remarks Required before destroying a runtime that has started a connection.
 */
cardio::promise<void> terminal_runtime_disconnect(TerminalRuntime *runtime);

/**
 * Gets the current or completed connection snapshot.
 * @param runtime Live runtime.
 * @return Spawn, lifecycle, and child recovery information.
 */
TerminalConnectionState terminal_runtime_connection_state(const TerminalRuntime *runtime);

/**
 * Shares the current shell's authenticated SSH route with a file-transfer client.
 * @param runtime Live runtime.
 * @return Shared transport, or nullptr when no SSH shell is connected.
 * @remarks The caller may retain the share across shell disconnect/reconnect.
 */
std::shared_ptr<AuthenticatedSshTransport> terminal_runtime_authenticated_ssh(const TerminalRuntime *runtime);

/**
 * Installs a receiver for normal output, before session parsing and retries.
 * @param runtime Live runtime.
 * @param callback Borrowed raw and UTF-8 bytes, valid during the call only.
 * @remarks Either span may be empty for an incomplete character or decoder
 * finalization; each byte in either stream is reported exactly once.
 */
void terminal_runtime_set_output_callback(TerminalRuntime *runtime,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> callback);

/**
 * Requests an atomic exchange of Enter, Backspace and cursor-key settings.
 * @param runtime Live runtime.
 * @param settings Desired input settings; encoding belongs to the transport.
 * @remarks A BUSY widget retains its old settings until an input/frame
 * notification permits retry. A newer request replaces a pending request.
 */
void terminal_runtime_request_input_settings(TerminalRuntime *runtime, TerminalTextSettings settings);

/**
 * Reports whether an input-settings exchange is waiting for the widget.
 * @param runtime Live runtime.
 * @return True until a requested exchange is applied or fails.
 */
bool terminal_runtime_input_settings_pending(const TerminalRuntime *runtime);

/**
 * Exchanges live encoding or updates a queued connection/device recovery.
 * @param runtime Runtime with an active or pending connection.
 * @param encoding Desired peer encoding.
 * @return False when inactive or an exchange is already running.
 * @remarks Live exchange waits for widget input to stop. A pending request may
 * be replaced before it starts. Invalid
 * encodings leave the active conversion and connection unchanged.
 */
bool terminal_runtime_request_encoding(TerminalRuntime *runtime, std::string encoding);

/**
 * Reports whether widget inhibition or stream exchange is still pending.
 * @param runtime Live runtime.
 * @return True until the requested encoding exchange finishes.
 */
bool terminal_runtime_encoding_pending(const TerminalRuntime *runtime);

/**
 * Starts a finite text send after standard widget input has stopped.
 * @param runtime Connected runtime without another exclusive operation.
 * @param request Text source, rate, newline policy and presentation callbacks.
 * @return True when the request is accepted.
 * @remarks Current key/newline settings and peer encoding are captured.
 * Display, selection, copying and received output remain available.
 */
bool terminal_runtime_start_text_send(TerminalRuntime *runtime, TerminalTextSendRequest request);

/**
 * Cancels a pending or active text send or binary transfer, then resumes input.
 * @param runtime Live runtime.
 * @return True when there was an operation to cancel.
 */
bool terminal_runtime_cancel_transfer(TerminalRuntime *runtime);

/**
 * Reserves binary receive routing, stops widget input, and runs X/Y/ZMODEM.
 * @param runtime Connected runtime without another exclusive operation.
 * @param request Protocol, local files and presentation callbacks.
 * @return True when the transfer reservation is accepted.
 * @remarks Accepted bytes remain in bounded queues while input stops and old
 * normal output drains. Display and selection remain available throughout.
 */
bool terminal_runtime_start_transfer(TerminalRuntime *runtime, TerminalTransferRequest request);

/**
 * Enables automatic ZMODEM transfer selection or disables it with an empty callback.
 * @param runtime Live runtime.
 * @param select Selects files without blocking the dispatcher; retained across connections.
 */
void terminal_runtime_set_zmodem_auto_start(TerminalRuntime *runtime, TerminalZmodemAutoStart select);

/**
 * Reports whether BREAK can be started on the current connection.
 * @param runtime Live runtime.
 * @return False while another exclusive operation is pending or active.
 */
bool terminal_runtime_can_send_break(const TerminalRuntime *runtime);

/**
 * Sends BREAK after accepted output drains, retaining received output and display.
 * @param runtime Connected runtime with BREAK support.
 * @param request Status and completion callbacks.
 * @return True when accepted; completion follows transport cleanup.
 */
bool terminal_runtime_send_break(TerminalRuntime *runtime, TerminalBreakRequest request);

/**
 * Requests input inhibition or resumption while preserving selection and display.
 * @param runtime Live runtime.
 * @param enabled Whether application policy permits terminal input.
 * @remarks Connections additionally inhibit input while starting or closed.
 * The widget's input state distinguishes accepted STOPPING from DISABLED;
 * transport output may remain pending in either state.
 */
void terminal_runtime_request_input_enabled(TerminalRuntime *runtime, bool enabled);

}
