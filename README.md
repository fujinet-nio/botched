# Botched!

Server-authoritative, deterministic multiplayer factory game for FujiNet-NIO clients.
Rules: [`docs/botched-game-spec-v1.md`](docs/botched-game-spec-v1.md) (authoritative).
Where the spec was silent: [`docs/rules-interpretations.md`](docs/rules-interpretations.md).

## Status

**M0 (pure rules engine) is complete and accepted** (see `docs/m0-hardening.md`): a deterministic,
headless C++20 rules library, a transport-agnostic `GameServer`, a stdio JSON host and a POSIX
reference simulator, covered by tests for all 14 acceptance categories of the spec (clean under
ASan/UBSan; identical results in Debug and Release).

**M1-A (minimal playable POSIX multiplayer) is built:** a TCP server and an interactive terminal
client play a complete 2-4 player match (see "Play a match" below). **B3 is accepted:** an optional binary TCP listener and Linux terminal/SDL client
mode share the existing server and UI. Binary sessions support transient disconnect recovery.
**B4 is accepted:** a portable C99 transport uses the existing FujiNet-NIO
NetworkDevice TCP service; see [B4 integration and verification](docs/binary-v1/b4-review.md).
**B5 measurements are ready for review:** [6502 costs and full RAM accounting](docs/binary-v1/b5-review.md).
The codec/storage harness fits K4; the current complete general-NIO configuration needs K8
for shipped maps. Historical CPU targets are not met.
**Atari feasibility spike:** [compact stream transport, thin decoder and measured full-client budget](docs/atari-feasibility.md).
The headless prototype reuses `fn_stream_*` assembly and fits K4 on shipped maps; physical hardware and graphics remain unverified.
**Amiga A1c:** [Mixed-client reliability acceptance](docs/amiga/a1c-review.md) covers gameplay, exact request recovery and cooperative shutdown on a stock A500/KS1.3 with 512 KB Chip + 512 KB slow expansion RAM. Broker blocking and unconfirmed remote Close limits are documented.
**Amiga A2a:** [Graphical tiles, dirty-region rendering and authoritative event animation](docs/amiga/a2a-review.md)
adds a 16-colour PAL presentation on the same minimum A500, with screenshots and measured resources.
**Amiga A2b slice 1:** [UI/viewport review and minimum-machine evidence](docs/amiga/a2b-slice1-review.md). Four-character labels, two-digit scores with exact-value access, a visible command palette, larger tiles, inspection and priority preview. The [interaction/hazard correction](docs/amiga/a2b-slice1-corrections.md) adds mouse input, empty orders, persistent hazard guides and faster cached panning. [Slice 2](docs/amiga/a2b-slice2-review.md) adds authoritative captions/history, live priority/machinery context and deliberate continuous movement/pushes.
**Not built yet:** complete retro gameplay clients, physical constrained-client acceptance, authentication beyond per-seat session tokens,
and host persistence. See [`docs/binary-protocol-requirements.md`](docs/binary-protocol-requirements.md).

## Amiga Workbench playtesting

From the workspace root, with an existing WB3.2 A500 run disk configured:

```sh
source scripts/env.sh
make -C external/botched -f tools/amiga/Makefile
AMIBERRY_EXTRA_ARGS="-s 'filesystem2=rw,DH2:BT:$NIO_WORKSPACE/external/botched/build/amiga,-128'" \
  scripts/build.sh amiga-workbench --profile wb32-a500 -- --tcp
```

This optional accelerated test profile uses a 68030; the minimum supported game
machine remains the A500/68000/KS1.3 profile. Use `wb32-a1200` for the existing
A1200/030 Workbench profile instead. The final **`-128`** makes the game share
non-bootable. With `0`, Amiberry can boot the share instead of Workbench, leaving
`SYS:`, `C:` and `DEVS:` pointing at `BT:`: DOS commands disappear and NIO cannot
open `serial.device` (Botched error 102).

In the Workbench Shell:

```text
NIO:fujinet-load-resident NIO:fujinet-nio.device fujinet-nio.device
NIO:fujiinfo
Stack 16384
BT:botched-amiga tcp://127.0.0.1:7879 m1
```

