#include "inc/world.h"

#include <algorithm>
#include <cmath>

namespace inc {
namespace {

constexpr float kG = 9.81f;
constexpr float kSettleTime = 2.0f;  // a fresh heap keeps slumping this long after tipping stops [s]

// Force to shove a heap of loose gravel [N].
float push_force(float pile_m3) { return kPushFriction * kGravelDensity * kG * pile_m3; }

// Extent of a vehicle along the lane: its footprints, and the blade in front if it has one.
void extent(const Machine& m, float& lo, float& hi) {
    const VehicleSpec& s = m.rig.spec();
    lo = 1e30f, hi = -1e30f;
    for (const WheelSpec& w : s.wheels) {
        lo = std::min(lo, w.x - 0.5f * s.tyre.length);
        hi = std::max(hi, w.x + 0.5f * s.tyre.length);
    }
    if (m.has_blade) hi = std::max(hi, kBladeAhead);
    lo += m.rig.x(), hi += m.rig.x();
}

}  // namespace

World::World(presets::Course course, float quarry_end, float town_start)
    : course_(std::move(course)), quarry_end_(quarry_end), town_start_(town_start) {}

int World::add_machine(std::string label, VehicleSpec spec, float x, float capacity, bool has_blade) {
    machines_.push_back({std::move(label), Rig(std::move(spec), course_.terrain, SoilParams{}, x, course_.lane_y), x, capacity,
                         has_blade});
    return static_cast<int>(machines_.size()) - 1;
}

PlayerId World::join() {
    const auto id = static_cast<PlayerId>(players_.size());
    int seat = 0;
    for (std::size_t i = 0; i < machines_.size(); ++i)
        if (machines_[i].driver < 0) {
            seat = static_cast<int>(i);
            break;
        }
    players_.push_back({seat, {}});
    if (machines_[static_cast<std::size_t>(seat)].driver < 0) machines_[static_cast<std::size_t>(seat)].driver = id;
    say(id, seat, Event::Kind::Seated);
    return id;
}

void World::say(int player, int vehicle, Event::Kind kind, float value, float value2, bool on) {
    events_.push_back({kind, player, vehicle, value, value2, on});
}

std::vector<Event> World::take_events() {
    std::vector<Event> out;
    out.swap(events_);
    return out;
}

void World::controls(PlayerId player, const PlayerControls& c) { players_[player].held = c; }

void World::command(PlayerId player, Command c) {
    const int vi = players_[player].vehicle;
    Machine& v = machine(vi);
    Terrain& t = course_.terrain;
    const int top = static_cast<int>(v.rig.spec().gearbox.forward.size());
    const float x = v.rig.x();
    const Rect under{x - 4.0f, course_.road.y0, x + 4.0f, course_.road.y1};
    using K = Event::Kind;
    switch (c) {
        case Command::Gear0: case Command::Gear1: case Command::Gear2: case Command::Gear3:
        case Command::Gear4: case Command::Gear5: case Command::Gear6:
            if (v.box.mode != ShiftMode::Sequential)
                v.lever = std::min(static_cast<int>(c) - static_cast<int>(Command::Gear0), top);
            break;
        case Command::Reverse:
            if (v.box.mode != ShiftMode::Sequential) v.lever = -1;
            break;
        case Command::ShiftUp:
        case Command::ShiftDown: {
            const int step = c == Command::ShiftUp ? 1 : -1;
            if (v.box.mode == ShiftMode::Sequential) v.pending_shift = step;
            else if (v.box.mode == ShiftMode::Manual) v.lever = std::clamp(v.lever + step, -1, top);
            else v.lever = std::clamp((v.lever > 0 ? 1 : v.lever) + step, -1, 1);  // R - N - D
            break;
        }
        case Command::NextMode:
            v.box.mode = static_cast<ShiftMode>((static_cast<int>(v.box.mode) + 1) % 3);
            v.lever = v.box.mode == ShiftMode::Automatic ? (v.rig.gear() > 0) - (v.rig.gear() < 0) : v.rig.gear();
            say(player, vi, K::GearboxMode, static_cast<float>(v.box.mode));
            break;
        case Command::ToggleAwd:
            v.in.awd = !v.in.awd;
            say(player, vi, K::Awd, 0, 0, v.in.awd);
            break;
        case Command::ToggleLock:
            v.in.diff_lock = !v.in.diff_lock;
            say(player, vi, K::DiffLock, 0, 0, v.in.diff_lock);
            break;
        case Command::ToggleLow:
            v.in.low_range = !v.in.low_range;
            say(player, vi, K::LowRange, 0, 0, v.in.low_range);
            break;
        case Command::ToggleEngineBrake:
            v.in.engine_brake = !v.in.engine_brake;
            say(player, vi, K::EngineBrake, 0, 0, v.in.engine_brake);
            break;
        case Command::ToggleHandbrake:
            v.in.handbrake = !v.in.handbrake;
            say(player, vi, K::Handbrake, 0, 0, v.in.handbrake);
            break;
        case Command::NextVehicle: {
            // The next vehicle nobody else is driving. The one left behind keeps its toggles and lever, but
            // nobody is on the pedals any more.
            const int n = static_cast<int>(machines_.size());
            int next = -1;
            for (int k = 1; k < n && next < 0; ++k) {
                const int i = (vi + k) % n;
                if (machine(i).driver < 0) next = i;
            }
            if (next < 0) {
                say(player, vi, K::NoFreeVehicle);
                break;
            }
            if (v.driver == player) v.driver = -1;
            machine(next).driver = player;
            players_[player].vehicle = next;
            say(player, next, K::Seated);
            break;
        }
        case Command::Cargo:
            if (v.has_blade) {
                v.blade = !v.blade;
                say(player, vi, K::Blade, v.blade_layer, 0, v.blade);
            } else if (v.capacity <= 0.0f) {
                break;
            } else if (x < quarry_end_) {
                v.gravel = v.capacity;
                say(player, vi, K::Loaded, v.capacity);
            } else if (v.gravel > 0.0f) {
                say(player, vi, K::HoldToTip);  // tipping happens in tick() while the tip control is held
            } else {
                say(player, vi, K::BedEmpty);
            }
            break;
        case Command::BladeLower:
        case Command::BladeRaise:
            if (!v.has_blade) break;
            // Below zero the edge goes under the original ground level, to scrape gravel out of ruts and holes.
            // The blade moves gravel only; digging the mud itself is not modelled yet.
            v.blade_layer = std::clamp(std::round((v.blade_layer + (c == Command::BladeRaise ? 0.05f : -0.05f)) * 20.0f) / 20.0f,
                                       -0.5f, 0.5f);
            say(player, vi, K::BladeLayer, v.blade_layer);
            break;
        case Command::Pave: {
            const int n = t.pave(under, kMinBase);
            say(player, vi, K::Paved, static_cast<float>(n) * t.cell_size() * t.cell_size(), kMinBase);
            break;
        }
        case Command::Roll: {
            const int n = t.roll(under);
            say(player, vi, K::Rolled, static_cast<float>(n) * t.cell_size() * t.cell_size());
            break;
        }
        case Command::Recover: {
            const ShiftMode mode = v.box.mode;
            v.rig = Rig(v.rig.spec(), t, SoilParams{}, v.home_x, course_.lane_y);
            v.lever = 0;
            v.box = ShiftController{};
            v.box.mode = mode;
            v.trip_running = false;
            say(player, vi, K::Recovered);
            break;
        }
    }
}

void World::tick() {
    for (Machine& m : machines_) tick_machine(m);
    ++ticks_;
}

void World::tick_machine(Machine& m) {
    Terrain& t = course_.terrain;
    const int vi = static_cast<int>(&m - machines_.data());

    // Inputs: the driver's held controls, or nobody on the pedals.
    const PlayerControls held = m.driver >= 0 ? players_[static_cast<std::size_t>(m.driver)].held : PlayerControls{};
    m.in.throttle = held.throttle;
    m.in.brake = held.brake;
    m.in.clutch = held.clutch;
    m.in.starter = held.starter;
    m.in.gear = m.lever;
    m.rig.set_payload(m.gravel * kGravelDensity);  // what is in the bed weighs on the wheels under it

    const float x0 = m.rig.x();
    for (int i = 0; i < kStepsPerTick; ++i) {
        m.in.shift = i == 0 ? m.pending_shift : 0;
        m.out = m.box.update(m.in, m.rig, kStepDt);
        m.rig.step(m.out, kStepDt);
    }
    m.pending_shift = 0;
    const float x = m.rig.x();
    float external = 0.0f;

    // Tipping: gravel leaves the bed while the control is held and lands ahead of the truck, a little each tick,
    // slumping as it lands: a short press drops a small heap, tipping on the move spreads a windrow. Never near
    // another vehicle: gravel on or against a vehicle is always a mistake, and not something to simulate.
    m.tipping = false;
    if (held.tip && m.capacity > 0.0f && m.gravel > 0.0f && x >= quarry_end_) {
        const float land = x + kTipAhead;
        int blocker = -1;
        for (const Machine& o : machines_) {
            if (&o == &m) continue;
            float lo, hi;
            extent(o, lo, hi);
            if (land > lo - kTipClearance && land < hi + kTipClearance) blocker = static_cast<int>(&o - machines_.data());
        }
        if (blocker >= 0) {
            if (!m.tip_refused) say(m.driver, vi, Event::Kind::TipBlocked, static_cast<float>(blocker));  // once, not every tick
            m.tip_refused = true;
        } else {
            m.tip_refused = false;
            const float out = std::min(m.gravel, kTipRate * kTickDt);
            m.gravel = m.gravel - out < 1e-3f ? 0.0f : m.gravel - out;
            // The load pours out over the whole tailgate, not from one point: a wider, lower heap than a cone
            // (10 m3: about 1.4 m high and 5.5 m across instead of 1.6 m and 5 m).
            constexpr int kPour = 5;
            for (int k = 0; k < kPour; ++k)
                t.dump_gravel(land, course_.lane_y + kTipWidth * (static_cast<float>(k) / (kPour - 1) - 0.5f), out / kPour);
            m.tipping = true;
            m.settle_x = land;
            m.settle_left = kSettleTime;
            if (m.gravel <= 0.0f) say(m.driver, vi, Event::Kind::BedEmpty);
        }
    } else {
        m.tip_refused = false;
    }
    if (m.settle_left > 0.0f) {
        const float ly = course_.lane_y;
        t.settle({m.settle_x - 4.0f, ly - 4.0f, m.settle_x + 4.0f, ly + 4.0f}, 4);
        m.settle_left -= kTickDt;
    }

    // The blade.
    if (m.has_blade) {
        m.blade_pile = 0.0f;
        m.ripping = false;
        if (m.blade && m.rig.speed() >= 0.0f) {
            // The edge holds the layer above the undisturbed ground under the tracks (firm ground plus mud, without
            // ruts or gravel), like grade control on a real machine. Following the ruts instead drops the blade as
            // the dozer sinks, so it bites deeper, slips, digs and sinks further; following the gravel makes it
            // climb onto the layer it has just laid and build a ramp.
            const auto& wheels = m.rig.spec().wheels;
            float base = 0.0f;
            for (const WheelSpec& w : wheels) {
                const Cell& c = t.at(x + w.x, m.rig.y() + w.y);
                base += c.hard + c.soft;
            }
            m.blade_edge = base / static_cast<float>(wheels.size()) + m.blade_layer;
            // Set below the original ground, the blade also rips up asphalt it passes under: the way to remove a
            // road laid in the wrong place. At or above ground level it only scrapes loose gravel off a road.
            const PushResult r = t.push({x + kBladeAhead, m.rig.y(), 1.0f, 0.0f, kBladeWidth, m.blade_edge, std::max(0.0f, x - x0),
                                         m.blade_layer < -0.01f});
            m.blade_pile = r.pile;
            m.ripping = r.ripped > 0;
        }
        m.blade_force = push_force(m.blade_pile);
        external += m.blade_force;
    }

    // The chassis ploughs loose gravel: what stands higher than the ground clearance between the wheel tracks is
    // pushed ahead of the leading axle, like a small blade, instead of lifting the vehicle (Rig only lets fixed
    // ground high-centre it). Not with a blade down: the blade does the pushing, and the chassis's own pass would
    // let the blade's heap slump back under the tracks.
    if (std::abs(x - x0) > 1e-4f && !(m.has_blade && m.blade)) {
        const VehicleSpec& s = m.rig.spec();
        const float dir = x > x0 ? 1.0f : -1.0f;
        float lead = dir > 0.0f ? -1e30f : 1e30f, ymin = 1e30f, ymax = -1e30f, ground = 0.0f;
        for (const WheelSpec& w : s.wheels) {
            lead = dir > 0.0f ? std::max(lead, w.x) : std::min(lead, w.x);
            ymin = std::min(ymin, w.y), ymax = std::max(ymax, w.y);
            ground += t.at(x + w.x, m.rig.y() + w.y).surface();
        }
        const float gap = (ymax - ymin) - s.tyre.width;  // between the inner edges of the wheel tracks
        if (gap > 0.1f) {
            const float belly = ground / static_cast<float>(s.wheels.size()) + s.clearance;
            const PushResult r = t.push({x + lead + dir * 0.5f * s.tyre.length, m.rig.y() + 0.5f * (ymin + ymax), dir, 0.0f, gap, belly,
                                         std::abs(x - x0)});
            external += push_force(r.pile);
        }
    }
    m.rig.set_external_resistance(external);

    // Trip clock: quarry gate to town gate, per vehicle.
    if (x0 < quarry_end_ && x >= quarry_end_) {
        m.trip_running = true;
        m.trip_time = 0.0f;
        m.trip_fuel0 = m.rig.fuel_kg();
    }
    if (m.trip_running) {
        m.trip_time += kTickDt;
        if (x >= town_start_) {
            m.trip_running = false;
            if (m.driver >= 0) say(m.driver, vi, Event::Kind::Arrived, m.trip_time, (m.rig.fuel_kg() - m.trip_fuel0) / 0.84f);
        } else if (x < quarry_end_) {
            m.trip_running = false;
        }
    }
}

std::unique_ptr<World> make_playground() {
    // 180 m of firm ground with 40 m of half-metre-deep mud between the quarry (up to 45 m) and the town (from 120 m).
    presets::Course c{Terrain(720, 48, 0.25f), 6.0f, 16.0f, 150.0f, {60.0f, 0.0f, 100.0f, 12.0f}, {58.0f, 3.5f, 102.0f, 8.5f}};
    Cell mud;
    mud.hard = -0.5f;
    mud.soft = 0.5f;
    c.terrain.fill(c.mud, mud);
    c.terrain.take_deltas();
    auto w = std::make_unique<World>(std::move(c), 45.0f, 120.0f);
    w->add_machine("STOCK TRUCK - HIGHWAY TYRES", presets::truck_6x6(presets::highway_tyre()), 14.0f, kLoad, false);
    w->add_machine("OFFROAD TRUCK - MUD TYRES", presets::truck_6x6(presets::mud_tyre()), 24.0f, kLoad, false);
    w->add_machine("DOZER - TRACKED", presets::dozer(), 34.0f, 0.0f, true);
    return w;
}

}  // namespace inc
