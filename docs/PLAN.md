# Plan

## Engine: Godot 4, with the simulation in engine-free C++

Use an engine. Rendering, editor, input, audio, UI and platform support are not where this game is different,
and three people cannot afford to write them.

Godot fits this team and this game:

- **C++ is first-class without rebuilding the engine** (GDExtension). The sim core plugs in as a native library.
- **Open source, no royalty.** You can read and patch the engine when physics or terrain gets in the way.
- **Mod loading is a documented feature** (resource packs loaded at runtime), and modders get the same free editor.
- **Jolt is the default 3D physics since Godot 4.6.**
- **The editor is light**, which matters for the two people doing visual work.

What Godot does not give you, and you will write or adopt:

- **Terrain.** There is no built-in terrain. Terrain3D is the main heightmap plugin; whether it tolerates
  per-frame deformation is not verified. Expect to render the ground from the core grid yourself.
- **Off-road vehicle physics.** The built-in vehicle node is too simple. Wheels will be ray or shape casts on a
  rigid body, calling the core for forces. Jolt itself has wheeled and tracked vehicle controllers, but I have
  not verified that Godot exposes them; assume it does not.
- **Large-world streaming.** Weaker than Unreal out of the box (my judgment, not verified here).

The alternative is Unreal: landscape, replication and world streaming are built in and it is C++ native. It is
heavier for a team of three, mod support means shipping your own mod kit, and it carries a revenue royalty
(check current terms). A custom engine is what Saber and BeamNG did; not realistic at this team size.

The decision that keeps this reversible: **everything that is specific to this game lives in `core/` and knows
nothing about the engine.** It builds and tests in a second from the command line, runs on a headless server,
and would survive a change of engine.

## Architecture

```
core/        C++20, no dependencies. Ground state, soil model, driveline, road-building operations.
binding/     GDExtension. Exposes core types to Godot; owns per-wheel casts and force application.
game/        Godot project. Scenes, rendering of the ground, vehicles, tools, UI, networking glue.
data/        Vehicles, tyres, soils, cargo as text files. This is the mod format.
```

- **Terrain authority:** the host owns the ground. `Terrain::take_deltas()` already yields exactly the cells
  that changed; clients `apply()` them. A test proves a client fed only deltas matches the host. `apply()`
  skips deltas with an out-of-range index or a malformed cell and returns how many it skipped; a nonzero count
  means the client has diverged and should ask the host for a full resync (not written yet).
- **Vehicles:** each player simulates their own vehicle and sends its state. Decide at M3 whether the host validates.
- **Getting there:** [MULTIPLAYER.md](MULTIPLAYER.md) is the step-by-step plan for making the core multiplayer-shaped before M3.
- **Tunnels:** hybrid. The grid stays a heightfield; a tunnel is a portal hole plus separate geometry along a
  planned line.

## Milestones

Each ends with something a person can check. M4, M5 and M6 do not depend on each other and can run in parallel
once M1 is done.

| | Milestone | Done when |
|---|---|---|
| M0 | **Core** (done) | `loop_demo` shows stuck, crossing, wear, gravel, asphalt; tests pass |
| M0.5 | **Playground** (done) | `play`: the loop is playable from the side with a keyboard; gearbox feel can be tuned |
| M1 | **See it in Godot** | Drive the truck into the mud with a gamepad, get stuck, see the ruts |
| M2 | **Build in the world** | Dump gravel from a bed, grade with a blade, pave, roll, then drive the stock truck across |
| M3 | **Two to four players** | Every client sees the same ruts and the same road; the road survives save and load |
| M4 | **Tunnel spike** | One portal cut at runtime, one segment, a truck drives through in multiplayer |
| M5 | **Tracked vehicle in 3D** | The dozer skid-steers and crosses mud the truck cannot |
| M6 | **Content as data** | Truck, tyres and soil load from files; a second truck loads from a mod pack |
| M7 | **Slice for outsiders** | A stranger can play the loop for half an hour without help |

## Who does what

- **You (C++):** core, binding, networking.
- **Programmer-artist:** Godot project, ground rendering (ruts, mud, gravel and asphalt blending), vehicle rig, tools.
- **Artist:** truck and dozer models, materials, HUD and map mock-ups, the look of the first map.

## What the core does today

- Ground as a grid of columns: firm base, soft layer, gravel, rut depth, paving state, loose gravel on asphalt.
- Wheels and tracks sink according to ground pressure; gravel spreads the load; ruts persist.
- Traction and compaction resistance from the standard terramechanics formulas; slipping wheels dig into mud
  and throw gravel behind them (none is lost).