Amiga controls: Up/Down browses the nine labelled commands; Space puts the chosen
command in the selected empty slot (or the next empty slot). Enter also places;
Tab or Left/Right selects a slot; 1–9 are direct shortcuts. All slots start empty:
WAIT must be selected explicitly. Backspace clears the selected slot. **L** or
clicking **LOCK** submits only when all three slots are filled. Click a command to
place it, or a programme slot to select/clear it. Click the board to inspect;
right-click closes inspection or a panel. Mouse and keyboard share the same editor.
`V` toggles the full-map overview, `I` enables cursor inspection/panning, `F1`
shows the field guide, and `F2` shows exact scores. `H` opens the last 16 captions;
Up/Down scrolls older/newer entries while replay continues. Escape closes an overlay or
inspection first, then quits from the main view. `PLY1`–`PLY4` are local display
labels; `++` means a score above 99, never a capped authoritative score.

Start the Botched server with its binary listener first and substitute a current
match ID. `fujiinfo` should report the POSIX firmware/profile. The launcher bridges
Amiberry serial I/O to host NIO; NIO makes the TCP connection, so `127.0.0.1` refers
to that host, and an Amiga TCP/IP stack is unnecessary. A successful resident load
alone does not verify serial communication. On transport failure the game prints
broker diagnostics to its launching Shell (visible after Escape).

## Play a match (2-4 players, separate terminals)

Build once (`cmake -S . -B build && cmake --build build -j`), then:

```sh
# Terminal 1: the server (127.0.0.1:7878, 60 s planning per round)
./build/botched-tcp-server
#   --port N  --planning-seconds N  --bind 0.0.0.0   (LAN play: see the warning below)

# Terminal 2: create a match for 3 players and join it as P1
./build/botched-client --create --players 3
#   prints:  Match m1 created ...   Others join with:  botched-client --join m1 ...

# Terminals 3 and 4: join (use --host and --port when the server is on another machine)
./build/botched-client --join m1
```

### Configured games and named bots (S1–S2)

Start ready-to-join rooms without a client creating them:

```sh
./build/botched-tcp-server --config docs/examples/server-pool.json --binary-port 7879 --bot-log
# Read the printed match_id, then join normally:
./build/botched-client --wire binary --join m1
# Amiga Shell: botched-amiga tcp://server:7879 m1
```

The [example configuration](docs/examples/server-pool.json) uses shipped maps and
provisions two named-bot Foundry games plus two human-only Training duels. Each
`pool {...}` startup line is a JSON object after a fixed prefix, containing the
offering ID/label, instance number, match ID, map, seat count, bot names/blends and
`LOBBY` state. No lobby/discovery API is required. The entire pool validates before
any room is created or listener opens; invalid pool declarations exit with code 2.
Map-loading and listener failures retain their existing failure exits.

`--config` is optional and applies to `botched-tcp-server`. Its `server` object
accepts `max_concurrent_matches` and `default_planning_seconds`. Explicit
`--max-matches` and `--planning-seconds` override them **regardless of argument
order**. Other CLI settings retain their existing behaviour. An offering's explicit
`planning_seconds` overrides the server default for that offering. File values
must validate even when overridden. Without a file, existing CLI/CREATE behaviour
is unchanged. Files are limited to 1 MiB; duplicate keys and unknown fields fail.

Each offering requires `id`, `name`, `map_id`, positive `instances` and a `bots`
array. IDs use 1–32 ASCII letters/digits or `._:-`; labels and bot names use 1–32
printable ASCII characters. Offering IDs are unique; bot names are unique within
an offering, case-sensitive, and may repeat in another offering or instance.
`player_count` is mandatory for maps supporting a range; a map declaring exactly
one count infers that count. `rounds`, `target_score` and `planning_seconds` use
CREATE_MATCH's existing defaults and limits (1–200 rounds, 0–1000 target, 0–86400
seconds; omitted rounds default to 12, or 100 with a positive target).

