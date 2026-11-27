# Incatramato

Off-road trucking where you build the road. This repository holds the simulation core (deformable ground,
a full powertrain, road-building operations) as plain C++20 with no engine dependency, plus a small
side-view prototype you can play. See [docs/PLAN.md](docs/PLAN.md) for where it goes next.

## Build

```
cmake -S . -B build -G Ninja
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

Needs CMake 3.20+ and a C++20 compiler (built here with GCC 13). The core, the demo and the tests have no
third-party dependencies. The playable prototype needs SDL2 2.0.18 or newer (`libsdl2-dev`, `brew install sdl2`,
or vcpkg); without it CMake skips that one target.

## Play

```
./build/play
```

One lane, seen from the side: quarry on the left, town on the right, 40 m of mud between them. Three vehicles
share the world. The loop: get the stock truck stuck, get through with the off-road truck, then haul gravel,
grade it with the dozer, pave it, and drive the stock truck across.

| Key | Action |
|---|---|
| W / S (or Up / Down) | Throttle / brake |
| S S (double tap) | Handbrake on / off (rear axles; the dozer's tracks) |
| Left Shift | Clutch (manual mode). Stamps down fast, comes up slowly |
| 1-6, N, R | Gear lever (manual), or D / N / R (automatic) |
| E / Q | Shift up / down |
| M | Gearbox mode: automatic, manual, sequential |
| T / Y / L | AWD / differential locks / low range |
| B | Engine brake |
| I (hold) | Starter, after a stall |
| Tab | Next vehicle |
| G | Truck: load at the quarry; elsewhere hold to tip (1.5 m3/s over the 2.2 m tailgate, let go to stop; tip on the move to spread a line). A marker shows where it lands; not within 3 m of another vehicle. Dozer: blade down / up |
| Z / X | Dozer: blade lower / higher, 5 cm a step (from 50 cm below to 50 cm above the original ground). Below ground it also rips up asphalt |
| P / O | Lay asphalt under the vehicle (needs 15 cm of graded gravel) / roll it |
| Backspace | Recover the vehicle to the quarry |
| F1 | Sound on / off |
| F3 | Controller setup |
| F5 / Esc | Reset the world / quit |
| F11 | Fullscreen on / off. The window can also be resized or maximised; `./build/play --fullscreen` starts fullscreen |

The pedal bars in the HUD show what the actuators are doing, so in automatic and sequential modes you can
watch the clutch being worked for you.

### The dozer

The blade cuts the gravel above its edge and pushes it as a heap in front, which slumps at the angle of repose
(about 35 degrees): forward, into ruts and holes, and off the blade ends. Behind it is an even layer. The edge sits
at the set height above the original ground under the tracks, not above the dozer itself, so it spreads a level
layer instead of climbing onto what it has just laid (like grade control on a real machine). At 0 cm it strips the
gravel down to the ground; below 0 it scrapes gravel out of ruts. It moves gravel only, not mud. Pushing a bigger
heap takes more force, and too big a heap in mud will stall it. Gravel tipped on a finished road lies on top of the
asphalt and can be pushed off again. The dozer has three reverse gears, a little faster than forward.

### Telemetry on a second screen

While the game runs, open **http://localhost:8787/** in a browser: driveline, forces, blade, every wheel or track
(load, sinkage, rut, slip, traction against its limit, drive torque, brake), the ground profile under the vehicle,
and charts of the last 30 seconds, updated 20 times a second. There is no suspension in the model yet, so wheel
loads are fixed shares of the weight until the 3D vehicle (M1).

- `--telemetry-port N` picks another port, `--no-telemetry` turns it off.
- `--telemetry-lan` also accepts connections from other machines, for a tablet: then open
  `http://<this machine's address>:8787/`. Off by default, since anyone on the network could then read it.
- `/snapshot` returns the latest JSON once and `/stream` is a Server-Sent Events stream of it, for your own tools.

**http://localhost:8787/3d** shows the whole ground in 3D (WebGL2, no libraries), reloaded whenever it changes, with
the vehicles as boxes. Three views: the surface as it is, the ground without its gravel (mud, ruts, firm ground),
and gravel thickness (blue: too thin to pave, green: 15 cm or more, orange: heaps). A height slider exaggerates
relief, and a readout gives the exact values of the cell under the cross. `/terrain.bin` is the ground it loads:
a small binary format described above `terrain_blob()` in `apps/play.cpp`.

### Wheels, pedals, throttles, shifters

Press F3 with the hardware plugged in. The game asks for each function in turn; push the pedal or lever all the
way and let go, press the button, or put the shifter in the gate. Esc skips a function. Any mix of devices
works, because bindings are stored per device name: a flight throttle and stick, a wheel with three pedals,
a separate H-shifter. Inverted pedals and half axes (stick back = brake) are detected from how you moved them.