- Grip per tyre type and surface: tracks grip far better than tyres on earth and in mud, far worse on asphalt.
- Engine with inertia, idle governor, stall, starter, engine braking and exhaust brake.
- Friction clutch that holds or slips; gears that grind unless the clutch is down or the revs match.
- Manual, sequential and automatic shifting over the same model.
- Low range, part-time AWD, lockable differentials as a tree.
- Dump, grade, pave, roll, all conserving material.
- A dozer blade for any heading: cuts gravel above its edge, pushes the heap, which slumps at the angle of repose
  and spills off the blade ends; the heap's size is the resistance the tracks feel.
- Wheels and tracks stand on the average of the ground under their contact patch, so a track bridges a tyre rut.
- A `World` that holds the site, every vehicle and its tools, and the players: commands in, fixed 50 Hz ticks,
  events out. Every vehicle ticks whether driven or parked. Loose gravel under a chassis is ploughed ahead of the
  leading axle instead of lifting the vehicle; no tipping within 3 m of another vehicle.
- Per-wheel and body telemetry from the rig, served live to a browser by the playground.
- Fuel burn, high-centring, grades.
- Tuning: every placeholder number lives in `data/tuning.json`, reloadable in the running game (a first step
  towards M6, content as data).
- Payload: cargo weighs on the wheels that carry it. Brakes and handbrake hold a stopped vehicle on a slope.

## Known gaps in the core

- **All parameters are placeholders.** The grip table (`soil.h`) is approximate and quoted from memory of the
  Caterpillar Performance Handbook; check it against the handbook. The demo behaves plausibly because I tuned one soil constant until the
  drivetrain options made a difference. Treat the numbers as a starting point.
- **Asphalt barely beats gravel.** 22.0 s against 22.2 s in the demo, about 1%. Nothing yet makes rough roads slow:
  no roughness, no wear, no cargo damage. The late game needs a design answer here, not just code.
- **Low range halves the crawl speed but does not change who gets through.** First gear already has more torque
  than the tyres can use. It will matter with trailers, heavy loads and steep grades.
- **Gearbox, still missing:** range and splitter layouts (a real 12 or 16 speed), torque-converter automatics,
  clutch heat and wear, a retarder, turbo lag, and tunable shift timing per vehicle. Shift timings and shift
  points are constants in `transmission.cpp`.
- **Controllers are tested against a virtual device only.** Binding, inverted pedals, half axes and the
  H-shifter are covered by the self-test, but no real wheel, throttle or shifter has been plugged in yet.
  No force feedback. Two identical devices cannot be told apart.
- **Sound is a first pass**, checked by frequency analysis, not by ear.
- **Payload sits at a fixed spot.** Gravel in the bed adds its weight (90% on the rear bogie) and its mass, so
  a loaded truck sinks deeper and accelerates slower, but the load does not shift on slopes or under braking.
- **The playground cheats on tools.** Trucks tip ahead of themselves because nothing can turn round in one
  dimension; paving and rolling are key presses; vehicles pass through each other.
- **The blade moves gravel only** (and, set below ground, rips asphalt back into gravel). It cannot dig mud or firm ground, and nothing spills over its top, however
  big the heap. The blade does not back-drag in reverse. Push resistance is the heap's weight times a placeholder
  friction coefficient; there is no separate cutting force.
- **No suspension.** Wheel loads are fixed shares of the weight; the telemetry page says so rather than show
  invented travel.
- **Diff locks only matter when wheel loads differ**, which a straight-line rig on even ground never produces.
  The test covers it with a wheel in the air; the real payoff arrives with suspension in M1.
- **Straight line only.** No steering, lateral force, suspension or weight transfer; those come from the engine's
  physics in M1.
- **No water, snow or ice**, and no tyre chains.
- **Memory.** A cell is 24 bytes at 0.25 m spacing, so a 4 km2 map stored densely is about 1.5 GB. Storage must
  become chunked, with soft-ground data only where there is soft ground, before maps grow.
- **Determinism** across compilers is not guaranteed, which is why the host owns the ground.

## Reasons to stop or re-scope

- Driving on the road you built is not more satisfying than the mud was (answerable at M2).
- Ground changes cannot stay in sync at four players (M3).
- Saber announces a SnowRunner sequel with construction before M7.