Configured `bots` is an ordered array of `{name, personality}` objects, with at
least one human seat left. All three weights `hauler`, `opportunist`, `saboteur`
are mandatory finite JSON numbers in [0,1], summing to 1 within absolute tolerance
`1e-6`. Decimal and exponent syntax is supported in weights only; network JSON
remains integer-only. Bots occupy the highest seats in array order. Rooms wait
for every configured human seat; neither bots alone nor an early START_MATCH
starts a configured game. Ad-hoc numeric CREATE_MATCH bot counts retain the old
selection and start behaviour.

Blends are fixed at construction, not sampled each round. Normalize accepted
weights to integer millionths using largest remainders (ties: hauler, opportunist,
saboteur). Weight the existing trait tables with those integers. Round risk,
sabotage value, blunder, fumble and patience to nearest integer, halves upward;
these stay within the original archetype ranges. Allocate goal-family percentages
by flooring and assigning largest remainders (ties: haul, mixed, hunt), giving
exactly 100. Pure blends preserve existing decisions and RNG consumption.

| Blend H/O/S | Haul/mixed/hunt | Risk | Sabotage | Blunder/fumble | Patience |
|---|---|---:|---:|---|---:|
| 0.2 / 0.3 / 0.5 | 41 / 25 / 34 | 93 | 132 | 11 / 9 | 6 |
| 0.9 / 0.1 / 0 | 77 / 17 / 6 | 136 | 64 | 6 / 6 | 8 |
| 0.5 / 0 / 0.5 | 50 / 20 / 30 | 105 | 120 | 10 / 8 | 7 |

**Identity compatibility:** names and blends appear only in server startup output,
`--bot-log` and `--trace`. Frozen binary v1 mandates one of three legacy categories
per bot and cannot represent mixed/unknown. JSON/binary report the
highest configured weight as compatibility metadata, before normalization or
rounding; exact ties prefer hauler, then opportunist, then saboteur. This label
does **not** describe the full blend or drive decisions. Pure blends retain their
existing values and behaviour; legacy numeric bots keep seeded seat categories.
Existing clients may display that label. Server diagnostics retain the original
configured weights (round-trip decimal values) alongside normalized millionths. No new
wire fields, opcodes or personality values are added.

**Finished-match lifecycle (S2):** `server.finished_retention_seconds` in the
configuration file defaults to **300** seconds (integer **1–86400**). Configured
matches keep their results until that interval elapses after the server completes
the final round. The first poll at or after expiry retires the room and provisions
a new waiting room with a new ID, credentials, history and bot state, preserving
the offering, instance number, rules, named bots and blends. Reads and TCP
disconnections do not alter the timer. Waiting and running rooms never expire.

All rooms, including retained results and ad-hoc CREATE_MATCH rooms, consume the
same global cap. Replacement retires before provisioning, with no client request
interleaving. Ad-hoc rooms retain their previous indefinite retention policy.
Runtime `pool {JSON}` records show `retained` timestamps and `replaced` records
containing both `retired_match_id` and the new `match_id`; use that new ID to join.
There is no automatic client redirection or lobby/discovery API.

After retirement, fresh requests for the old ID return `UNKNOWN_MATCH`; fresh
binary BIND fails authentication. Cached binary exchange replies and immutable
FETCH pages can still describe the old match until superseded/cancelled or the
existing session inactivity timeout expires. They never bind to a replacement.
Those caches retain the existing 64-session/30-minute inactivity and storage
limits independently of room retention; old cached replies are historical only.
Restart loses games, credentials and binary sessions and recreates the pool.
Textual IDs can recur after restart and must not be treated as persistent identity.
There is no presence tracking, explicit departure, bot takeover, lobby, hot reload
or persistence. S3–S4 are not implemented.

