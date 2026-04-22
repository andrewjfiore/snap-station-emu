# mupen64plus-input-snapstation (cross-platform)

This directory is the **new home** for the Mupen64Plus input plugin.
Existing authoritative logic remains in `../../src/joybus_snapstation.c`
and `../../src/smart_card.c`; this directory contributes only the
cross-platform glue the plan calls for:

- `control_socket.c` / `control_socket.h` - TCP control surface on
  `127.0.0.1:$SNAP_STATION_CONTROL_PORT` (default `47001`) that lets the
  web-app CI drive the emulator headlessly without pre-canned input
  scripts. Uses POSIX sockets with a Winsock shim; no SDL dependency.

The existing `src/m64p_input_plugin.c` is untouched in this phase. Once
it is audited for Win32-specific calls (Phase 2 follow-up), either its
portable core moves here verbatim or a new plugin entry point calls
into this directory's socket plus the shared joybus/smart_card code.
Until then the plugin builds as before via
`cmake -DWITH_M64P_PLUGIN=ON`.

## Control socket protocol

Line-delimited JSON, one request and one response per line:

| Request | Response | Effect |
|---|---|---|
| `{"cmd": "insert_card", "credits": N}` | `{"ok": true}` | Insert a card with N credits. |
| `{"cmd": "remove_card"}` | `{"ok": true}` | Eject the card. |
| `{"cmd": "get_balance"}` | `{"credits": N}` | Read current balance. |
| `{"cmd": "press_print"}` | `{"ok": true}` | Drive CC/33/5A sequence. |
| `{"cmd": "photo_ready"}` | `{"ok": true}` | Drive one 0x02 capture pulse. |
| `{"cmd": "end_print"}` | `{"ok": true}` | Drive 0x04 end-of-flow. |
| `{"cmd": "peek_flow"}` | `{"flow": "0xNN"}` | Read last 0xC000 state byte. |

Only loopback (127.0.0.1) is accepted; the port is never exposed off the
kiosk host. One active client at a time; reconnecting replaces the prior
session.
