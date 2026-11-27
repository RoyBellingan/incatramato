#pragma once

// The game world: the site, its vehicles and tools, and the players driving them. Everything a player does
// enters as a command or as held controls; the world advances in fixed ticks and reports what happened as
// events. Nothing here knows about frames, windows, sound or the network (docs/MULTIPLAYER.md).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "inc/presets.h"
#include "inc/transmission.h"
#include "inc/tuning.h"

namespace inc {

using PlayerId = std::uint8_t;

inline constexpr float kStepDt = 1e-3f;  // physics step [s]
inline constexpr int kStepsPerTick = 20;
inline constexpr float kTickDt = kStepDt * kStepsPerTick;  // 20 ms: the world advances 50 times a second

// Discrete things a player does, sent when they happen. Each acts on the vehicle the player is driving.
enum class Command : std::uint8_t {
    Gear0, Gear1, Gear2, Gear3, Gear4, Gear5, Gear6, Reverse, ShiftUp, ShiftDown, NextMode,
    ToggleAwd, ToggleLock, ToggleLow, ToggleEngineBrake, ToggleHandbrake,
    NextVehicle,
    Cargo,  // truck: load at the quarry; dozer: blade down / up
    BladeLower, BladeRaise, Pave, Roll, Recover,
};

// What a player is holding, applied at every tick until replaced.
struct PlayerControls {
    float throttle = 0.0f, brake = 0.0f, clutch = 0.0f;  // 0..1
    bool starter = false;
    bool tip = false;  // tip the bed
};

// Something worth telling a player. The world says what happened; the client decides how to show it.
struct Event {
    enum class Kind : std::uint8_t {
        GearboxMode,  // value: ShiftMode
        Awd, DiffLock, LowRange, EngineBrake, Handbrake,  // on
        Seated,         // the player now drives `vehicle`
        NoFreeVehicle,  // every other vehicle has a driver
        Blade,          // on: down; value: layer [m]; value2: 1 when it rose by itself for reversing
        BladeLayer,     // value: layer [m]
        Loaded,         // value: m3
        HoldToTip,      // Cargo pressed away from the quarry with gravel in the bed
        BedEmpty,       // Cargo pressed with nothing in the bed, or tipping emptied it
        TipBlocked,     // another vehicle (value: its index) is too close to where the gravel would land
        Paved,          // value: m2 (0: nothing had enough base); value2: base needed [m]
        Rolled,         // value: m2
        Recovered,
        Arrived,        // trip from the quarry gate to the town gate; value: s, value2: litres
    };
    Kind kind;
    int player;  // who it is for
    int vehicle;
    float value = 0.0f, value2 = 0.0f;
    bool on = false;
};

// A vehicle on the site, with the tools it carries.
struct Machine {
    std::string label;
    std::string vehicle, tyre;  // what it is built from in the tuning (Tuning::spec)
    Rig rig;
    float home_x;     // where Recover puts it
    float capacity;   // bed [m3]; 0 without one
    bool has_blade;
    ShiftController box{};
    DriverInput in{};  // what its driver asks for (persistent toggles live here too)
    Controls out{};    // what its actuators did on the last step
    int lever = 0;     // manual: lever position; automatic: >0 D, 0 N, <0 R
    int pending_shift = 0;
    int driver = -1;   // player driving it, or -1 when parked

    float gravel = 0.0f;  // in the bed [m3]
    bool tipping = false;
    bool tip_refused = false;  // tipping is being refused (another vehicle too close); told once
    float settle_x = 0.0f, settle_left = 0.0f;  // a fresh heap (tipped, or let go by the blade) keeps slumping a moment
    float trail_left = 0.0f;  // the lane behind a pushing blade keeps slumping a moment after it passes

    bool blade = false;
    float blade_layer = 0.20f;  // edge above the original ground under the tracks [m]; negative digs into ruts
    float blade_edge = 0.0f;    // [m]
    float blade_pile = 0.0f;    // gravel being pushed [m3]
    float blade_force = 0.0f;   // what that costs the tracks [N]
    bool ripping = false;       // broke up asphalt this tick (only with the blade set below ground)

    bool trip_running = false;  // quarry gate to town gate
    float trip_time = 0.0f, trip_fuel0 = 0.0f;
};

class World {
public:
    // quarry_end: loading only before this x; tipping only after. town_start: the trip ends here.
    World(presets::Course course, float quarry_end, float town_start, Tuning tuning = Tuning::defaults());
    World(const World&) = delete;  // vehicles point at the terrain
    World& operator=(const World&) = delete;

    // A vehicle from the tuning (e.g. "truck_6x6") fitted with a tyre from it (e.g. "mud").
    int add_machine(std::string label, const std::string& vehicle, const std::string& tyre, float x, float capacity, bool has_blade);
    // Applies new tuning to the running world: vehicles keep where they are and what they are doing.
    void retune(const Tuning& tuning);
    const Tuning& tuning() const { return tuning_; }
    const Rules& rules() const { return tuning_.rules; }

    PlayerId join();  // seats the new player in the first free vehicle (shared if none is free)
    void command(PlayerId player, Command c);
    void controls(PlayerId player, const PlayerControls& c);
    void tick();
    std::vector<Event> take_events();

    Terrain& terrain() { return course_.terrain; }
    const Terrain& terrain() const { return course_.terrain; }
    const presets::Course& course() const { return course_; }
    float quarry_end() const { return quarry_end_; }
    float town_start() const { return town_start_; }

    const std::vector<Machine>& machines() const { return machines_; }
    Machine& machine(int i) { return machines_[static_cast<std::size_t>(i)]; }
    const Machine& machine(int i) const { return machines_[static_cast<std::size_t>(i)]; }
    int vehicle_of(PlayerId p) const { return players_[p].vehicle; }

    std::uint64_t ticks() const { return ticks_; }
    double time() const { return static_cast<double>(ticks_) * kStepsPerTick / 1000.0; }  // whole ms: no float drift

private:
    struct Player {
        int vehicle;
        PlayerControls held;
    };
    void tick_machine(Machine& m);
    void let_go(Machine& m);
    void say(int player, int vehicle, Event::Kind kind, float value = 0.0f, float value2 = 0.0f, bool on = false);

    presets::Course course_;
    float quarry_end_, town_start_;
    Tuning tuning_;
    std::vector<Machine> machines_;
    std::vector<Player> players_;
    std::vector<Event> events_;
    std::uint64_t ticks_ = 0;
};

// The playground: 180 m with 40 m of mud, a stock truck, an off-road truck and a dozer at the quarry.
std::unique_ptr<World> make_playground(const Tuning& tuning = Tuning::defaults());

}  // namespace inc