Verification: `build/botched_tests --test-case='S1*'` and
`ctest --test-dir build -R botched_pool_integration --output-on-failure` cover
validation, deterministic blends and real mixed JSON/binary play. With existing
licensed workspace Amiga media/tools, `python3 tools/amiga/accept.py --s1` runs the
unchanged A500/KS1.3 client against two startup-provisioned named-bot matches
through real NIO. Initial S1 verification passed the existing 11 CTest checks and
the new real TCP integration check. The dominant-archetype clarification passes six focused
tests (374 assertions), also under ASan/UBSan, including pre-rounding near ties,
metadata-independent decisions and frozen binary/JSON status equivalence; the
real TCP integration also checks the expected dominant categories. The full
native `botched_tests` regression suite also passes after this clarification. The earlier
Amiga run `build/amiga/evidence/a500-z1r97kpk` completed both configured matches (89 and 57 ordered events), with the accepted A1c executable
unchanged; this is emulator/NIO evidence, not physical hardware testing.
S2 checks: `build/botched_tests --test-case='S2*'` and
`ctest --test-dir build -R 'botched_pool.*integration' --output-on-failure` cover
exact expiry boundaries, repeated full-capacity replacement, stale credentials,
immutable binary pages/retries and three real mixed-client games through recycling.
The approved scope is [the pool specification](docs/server-managed-matches-spec-v2.md).

### Mixed JSON and binary play (B3)

The JSON service remains the default on port 7878. Enable a separate binary listener explicitly:

```sh
./build/botched-tcp-server --binary-port 7879
./build/botched-client --wire binary --ui --create --players 2
./build/botched-client --wire json --ui --join m1
```

Use the match ID printed by the creating client. `--wire binary` also works without `--ui`, with
`--bot`, and with `--list-maps`. Both modes use the same board, orders, narration and playback.
The client defaults to JSON/7878; binary defaults to 7879. `--port N` overrides either default,
and `--host H` applies to both. There is no protocol autodetection on the JSON port.

Binary requests resume and retry once on a transient socket failure, retaining exact exchange
bytes and the server-issued recovery secret in memory. The adapter lease is 30 minutes. A lost
CREATE/JOIN response is recoverable within that lease; expiry reports uncertainty instead of
creating another room/seat. Quitting the process discards recovery state; restarting the server
still loses rooms. This does not add persistent reconnect or a new authentication system.

[B3 review and verification](docs/binary-v1/b3-review.md) records transport limits and acceptance.

### Maps

