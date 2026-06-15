# AetherSDR direwolf-agwpe subset

This directory contains a monitoring-only AGWPE (AGW Packet Engine) TCP server,
rewritten in Qt C++ for AetherSDR's VHF 1200 baud AX.25 receive path.

Upstream: https://github.com/wb2osz/direwolf
Reference release: Dire Wolf 1.7
Referenced files: `src/server.c`
License: GPL-2.0-or-later (Dire Wolf) — compatible into AetherSDR's GPL-3.0-or-later

The AGW packet-engine TCP protocol (port 8000) is derived from the reference
implementation in `server.c`.  The 36-byte header layout, frame-kind codes, and
exact response payloads for `R`, `G`, `g`, `k`, `K`, `X`, and `x` are taken
directly from that source.  All code was rewritten in idiomatic C++20 / Qt 6.

## Implemented frame kinds

| Kind | Direction | Purpose |
|------|-----------|---------|
| `R`  | ←→        | Version number handshake |
| `G`  | ←→        | Port information |
| `g`  | ←→        | Port capabilities (supplies MaxFrame to wl2k-go) |
| `k`  | →server   | Toggle raw-frame reception |
| `K`  | ←→        | Receive / transmit raw AX.25 frame |
| `m`  | →server   | Toggle monitor frames (flag set; monitor frames not yet sent) |
| `X`  | ←→        | Register callsign (success ACK) |
| `x`  | →server   | Unregister callsign (silently ignored) |
| `P`  | →server   | Application login (silently ignored) |

## NOT implemented — pending AX.25 L2 state machine

| Kind | Purpose |
|------|---------|
| `C`/`v`/`c` | Connect / connect-via / non-standard-PID connect |
| `D`  | Send connected data |
| `d`  | Disconnect |
| `Y`/`y` | Outstanding frames queries (connected-mode flow control) |
| `V`/`M` | Transmit UI/unproto frame (TX path not yet wired) |

Connected mode requires an AX.25 Layer 2 state machine (SABM/UA/I/S/DISC).
This work is tracked separately and will be added once the shared L2 state
machine lands in AetherSDR.

## Refresh notes

1. Review upstream changes to `src/server.c` for protocol additions.
2. Re-derive changed frame kinds into `AgwpeServer.cpp` as needed.
3. Validate against Dire Wolf and wl2k-go (pat) on live APRS audio.
