# elder-terms

## Connection macros

Terminal connection INI files can define ordered macros that watch decoded
terminal output and either send text back to the connection or spawn a
command. The Macro tab in the connection Settings dialog provides the same
editing operations, including adding, removing, and reordering rules and
editing command arguments.

Macros are available for local, TELNET, serial, and SSH terminal connections.
They are not available for SFTP profiles or global settings.

### INI format

Each rule is one `[macro.<id>]` section. Version 1 does not use a separate
`[macro]` section or a `rules` key. The order of the sections is the priority
order: the first matching rule is executed.

Rule IDs must be unique and contain only ASCII letters, digits, `-`, and `_`.
Every rule requires `regex` and exactly one of `send` or `command`.

```ini
[macro.reply_challenge]
regex=^CHALLENGE: (?<token>[A-Za-z0-9]+)$
send=RESPONSE ${token}\r\n

[macro.notify_error]
regex=^ERROR (?<code>\\d+): (?<message>.*)$
command=notify-send
arguments=elder-terms;Error ${code}: ${message};
```

The keys are:

- `regex`: a GLib regular expression matched against the current received
  logical line.
- `send`: a non-empty UTF-8 text template. The expanded text uses the active
  terminal character encoding and the same input path as keyboard input.
- `command`: a non-empty executable name or path template. It is spawned
  directly without a shell, with `PATH` lookup enabled.
- `arguments`: an optional GLib key-file string list for a command action.
  Each item is one argument; shell quoting, redirection, and pipelines are not
  interpreted. `arguments` cannot be used with `send`.

GLib key-file escaping applies before macro expansion. For example, `\r\n` in
a `send` value becomes CRLF, while `\\d` is needed to pass `\d` to the regular
expression.

### Capture expansion

The action templates support these substitutions:

- `${0}`: the complete regular-expression match.
- `${1}`, `${2}`, and so on: numbered capture groups.
- `${name}`: a named capture group such as `(?<name>...)`.
- `$$`: a literal dollar sign.

Unknown or malformed capture references make the rule invalid. Invalid rules
are ignored with a warning when the settings are loaded.

### Matching and execution

Matching uses terminal output after character decoding but retains terminal
control and ANSI escape sequences. A rule is evaluated as received text is
added, so an action can run before LF arrives. Matching never crosses an LF
line boundary, and a CR immediately before LF is omitted from the line.

Only one action can run for each logical line. Rules are checked from top to
bottom whenever the current line changes; after one runs, no other rule is
evaluated until the next LF. The current line is limited to 1 MiB, with the
oldest text discarded when the limit is exceeded. No line buffer is maintained
when there are no rules.

A command action reports a message box only when the process cannot be
spawned, for example when its executable does not exist. A process that starts
successfully and later exits with a non-zero status is not reported.

Applying connection Settings replaces the active rules immediately and clears
the partially collected line. Macros are not inherited from `global.ini`. A
successfully loaded startup INI (`-s`) replaces all macros from the connection
INI (`-c`), including replacing them with an empty set; if the startup INI
cannot be loaded, the connection macros remain active.
