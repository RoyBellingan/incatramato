# Multiplayer: shape the engine now, network it later

Real networking (sockets, lag, smoothing) belongs to Godot's multiplayer layer at M3. What we can and should do
now is make the simulation *multiplayer-shaped*: every player action is a command, the world ticks at a fixed
rate, and everything that changes can be written down and sent. Each step below ends with a check a person can
run, and leaves single-player working exactly as before.

## Rules while we build

Check new code against these, in the core, the playground and later in Godot. Each one is cheap to follow now
and expensive to retrofit.

1. **Game rules live in the core, never in the app.** If it changes the world (a tool, a vehicle, the ground),
   it goes in `core/`. The playground and Godot only turn input into commands and draw what the core says.
2. **Every player action enters as a command** (discrete action or held controls) addressed to a vehicle and a
   player. Input code never writes world state directly.
3. **The simulation knows only ticks.** No wall-clock time, no frame length, no `SDL_GetTicks` in world code.
   Frame-rate things (pedal ramps on keys, camera, smoothing) stay on the client.
4. **Nothing random without a seed the world owns**, saved with the world.
5. **Every change to the ground goes through `Terrain`**, so it is in the change list. Do not touch cells
   needlessly: a sub-millimetre rewrite of a thousand cells is a thousand cells to send.
6. **Every new piece of state is saveable**: add it to the save format and its round-trip test (from step 3 on).
   Derived data (caches, the minimap) must be rebuildable from saved state.
7. **Messages, sounds and effects come from events** the world emits, not from world code calling the HUD.
8. **Do not rely on two machines computing the same floats.** Authority, not determinism, keeps players in sync.
9. **Every vehicle has an owner** (a player id), and every vehicle ticks whether or not someone is driving it.
10. **Keep within the budget:** 8 Mbit/s of host upload for 5 busy players. Anything new that sends data
    gets measured.

## Where the code stands

What already fits:

- **The ground is a single list of changes.** `Terrain::take_deltas()` / `apply()`, with a test proving a client
  fed only deltas matches the host, and `apply()` rejects malformed or out-of-range cells.
- **Fixed physics step** (1 ms), now independent of frame rate.
- **No randomness** anywhere in the simulation.

What is in the way:

1. **Game rules live in the SDL app.** Loading, tipping, the blade, paving, rolling, recovering, the trip clock
   and the post-tip settling are in `apps/play.cpp`, mixed with input handling and drawing. Godot and a server
   would each need their own copy.
2. **Only the active vehicle is simulated.** `update()` steps `g.car()` alone; parked vehicles freeze, engine
   and all. With four players every vehicle must tick every step.
3. **Vehicles write the ground directly.** `Rig::step` calls `terrain.press()` and `terrain.dig()` itself, so
   whoever runs a vehicle's physics also edits the ground. That is incompatible with "the host owns the ground"
   unless those edits become data.
4. **Input reaches into state.** `apply(Game&, Action)` changes vehicles, the terrain and the HUD message in one
   function. Some of that is a world change, some is presentation.
5. **Nothing can be saved.** `Rig`, `ShiftController` and the per-vehicle game state have no serialised form.
   M3 also asks for "the road survives save and load", which is the same code as sending a joining player the
   world.

## Measured traffic

From the current core (a probe run on the mud course, 20 Hz batches, 28 bytes per changed cell as stored now):

| Activity | Cells changed per second | Raw | Peak batch |
|---|---|---|---|
| Truck crossing mud | ~810 | ~23 kB/s | 48 cells |
| Dozer pushing a 10 m3 heap | ~1,175 | ~33 kB/s | 260 cells |
| Whole course for a joining player | 57,600 cells | 1.4 MB | |

**Players: usually 3, sometimes 5, at most 8.** That is the design cap: player ids fit in a byte, one of the
players hosts (no dedicated server), and every player receives every change, with no need to work out who can
see what.