- An H-shifter moves the lever when it changes gate, and back to neutral when it is in none.
- Analogue pedals add to the keyboard, so both keep working.
- Bindings are saved to `input.cfg` in the working directory; it is a small text file you can edit.

There is no steering yet (one lane) and no force feedback.

### Sound

Synthesised, no audio files: the engine note follows revs and throttle, plus wheelspin, gear grind and starter.
The engine is tuned as a six-cylinder, so the firing frequency is rpm / 20 Hz.

## Gearbox

Three modes over one physical model:

- **Manual.** Clutch pedal and lever. A gear goes in with the clutch down, or without it if engine and gearbox
  speeds match (floating gears). Otherwise it grinds and stays in neutral. Dump the clutch in a high gear and
  the engine stalls; hold the starter, or bump-start it by rolling in gear.
- **Sequential.** You choose the gear; each shift is clutch out, new gear, rev-match, clutch in.
- **Automatic.** An automated manual. Upshifts are judged on road speed, so wheelspin in mud does not make it
  change up.

Underneath: an engine with its own inertia and an idle governor (the truck creeps in first without throttle, at
half the speed in low range), a friction clutch that either holds or slips, engine braking and an exhaust brake.

## Numbers demo

`./build/loop_demo` runs the same loop without a window and prints it:

```
260 m run, 100 m of it through half a metre of mud. Each line starts on untouched ground.
  highway tyres, RWD, open diffs                 STUCK   5 m into the mud (of 100)
  highway tyres, AWD + diff locks                STUCK   5 m into the mud (of 100)
  mud tyres, RWD, open diffs                     STUCK  27 m into the mud (of 100)
  mud tyres, AWD + diff locks                    crossed     41.2 s   22.7 km/h avg   0.63 L
  tracked dozer                                  crossed     62.7 s   14.9 km/h avg   0.51 L

Same world from here on. The well-equipped truck keeps using the track:
  pass  1 (deepest rut after: 24 cm)             crossed     41.2 s   22.7 km/h avg   0.63 L
  pass  2 (deepest rut after: 26 cm)             crossed     38.3 s   24.4 km/h avg   0.58 L
  pass  3 (deepest rut after: 29 cm)             crossed     39.2 s   23.8 km/h avg   0.60 L
  pass  4 (deepest rut after: 32 cm)             crossed     40.5 s   23.1 km/h avg   0.62 L
  pass  5 (deepest rut after: 36 cm)             crossed     42.3 s   22.2 km/h avg   0.64 L
  pass  6 (deepest rut after: 50 cm)             STUCK  67 m into the mud (of 100)

Build a gravel road: 14 loads of 10 m3, then grade.
  gravel: 19 cm on the crown, 62 cm where the ruts were
  stock truck on gravel, first pass              crossed     22.3 s   42.0 km/h avg   0.27 L
  stock truck on gravel, second pass             crossed     22.3 s   42.0 km/h avg   0.27 L

Pave and roll it.
  paved 547 m2
  stock truck on asphalt                         crossed     22.0 s   42.6 km/h avg   0.25 L
```

All soil, tyre and vehicle numbers are placeholders chosen to be plausible. Nothing is calibrated yet.

## Layout

| Path | Contents |
|---|---|
| `core/include/inc/terrain.h` | Ground grid, traffic damage, gravel/grade/pave/roll, the dozer blade, change deltas for networking |
| `core/include/inc/soil.h` | Sinkage, compaction resistance and traction (Bekker / Janosi-Hanamoto) |
| `core/include/inc/drivetrain.h` | Engine, clutch, gearbox, and a differential tree solved exactly for any layout |
| `core/include/inc/transmission.h` | Shift controller: manual, sequential, automatic |
| `core/include/inc/rig.h` | Straight-line test rig |
| `core/include/inc/world.h` | The world: site, vehicles and tools, players, commands, ticks (50 Hz) and events |
| `core/include/inc/presets.h` | Placeholder truck, dozer, tyres and test course |
| `apps/play.cpp` | The playable prototype (SDL2) |
| `apps/input.h` | Controller bindings and the setup wizard |
| `apps/sound.h` | Sound synthesis |
| `apps/telemetry.h`, `apps/telemetry_page.h`, `apps/view3d_page.h` | Telemetry HTTP server (POSIX sockets, no dependencies), the dashboard page and the 3D view |
| `apps/loop_demo.cpp` | The numbers demo |
| `tests/core_tests.cpp` | Checks: material conservation, sinkage, differentials, clutch and gearbox, the loop, terrain replication |
