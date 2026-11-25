// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

#pragma once

#include <boxrig/posix.h>
#include <cardio.h>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "terminal-transport.h"
#include "terminal-text-send.h"
#include "terminal-break.h"

namespace elder_terms {

/** Connection lifecycle, independent of terminal display ownership. */
enum class TerminalConnectionPhase {
  /** No command has been accepted yet. */
  idle,
  /** Parent preparation or child startup is pending. */
  starting,
  /** The channel is open; carrier_connected determines whether it accepts input. */
  connected,
  /** Cancellation or failure is recovering the old channel. */
  stopping,
  /** The channel and child have been recovered; another start is permitted. */
  closed
};

/** An owned local command and bounded communication limits. */
struct TerminalLocalCommand {
  /** Executable followed by its arguments; execvp search semantics apply. */
  std::vector<std::string> arguments;
  /** TERM supplied only to the child process. */
  std::string terminal_type = "xterm-256color";
  /** Maximum bytes retained in each application queue, from 1 through 1 MiB. */
  std::size_t queue_capacity = 64 * 1024;
  /** Maximum bytes in one OS read or write, also bounded by queue capacity. */
  std::size_t io_chunk_size = 4096;
  /** iconv encoding at the peer boundary; the core always receives UTF-8. */
  std::string encoding = "UTF-8";
  /** Observes completed local I/O; true denotes outgoing bytes. */
  std::function<void(bool outgoing)> activity = {};
};

/** One prepared local PTY handoff; the caller owns and closes master_fd. */
struct TerminalLocalHandoff {
  /** Versioned boxrig terminal image, including unsent core output. */
  std::vector<std::uint8_t> checkpoint;
  /** Whether the old session had accepted peer EOF. */
  bool input_finished = false;
  /** Duplicate of the PTY master for SCM_RIGHTS transfer. */
  int master_fd = -1;
  /** Original shell PID; only its spawning process can reap it. */
  pid_t child = 0;
  /** Capacity of each bounded application queue. */
  std::size_t queue_capacity = 0;
  /** Maximum operating-system I/O chunk size. */
  std::size_t io_chunk_size = 0;
};

/** Final child status collected by the original spawning process. */
struct TerminalLocalChildExit {
  /** Original child PID. */
  pid_t child = 0;
  /** Normal exit code, or -1 when unavailable. */
  int exit_code = -1;
  /** Terminating signal, or zero. */
  int signal_number = 0;
  /** waitpid errno, or zero after successful reaping. */
  int error = 0;
};

/** Independent child reaper retained after the source terminal is destroyed. */
struct TerminalLocalChildReaper;

/** Snapshot of a paused nonlocal channel for a remote transport proxy. */
struct TerminalTransportHandoff {
  /** Versioned terminal image. */
  std::vector<std::uint8_t> checkpoint;
  /** Whether terminal input has already reached EOF. */
  bool input_finished = false;
  /** Peer-side text encoding. */
  std::string encoding;
  /** Application queue capacity. */
  std::size_t queue_capacity = 0;
  /** Maximum source I/O chunk. */
  std::size_t io_chunk_size = 0;
  /** Duplicate of the full-duplex proxy data socket, owned by the caller. */
  int data_fd = -1;
  /** Duplicate of the resize/BREAK control socket, owned by the caller. */
  int control_fd = -1;
};

/** Original live channel retained independently of the old terminal. */
struct TerminalTransportOrigin;

/** Snapshot separating spawn diagnostics, process exit, and resource recovery. */
struct TerminalConnectionState {
  /** Monotonically increasing accepted connection number. */
  std::uint64_t generation = 0;
  /** Current connection lifecycle. */
  TerminalConnectionPhase phase = TerminalConnectionPhase::idle;
  /** Parent preparation or child setup/exec diagnostic. */
  boxrig_posix_pty_spawn_result spawn{};
  /** Owned child PID, retained in the final report after reaping. */
  pid_t child = 0;
  /** Normal exit code, or -1 when unavailable. */
  int exit_code = -1;
  /** Terminating signal, or zero. */
  int signal_number = 0;
  /** Communication or observation errno, separate from spawn errno. */
  int operation_error = 0;
  /** Owned transport diagnostic, including authentication or device details. */
  std::string operation_error_message;
  /** Whether cancellation was requested for this generation. */
  bool cancelled = false;
  /** Whether the sole child reaper completed. */
  bool reaped = false;
  /** Whether the peer reader has ended, before buffered input and services finish. */
  bool receive_ended = false;
  /** Whether the optional carrier permits sending on the open channel. */
  bool carrier_connected = true;
  /** Carrier drops in this generation, including drops restored before a frame. */
  std::uint64_t carrier_losses = 0;
  /** Received bytes currently belong to a reserved binary transfer, not terminal text. */
  bool binary_receive = false;
  /** Largest observed application receive queue occupancy. */
  std::size_t receive_peak = 0;
  /** Largest observed application send queue occupancy. */
  std::size_t send_peak = 0;
  /** Currently applied peer encoding; empty before conversion is prepared. */
  std::string encoding;
};

/** Owns a boxrig session and its asynchronous transport on one dispatcher. */
struct TerminalConnection;

/**
 * Creates a communication owner without starting a command.
 * @param terminal Borrowed core, retained across connection generations.
 * @param wake Requests a future dispatch/frame; must not dispatch recursively.
 * @param output Receives each raw/cooked chunk once, before session retries.
 * @return Owned connection.
 * @remarks All functions and callbacks run on the core's Cardio dispatcher.
 * Cooked bytes may be empty for a partial encoded character; raw bytes may
 * be empty when finalizing the decoder at EOF or cancellation.
 */
TerminalConnection *create_terminal_connection(
    boxrig_terminal *terminal, std::function<void()> wake,
    std::function<void(std::span<const unsigned char>, std::span<const unsigned char>)> output);

/**
 * Destroys an inactive connection.
 * @param connection Owned connection, or nullptr.
 * @remarks Stop and await wait_stopped before destruction or core release.
 */
void destroy_terminal_connection(TerminalConnection *connection);

/**
 * Starts one local command using the currently confirmed core grid.
 * @param connection Inactive communication owner.
 * @param command Owned arguments and queue limits.
 * @return False while previous resources are still being recovered or limits are invalid.
 */
bool terminal_connection_start_local(TerminalConnection *connection, TerminalLocalCommand command);

/**
 * Pauses a UTF-8 local connection at a boxrig checkpoint boundary.
 * @param connection Connected local owner; callers must inhibit new terminal input.
 * @return Handoff with a duplicated master FD, or nullopt if the state is busy.
 * @remarks On failure the connection resumes automatically. The returned FD is
 * owned by the caller, including after commit or rollback.
 */
cardio::promise<std::optional<TerminalLocalHandoff>> terminal_connection_prepare_local_handoff(
    TerminalConnection *connection);

/** Restores normal I/O after an uncommitted local handoff. */
void terminal_connection_rollback_local_handoff(TerminalConnection *connection);

/**
 * Ends source I/O after the destination has acknowledged the handoff.
 * @return Owned reaper for the original spawning process, or nullptr when a
 * previously received descriptor is moved again or no handoff was prepared.
 * The caller must await and destroy a nonnull reaper.
 * @remarks The spawning process must keep its dispatcher alive until reaping
 * completes, even if its last GTK window has closed.
 */
TerminalLocalChildReaper *terminal_connection_commit_local_handoff(TerminalConnection *connection);

/** Awaits pidfd readiness and reaps the original shell without retaining the old terminal. */
cardio::promise<TerminalLocalChildExit> terminal_local_child_reaper_wait(TerminalLocalChildReaper *reaper);

/** Releases a reaper after terminal_local_child_reaper_wait has completed. */
void destroy_terminal_local_child_reaper(TerminalLocalChildReaper *reaper);

/**
 * Starts direct PTY I/O over a transferred descriptor and imported terminal.
 * @param connection Inactive owner borrowing the already imported core.
 * @param master_fd Nonblocking received FD; ownership transfers only on success.
 * @param input_finished EOF flag from the handoff.
 * @param child Original child PID, monitored but never reaped here.
 * @param queue_capacity Bounded queue size from the source.
 * @param io_chunk_size Maximum I/O chunk size from the source.
 * @return True if the descriptor was adopted.
 */
bool terminal_connection_resume_local(TerminalConnection *connection, int master_fd,
    bool input_finished, pid_t child, std::size_t queue_capacity, std::size_t io_chunk_size);

/** Pauses a connected SSH or other nonlocal transport at a checkpoint boundary. */
cardio::promise<std::optional<TerminalTransportHandoff>> terminal_connection_prepare_transport_handoff(
    TerminalConnection *connection);

/** Restores the old nonlocal session after a failed handoff. */
void terminal_connection_rollback_transport_handoff(TerminalConnection *connection);

/**
 * Commits a prepared nonlocal handoff and returns the original channel.
 * @remarks Returns nullptr after a proxy connection is moved again; the
 * original owner keeps its existing handle, so no relay hop is added.
 */
cardio::promise<TerminalTransportOrigin *> terminal_connection_commit_transport_handoff(
    TerminalConnection *connection);

/** Starts a resumed terminal over an already opened proxy channel. */
bool terminal_connection_resume_transport(TerminalConnection *connection, int data_fd,
    int control_fd, bool input_finished, std::string encoding,
    std::size_t queue_capacity, std::size_t io_chunk_size);

/** Shares the SSH connection used by a still-live origin channel. */
std::shared_ptr<AuthenticatedSshTransport> terminal_transport_origin_authenticated_ssh(
    TerminalTransportOrigin *origin);

/** Closes the original channel after destination tabs and transfers finish. */
cardio::promise<void> terminal_transport_origin_close(TerminalTransportOrigin *origin);

/** Waits for the last proxy endpoint to close, then recovers the original channel. */
cardio::promise<void> terminal_transport_origin_wait(TerminalTransportOrigin *origin);

/** Releases an origin after terminal_transport_origin_close completes. */
void destroy_terminal_transport_origin(TerminalTransportOrigin *origin);

/**
 * Starts a channel factory using the confirmed core grid and bounded queues.
 * @param connection Inactive communication owner.
 * @param request Owned factory and stream policy.
 * @return False if active, the factory is empty, or queue limits are invalid.
 */
bool terminal_connection_start_transport(TerminalConnection *connection, TerminalTransportRequest request);

/**
 * Cancels the current generation, including a pending synchronous spawn.
 * @param connection Communication owner.
 * @remarks Completion is reported only after asynchronous I/O and child recovery.
 */
void terminal_connection_stop(TerminalConnection *connection);

/**
 * Awaits recovery of the current operation without blocking the dispatcher.
 * @param connection Owner kept alive through completion.
 * @return Completion of all work borrowing the connection and core.
 */
cardio::promise<void> terminal_connection_wait_stopped(TerminalConnection *connection);

/** Separates session continuation from input that can change the terminal image. */
struct TerminalConnectionProgress {
  /** Another immediate dispatch can make progress. */
  bool dispatch_pending = false;
  /** Received bytes were accepted by the terminal parser during this dispatch. */
  bool input_received = false;
};

/**
 * Advances one bounded session dispatch after a readiness notification.
 * @param connection Communication owner.
 * @return Continuation and received-input progress, independent of outgoing writes.
 */
TerminalConnectionProgress terminal_connection_dispatch(TerminalConnection *connection);

/**
 * Applies backpressure to normal receive delivery while retaining transmit progress.
 * @param connection Communication owner.
 * @param enabled False keeps decoded bytes in the finite receive queue.
 * @remarks Used while received macros retain earlier notifications. This does
 * not pause outgoing writes or binary-transfer reads. Stop always reopens it.
 */
void terminal_connection_set_receive_delivery_enabled(TerminalConnection *connection, bool enabled);

/**
 * Awaits one grid request at the peer before applying it to the core.
 * @param connection Communication owner.
 * @param rows Nonzero rows.
 * @param columns Nonzero columns.
 * @return False on transport or core failure; the connection then stops.
 */
cardio::promise<bool> terminal_connection_resize(TerminalConnection *connection, std::size_t rows, std::size_t columns);

/**
 * Exchanges the stream converters after draining already accepted output.
 * @param connection Opening or connected owner, retained until completion.
 * @param encoding Desired iconv encoding name.
 * @return False when inactive, busy, cancelled, or the encoding is unavailable.
 * @remarks An opening connection replaces its unused converters directly.
 * The caller inhibits user input first. The receive boundary is the
 * last completed transport read. Its partial character is finalized once;
 * neither display state nor connection generation is replaced.
 */
cardio::promise<bool> terminal_connection_change_encoding(TerminalConnection *connection, std::string encoding);

/**
 * Sends finite text after existing normal output has reached the peer.
 * @param connection Connected owner, retained until completion.
 * @param request Source, newline policy, rate, and progress/status callbacks.
 * @param cancellation Caller cancellation, combined with connection shutdown.
 * @return True only when every encoded payload byte has been sent.
 * @remarks The runtime owns input inhibition and active/finished callbacks.
 * Peer encoding is captured from the active connection. Normal receive and
 * logging continue; normal replies wait until this send finishes or cancels.
 */
cardio::promise<bool> terminal_connection_send_text(TerminalConnection *connection,
    TerminalTextSendRequest request, cardio::cancellation cancellation);

/**
 * Reserves binary receive routing before the runtime stops widget input.
 * @param connection Connected owner without another exclusive operation.
 * @return True when subsequent peer bytes belong to the bounded transfer queue.
 * @remarks Complete every accepted reservation with terminal_connection_transfer,
 * including cancellation. Normal output can drain while input stops.
 */
bool terminal_connection_prepare_transfer(TerminalConnection *connection);

/**
 * Runs a reserved binary transfer after user input is disabled.
 * @param connection Owner with an accepted transfer reservation.
 * @param request Protocol, files and presentation callbacks.
 * @param cancellation Caller cancellation, combined with connection shutdown.
 * @return True on complete transfer success.
 * @remarks Binary bytes bypass conversion, logs and the terminal parser.
 * Successful completion returns prefetched normal output to the same stream.
 * X/YMODEM send can consume the latest isolated start control received in the
 * preceding five seconds, without duplicating its normal display/log output.
 * The runtime owns active/finished callbacks and input resumption.
 */
cardio::promise<bool> terminal_connection_transfer(TerminalConnection *connection,
    TerminalTransferRequest request, cardio::cancellation cancellation);

/**
 * Configures raw ZMODEM header detection for subsequent normal input.
 * @param connection Communication owner.
 * @param start Reserves a transfer synchronously and returns true if accepted;
 * an empty callback disables detection.
 * @remarks The detector holds at most 17 partial header bytes. On acceptance,
 * the matched header and current read are retained for transfer replay, in
 * addition to the bounded receive queue. Rejected headers remain normal data.
 */
void terminal_connection_set_zmodem_auto_start(TerminalConnection *connection,
    std::function<bool(TerminalTransferDirection)> start);

/**
 * Reports whether the connected transport can accept a BREAK operation.
 * @param connection Communication owner.
 * @return False for unsupported, inactive or busy transports.
 */
bool terminal_connection_can_send_break(const TerminalConnection *connection);

/**
 * Drains accepted output, sends BREAK, and resumes normal sending.
 * @param connection Connected owner, retained until completion.
 * @param request Status callback; the runtime owns the finished callback.
 * @return True after transport BREAK completion and cleanup.
 * @remarks Disconnect cancels and joins this operation before closing the channel.
 */
cardio::promise<bool> terminal_connection_send_break(TerminalConnection *connection, TerminalBreakRequest request);

/**
 * Gets an observable lifecycle snapshot.
 * @param connection Communication owner.
 * @return Current or most recently completed generation's status.
 */
TerminalConnectionState terminal_connection_state(const TerminalConnection *connection);

/**
 * Shares the authenticated SSH transport of the connected shell.
 * @param connection Communication owner.
 * @return Shared transport, or nullptr for inactive/non-SSH connections.
 * @remarks An SFTP client retaining this share can outlive the shell channel.
 */
std::shared_ptr<AuthenticatedSshTransport> terminal_connection_authenticated_ssh(const TerminalConnection *connection);

}