Maps are plain files in `resources/maps/*.map` (shipped: `training-01`, the spec's example, `factory-01`, `crossroads-01` the G2-A playtest map `trapworks-01` and the laser map `laserworks-01` the G2-B turret map `turretworks-01` the G2-C tar map `tarworks-01` the G2-D SHUNT map `shuntworks-01` and the G3 map `foundry-01`, which combines every hazard).
The server loads and validates every `.map` file at startup and reports problems per file on stderr; valid
maps stay available when another file is bad, and duplicate ids are rejected. Adding a map needs no rebuild:
drop a file in the directory and restart the server.

```
; id: my-map                 required, unique, [A-Za-z0-9._:-]
; name: My Map               required, shown to players
; description: One line.     optional
; players: 2-4               optional (N or A-B), default 2-4
#  #  #  ...                 then the tile rows exactly as in the spec: # . O S1 D1 G1 C> C<B T TA L> X1 K1 ...
```

Where the server looks (first match wins): `--maps-dir DIR`; `$BOTCHED_MAPS_DIR`; next to the executable
(`<exe dir>/resources/maps`, the installed layout `<exe dir>/../share/botched/maps`, or `<exe dir>/../resources/maps`
for a build tree inside the source tree); the source tree's `resources/maps` baked in at build time; `./resources/maps`.
So a build tree, an installed tree (`cmake --install build --prefix /opt/botched`) and `--maps-dir` all work from any
working directory.

```sh
./build/botched-tcp-server --maps-dir /srv/botched/maps       # optional
./build/botched-client --list-maps                            # id, name, players, description
./build/botched-client --create --map-id factory-01 --players 3
./build/botched-client --join m1                              # joiners need no local map files (GET_MAP)
```

`LIST_MAPS` (no fields) returns `{"type":"MAPS","ok":true,"maps":[{"map_id","name","description","min_players",
"max_players"}...]}` in ascending id order; `CREATE_MATCH` takes an optional `"map_id"` (default `training-01`).
Maps are immutable once registered and every match holds its own reference, which is what a future runtime
"register a map" feature would build on (see the backlog, B-12); none of that exists yet.

The match starts when all seats are filled (or type `start` in the creating client to begin with the
players who have joined; at least 2). Each round, every player has the planning time to enter **three
orders** and lock them in:

```
F2 R T        type three orders separated by spaces, then Enter (type again to change them)
go            lock in: the round runs as soon as everyone is locked in, or when the timer ends
```

| Code | Order | Code | Order |
|---|---|---|---|
| `F` / `F2` | forward 1 / 2 | `U` | turn around |
| `B` | back 1 | `D` | drop the crate |
| `L` / `R` | turn left / right | `T` | toggle every console on your tile or next to you |
| `W` | wait | | |

Other commands: `help`, `show` (redraw), `quit`. Players who do not lock in before the timer simply
`WAIT` (the programme you saved but did not lock is used). The screen shows the factory, robots, crates,
scores, who is ready, the time left, and what every robot did last round. When round 12 ends the client
prints the final scores and the winner (ties are shared victories, per the spec).

`botched-client --bot ...` plays automatically through the same request path (useful for filling seats,
demos and tests); `--plain` disables screen redraws (also the default when output is not a terminal).

**Reading the board.** Every cell is 3 characters wide. A robot (`1>` = player 1 facing east, a trailing
block/`*` if it carries a crate) *replaces* the tile it stands on; the status line under the board names
the tile under your robot (for example "conveyor heading E ... it will move you at the end of every
tick"). Loaded dispensers are `G1`, empty ones `g1`; the legend lists belt groups (normal/reversed) and
which dispensers hold a crate. The "what happened" log narrates each tick in the server's own order:
`P1 F2: drives ...` for your orders, `belt moves P2 ... (they ordered WAIT)` for the conveyor phase,
`pushes`, `is blocked`, turns, pickups and console toggles. Colour and Unicode blocks are used on a
terminal; `--ascii` forces the plain ASCII board (the default when the locale is not UTF-8) and
`--no-color` (or `NO_COLOR`) disables colour.

**Bots.** `--create --players 4 --bots 3` fills three seats with server-managed bots (no extra processes);
any mix works as long as at least one seat is human (e.g. `--players 4 --bots 2`, the other human runs
`--join ID`). Bots plan from the public snapshot with a short look-ahead on the real rules engine, make
occasional mistakes, and are deterministic for a given server `bot_seed`.

To watch the bots think while playtesting, start the server with `--bot-log` (both `botched-tcp-server` and
`botched-server`): each round it prints one stderr line per bot with its personality, current goal and the three
orders it chose, e.g. `[bot] match m1 round 3 P3 saboteur: arm trap X1 -> F1 T WAIT`. Logging is read-only and does not
change play or determinism.

For a full post-mortem, use `--trace` (stderr) or `--trace-file PATH` instead. It writes, per match: the set-up and each
bot's personality; at every round the board and scores; each bot's complete reasoning (situation, whether last round
went to plan, why its goal was kept or re-chosen, the candidate goals with weights and the roll, the top programmes with
scores, blunders, fumbles, and a note when chosen orders cancel out); human submissions; and every event of each
executed round as JSON. It is read-only and does not change play or determinism.

Each bot has a **personality**: *hauler* (mostly fetches and delivers crates), *opportunist* (a bit of everything) or
*saboteur* (prefers arming traps, claiming turrets and chasing rivals). Seats cycle through the three, so a 3-bot match is
mixed; the SDL scoreboard shows `BOT hauler` / `opportunist` / `saboteur` next to each bot. A bot keeps a **goal** (haul a
crate, contest a resource, arm a trap, claim a turret, chase a rival) until it is done, impossible, timed out or the last round
did not go to plan, but it re-evaluates the goal against the current board every round: crates, rival positions, hazards. It
weighs risk by health: a healthy bot may run a laser or crawl through tar to save a long walk, a bot one hit from being
disabled will not. Bots still use only public information and the same rules as a human.

### Proximity traps (G2-A)

Maps may contain `X<n>` (a trap device, solid like a wall) and `K<n>` (its control console). A trap starts
**armed and neutral**. In each tick's machinery phase (after orders, belts and lasers) an armed trap with at
least one robot on its eight surrounding tiles fires once: every robot there takes 1 damage, the trap's owner
included, and the trap disarms. A robot that is next to a `K<n>` console and executes `T` arms (or re-arms)
trap `n` and becomes its **owner**; re-arming an armed trap only changes the owner. Damage an owned trap deals
to an *opponent* scores for the owner (+1 per damaged opponent, +3 more when the hit disables them); neutral
and self-inflicted damage scores nothing. `score = 5 x deliveries + sabotage`, and sabotage counts toward a
score target. Maps without `X`/`K` tokens, and `botched/1` itself, are unchanged: the new state (`traps`,
per-player `sabotage`) and events (`TRAP_ARMED`, `TRAP_TRIGGERED`, `TRAP_HIT`, `SCORE_AWARDED`) only appear on
maps with traps, and `ROBOT_DISABLED` gains `cause: "TRAP"`. In the SDL client an armed trap tints its eight
tiles in the owner's colour, fires with a flash, shows `-1` on each victim and a gold `+N` for the owner, and
the scoreboard lists `sabotage +N`. Server bots do not use traps yet (that is G4). Try it:

```sh
./build/botched-client --ui --create --players 2 --map-id trapworks-01
```

### Charging laser turrets (G2-B)

Maps may contain `Y<n>` (a turret, solid) and `P<n>` (its control pad). A turret is **neutral** at the start
and charges on its own for two ticks, after which it is **READY**. A READY turret fires once, on the first
machinery phase in which a robot is within two tiles north, east, south or west of it (a wall stops the shot; the
nearest robot wins, ties go N, E, S, W), for 1 damage, and then recharges from 0. With nobody in range it simply
stays READY, across rounds. A robot that **enters** a `P<n>` pad claims the turret; a rival
can take it by entering a pad later, and neither affects the charge. Scoring matches the traps: +1 for hitting an
opponent of the owner, +3 more for a disabling hit, nothing for self-inflicted or neutral damage. These are new
tokens only: the existing `L>` lasers are unchanged and still neutral, and `turrets` (state) and the
`TURRET_*` events only appear on maps with turrets. The SDL client shows two charge pips and a pulsing READY label on the turret, a faint cross over the tiles in range, the owner's colour, a beam to the
victim with `-1`, a `CAPTURED` ring and the gold `+N`. Try it (server bots do not use turrets deliberately):

```sh
./build/botched-client --ui --create --players 2 --map-id turretworks-01
```

### Tar (G2-C)

A tar generator `A` (solid, no id) makes the eight tiles around it tar. In tar `FORWARD_1` and `BACK_1` cannot
move you, and `FORWARD_2` moves you one tile, even if that tile leaves the tar (it is the way out); a `FORWARD_2` that
carries you into tar from outside stops there. Turning, `DROP`, `TOGGLE` and `WAIT` are normal, a robot in tar can still push with
`FORWARD_2`, and being pushed or carried by a belt is not slowed. Tar does no damage and scores nothing; it is a
map feature, so snapshots and every existing map are unchanged. New events: `ORDER_NO_OP` with `reason: "TAR"` and
`TAR_SLOWED`. The SDL client draws the generator and purple tar tiles and plays a "TAR!" pulse. Try it:

```sh
./build/botched-client --ui --create --players 2 --map-id tarworks-01
```

### SHUNT: pushing a robot into a pit (G2-D)

Push another robot into a pit and you score **+8** (shown as "SHUNT!"). The credit goes to the robot whose move did
the pushing, also down a chain of robots and also when a belt drives the pusher. It persists for the rest of the
round: a robot you pushed that later walks (or is pushed by nobody else) into a pit still pays you, a later push by
someone else transfers the credit, and it is cleared when the robot is disabled and at round end. Walking into a pit
yourself scores nobody anything. Pit behaviour is otherwise unchanged (immediate disable, crate drops on the last
safe tile, respawn at round end). SHUNT points are sabotage points, so they count towards score targets. Events:
`SCORE_AWARDED` with `reason: "SHUNT"`, and `ROBOT_DISABLED` carries `by`. Try it:

```sh
./build/botched-client --ui --create --players 2 --map-id shuntworks-01
./build/botched-client --ui --create --players 4 --bots 3 --map-id foundry-01   # G3: the full-hazard map
```

### Score-target matches

`CREATE_MATCH` also takes an optional `"target_score"` (1-1000). With a target the match ends at the end of
the first *complete* round (all three ticks plus round-end effects) in which a player has at least that many
points, and `"rounds"` becomes the maximum-round safety cap (default 100, up to 200). If nobody reaches the
target by the cap the highest score wins. Ties use the existing deliveries/damage tie-breakers, and several
players over the line in the same round are ranked by score, so rotating tick priority never decides it.
Without a target nothing changes: 12 fixed rounds and identical hashes. `MATCH_ENDED` carries
`"reason":"target"|"cap"` in target mode, and snapshots/status carry `target_score`.

```sh
./build/botched-client --ui --create --players 3 --bots 2 --map-id crossroads-01 --target-score 30
./build/botched-sim --players 3 --map-id crossroads-01 --target-score 30 --check
```

### Graphical client (SDL2)

`botched-client --ui ...` takes the same `--create` / `--join` arguments as the terminal client but opens a
window: the factory board with drawn sprites (player-coloured robots, docks and spawn pads; animated belts
coloured by group; dispensers that show whether they hold a crate; consoles, pits, walls), a scoreboard,
a "your robot" panel, order entry and a **scrollable** "what happened" log, each in its own panel so a long
log never pushes the board off screen. Click an order (or press `F` `2`=F2 `B` `L` `R` `U` `D` `T` `W`) to
fill the three slots; they save when complete; `Enter` or LOCK IN commits. Hover a tile for its description.
When a round resolves the window **replays it**: an execution queue under the board (`P1(1) P2(1) ⚙ P2(2) ...`,
⚙ = conveyors/hazards phase) highlights the live instruction while the board animates the server's own events
(moves, turns, pushes, belt rides, blocked moves, pickups, deliveries, pit falls, respawns). `Space` or SKIP jumps
to the final state, `[` / `]` change the speed, `--anim-speed X` / `--no-anim` set it at start. While you are **planning**, the same strip previews the coming round instead: the exact tick order (rotating priority, with a ⚙ after each tick), your own chips showing the orders in your slots (`_` for an empty slot) and every other player's chip showing `?`, since their orders are hidden until the round runs. It switches back to the live replay strip once the round executes. Scroll the log with the mouse wheel, `PgUp`/`PgDn`, `Home`/`End` or by dragging its bar; `Esc` quits.
`--bot` makes the window play automatically (`--think-ms N` slows it down for demos). It needs SDL2 and
SDL2_ttf at build time (found automatically; disable with `-DBOTCHED_SDL=OFF`) and a TrueType font
(DejaVu Sans, or set `BOTCHED_FONT`). Headless check: `SDL_VIDEODRIVER=dummy botched-client --ui --frames 300
--screenshot out.bmp ...`.

**Limits of this MVP:** there is no authentication beyond the per-seat session token, so keep the
server on `127.0.0.1` or a trusted LAN; a client process that quits or crashes cannot rejoin through the CLI (its robot waits
every round); one server hosts up to 16 matches; no TLS.

Tests: `ctest --test-dir build` (the TCP host and a full match over real sockets are in `tests/test_tcp.cpp`).

## Layout

| Path | Layer | Depends on |
|------|-------|------------|
| `include/botched/`, `src/` | **Rules**: map, state, engine, match lifecycle, canonical JSON. No clocks, sockets, rendering or platform code. | C++20 stdlib |
| `server/` | **Orchestration**: rooms, sessions, planning deadlines, the logical API of spec 9.2. JSON in, JSON out. | rules |
| `apps/botched_server_main.cpp` | Host: JSON lines over stdin/stdout. The seam where a NIO channel adapter attaches later. | server |
| `server/tcp_host.*`, `apps/botched_tcpd_main.cpp` | **TCP host**: single-threaded `poll()` loop around `GameServer`; advances planning deadlines without requests. | server |
| `apps/client_core.*`, `apps/botched_client_main.cpp` | **Terminal client** (and `--bot` auto-play); JSON default, `--wire binary` optional. | server, sim lib |
| `server/binary_tcp_host.*`, `apps/binary_connection.*` | Separate binary TCP listener and resumable Linux connection using the frozen B2 codec/bridge. | binary codec, server |
| `apps/botched_sim.cpp`, `bot.*`, `ascii_render.*`, `sim_runner.*` | **POSIX reference app**: four simulated players play a full match through the public API. | server |
| `tests/` | doctest suites mapped to spec section 11. | all |

Transport (SLIP/FujiBus/NIO) and retro platform clients are *not* part of this repository's current
scope; they are milestone M1/M2 and sit above `GameServer::handle()`.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure     # or ./build/botched_tests
```

### macOS

```sh
brew install cmake pkg-config sdl2 sdl2_ttf
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run from Terminal as on Linux. The SDL client uses Verdana unless DejaVu is installed (`brew install --cask font-dejavu`).

## Run a match with no clients connected

```sh
./build/botched-sim                      # 4 players, 12 rounds, seed 1
./build/botched-sim --verbose --seed 3   # ASCII board after every round
./build/botched-sim --events             # full event stream, one JSON object per line
./build/botched-sim --flaky 25 --check   # simulate missed rounds; verify byte-identical re-run
```

## Headless server

```sh
./build/botched-server --planning-seconds 60
{"type":"CREATE_MATCH","player_count":2}
{"type":"JOIN_MATCH","match_id":"m1"}          # repeat; the last join starts the match
{"type":"SUBMIT_PROGRAM","match_id":"m1","player_id":1,"session":"<from JOINED>","round":1,
 "revision":1,"orders":["FORWARD_2","TURN_RIGHT","TOGGLE"],"ready":true,"request_id":"c-1"}
{"type":"GET_SNAPSHOT","match_id":"m1"}   {"type":"GET_EVENTS","match_id":"m1","since_version":0}
{"type":"GET_MAP","match_id":"m1"}        {"type":"GET_MATCH_STATUS","match_id":"m1"}
```

`CREATE_MATCH`, `JOIN_MATCH`, `START_MATCH` are lobby operations outside the spec's section 9.2
list. Session tokens here are reference-grade; real authentication belongs to NIO infrastructure.

## Request ids and idempotency

`SUBMIT_PROGRAM` retries are idempotent for the life of a match, with a per-player per-round budget of new
ids instead of any eviction. Contract, rationale and memory bounds: [`docs/request-id-contract.md`](docs/request-id-contract.md).
The retro-client binary wire protocol (M1) is specified as requirements in
[`docs/binary-protocol-requirements.md`](docs/binary-protocol-requirements.md).

## Untrusted input

Request size, JSON shape, field types/ranges, identifier alphabets and memory bounds are
explicit and documented in [`docs/protocol-limits.md`](docs/protocol-limits.md).

## Checkpoints

`Match::save()` / `Match::restore()` serialise a match including secret programmes. `restore()`
treats the checkpoint as untrusted: it validates the configuration, header (`protocol`,
`rules_version`, `match_id`, board), the world (identities, positions/occupancy, carrying
relationships, groups, phase/round/tick), programme records, version and event cursor, all
*before* building anything, and rejects with a reason on any failure. A rejected checkpoint
leaves no partially restored `Match`. This is a single-match checkpoint, not host persistence
(rooms, sessions, deadlines and request-id history are M1).

## Determinism contract

`Match` takes commands and produces canonical state plus events; nothing in it reads a clock or
depends on arrival order. The server decides *when* planning closes. The tests assert that
identical programmes give identical `state_hash`/event streams however they arrive, however the
ticks are paced, and across save/restore.