**Budget: 8 Mbit/s (1 MB/s) of host upload.** Each busy player makes about 30 kB/s of ground changes (between
the truck and the dozer above), and the host sends every change to every other player, so upload grows with
roughly the square of the player count. Worst case, everyone driving through mud or pushing gravel at once,
uncompressed:

| Players | Changes made | Host sends | Of the budget |
|---|---|---|---|
| 3 (usual) | ~90 kB/s | ~180 kB/s, 1.4 Mbit/s | 18% |
| 5 (occasional) | ~150 kB/s | ~600 kB/s, 4.8 Mbit/s | 60% |
| 8 (cap) | ~240 kB/s | ~1.7 MB/s, 13 Mbit/s | 170% |

So the plain format is fine for the usual and occasional games, and the headroom can go on sending vehicle
states more often (30 Hz) and in full. Only a full game of 8 needs compression, and only when everyone is busy
at once; with the roughly 3x saving below it fits at about 4.5 Mbit/s.

Compression, when it is needed: quantising heights to millimetres
in 16-bit integers and sending only the fields that changed should bring a cell to 6-12 bytes, and settling
produces many sub-millimetre changes that need not be sent at all. A joining player on this course takes 1.4 MB,
under 2 s within the budget; on a 4 km2 map stored densely it would be about 1.5 GB, so there joining must send
only the chunks that differ from the map as shipped.

## Authority (decision needed)

**Recommended: the host owns the ground and the tools; each player owns their own vehicle's motion.**

- A player's machine runs its own vehicle's physics against its copy of the ground, so driving feels immediate
  at any ping, and sends the vehicle's state (position, speed, wheel speeds, engine, gear, ...) about 30 times a
  second. Other players' vehicles are drawn from those states, smoothed.
- What a vehicle does *to the ground* (pressing ruts, digging when it spins) is sent as **ground effects**
  (wheel position, footprint, depth, dig amount), not applied as truth. The host applies them to the one real
  ground and broadcasts the resulting cell changes, which overwrite everyone's copy, including the sender's.
  The sender applies its own effects locally at once, so its ruts appear without delay; the host's version
  replaces them a moment later and normally matches.
- Tool actions (load, tip, blade pass, pave, roll) are **commands** to the host, which runs them and broadcasts
  the cell changes.

This is co-op, so a player cheating with their own vehicle's motion hurts nobody but themselves; the host can
add plausibility checks later (speed limits, teleports) without changing the model. The alternative, the host
simulating every vehicle from players' inputs with prediction on each machine, is fairer but needs the vehicle
physics to agree closely across machines; `PLAN.md` already notes determinism across compilers is not
guaranteed.

## Steps

### 1. A `World` in the core

Move the game rules out of `apps/play.cpp` into `core/` as a `World`: the terrain, every vehicle with its shift
controller and tool state (bed, blade, layer), and the rules that today sit in `apply()` and `update()`.
`World::command(player, Command)` takes discrete actions (gear, mode, AWD, load, tip on/off, blade, pave, ...);
`World::controls(player, DriverInput)` takes the held inputs; `World::tick()` advances one tick. Messages for the
HUD come back as events, not as a string inside the world.

- **Tick:** 20 ms = 20 physics steps (50 Hz). Inputs are sampled per frame and applied per tick.
- **All vehicles tick**, not only the active one. Parked vehicles idle and hold on their brakes.
- `play.cpp` keeps input mapping, the keyboard pedal ramps, drawing, sound and telemetry, and talks to the
  world only through commands and controls.

Check: `loop_demo` output and the play self-test unchanged except for parked vehicles now idling; the game plays
the same.

### 2. Ground effects as data

`Rig::step` stops writing the terrain. It reads the ground and returns the effects it wants (`Press`, `Dig`,
per wheel per step); the world applies them. Single-player applies them at once, so behaviour is identical.

Check: `loop_demo` output identical to the digit.

### 3. Saving everything

A small versioned binary format for: commands and controls, vehicle state (`Rig`, `ShiftController`, tool
state), terrain cells and deltas, and the whole world. Save and load in the playground (two keys), which is
also what a joining player receives.

Check: round-trip tests; a run saved at 10 s, loaded and continued matches the uninterrupted run; the road
survives save and load in the game.

### 4. Host and players in one process

A `Host` (the real world, applies commands and ground effects, broadcasts cell changes and vehicle states) and
`Client`s (own vehicle simulated locally, others drawn from states), connected by in-memory queues that can
add latency, jitter and loss. No sockets.

Check: tests with three and five players driving, tipping and grading at 150 ms simulated ping with 2% loss:
every copy of the ground ends identical to the host's, and every client's view of each vehicle stays within a
set distance of the owner's. The tests also report the bytes each message type used, against the budget, and an
8-player run shows where compression starts to be needed.

### Not doing: two windows over the network

Decided (November 2026): no sockets in the prototype. Co-op is first played in Godot; steps 1-4 make sure the
core is ready for it, and the in-process tests of step 4 stand in for a network until then.

## Godot at M3

### What Godot gives us

- **ENet over UDP** (`ENetMultiplayerPeer`), with reliable and unreliable channels. WebSocket and WebRTC peers
  exist too; we do not need them.
- **RPCs** (`@rpc` functions): who may call them, reliable or not, and on which channel.
- **Authority per node**: each node has an owning peer. A vehicle node's authority is its owner.
- **`MultiplayerSpawner`**: creates and removes the same scenes on every machine (a player's vehicle when they
  join).
- **`MultiplayerSynchronizer`**: keeps chosen node properties in sync. Fine for small things; not for the
  ground, which is thousands of cells a second and already has its own change list.
- **`UPNP`**: asks the host's router to open the port, so friends can join without port forwarding (when the
  router allows it).

### What it does not, and stays ours

- **Prediction, smoothing, lag compensation.** Our vehicles run the core's physics, not Godot's, so smoothing
  other players' vehicles is ours (step 4 builds and tests it). Add-ons such as netfox target simpler
  character movement.
- **The world's data.** Saving, joining and change lists are the core's format (step 3); Godot carries the
  bytes as `PackedByteArray`.
- **Deterministic physics.** None; see rule 8.
- **Lobbies, invites, NAT relay.** Not built in. Options: UPnP or manual port forwarding for friends; if the game
  ships on Steam, the GodotSteam add-on for invites and Steam's relays.

### How our messages map onto it

| Message | From → to | Godot | Rate |
|---|---|---|---|
| Commands (gear, tools, ...) | player → host | RPC, reliable, ordered | on change |
| Ground effects (ruts, digging) | player → host | RPC, reliable | batched, 20 Hz |
| Cell changes | host → all | RPC, reliable, own channel | batched, 20 Hz |
| Vehicle state | owner → all (via host) | RPC, unreliable ordered | 30 Hz |
| World snapshot | host → joining player | RPC, reliable, in chunks | once |
| Vehicles appearing and leaving | host → all | `MultiplayerSpawner` | on change |

The host is a player (peer 1) and runs the authoritative `World`. The glue is a GDExtension class that feeds RPC
payloads into the core's `Host` and `Client` from step 4 and hands their outgoing messages back. Compression
(above) comes before games of 6-8 players are supported, chunked joining before maps larger than the prototype
course.

## Decisions for you

1. **Authority:** host owns ground and tools, players own their vehicle's motion (recommended), or host
   simulates everything with prediction. Needed before step 2.
2. **One vehicle per player, or shared?** Can two players swap vehicles, or ride along? Today Tab switches
   freely; with several players a vehicle needs an owner. Needed before step 4.
3. ~~Playtest co-op in the prototype over the network?~~ Decided: no, wait for Godot.
