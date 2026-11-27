#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "inc/presets.h"
#include "inc/transmission.h"
#include "inc/world.h"

using namespace inc;

namespace {

int failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

const float kCruise = 60.0f / 3.6f;
const Driver kRoadDriver{kCruise, false, false, false};
const Driver kOffroadDriver{kCruise, false, true, true};
const float kDt = 1e-3f;

void run(Rig& rig, ShiftController& box, const DriverInput& in, float seconds) {
    for (int i = 0, n = static_cast<int>(seconds / kDt); i < n; ++i) rig.step(box.update(in, rig, kDt), kDt);
}

void run(Rig& rig, const Controls& u, float seconds) {
    for (int i = 0, n = static_cast<int>(seconds / kDt); i < n; ++i) rig.step(u, kDt);
}

void gravel_is_conserved() {
    Terrain t(200, 80, 0.25f);
    t.dump_gravel(20.0f, 10.0f, 10.0f);
    t.dump_gravel(26.0f, 10.0f, 10.0f);
    CHECK(near(t.gravel_volume(), 20.0, 0.02));
    t.press(20.0f, 10.0f, 0.5f, 0.5f, 0.2f);  // firm ground: no rut
    CHECK(t.at(20.0f, 10.0f).rut == 0.0f);

    const Rect road{10.0f, 5.0f, 36.0f, 15.0f};
    t.grade(road);
    CHECK(near(t.gravel_volume(), 20.0, 0.02));
    float lo = 1e9f, hi = -1e9f;
    for (float x = 10.5f; x < 36.0f; x += 1.0f) {
        lo = std::min(lo, t.at(x, 10.0f).surface());
        hi = std::max(hi, t.at(x, 10.0f).surface());
    }
    CHECK(hi - lo < 1e-3f);
}

void grading_fills_ruts() {
    Terrain t(80, 40, 0.25f);
    Cell mud;
    mud.hard = -0.5f;
    mud.soft = 0.5f;
    t.fill({0, 0, 20, 10}, mud);
    t.press(10.0f, 5.0f, 0.5f, 4.0f, 0.3f);
    t.dump_gravel(10.0f, 5.0f, 8.0f);
    t.grade({5, 2, 15, 8});
    CHECK(t.at(10.0f, 5.0f).gravel > t.at(10.0f, 3.0f).gravel + 0.25f);  // thicker where the rut was
    CHECK(near(t.at(10.0f, 5.0f).surface(), t.at(10.0f, 3.0f).surface(), 1e-3));
}

// Runs a blade in a straight line from (x0, y0) along (dx, dy) for a distance, in 5 cm steps.
// Returns the largest pile the blade pushed on the way.
float drive_blade(Terrain& t, float x0, float y0, float dx, float dy, float distance, float edge) {
    float peak = 0.0f;
    for (float d = 0.0f; d <= distance; d += 0.05f) peak = std::max(peak, t.push({x0 + d * dx, y0 + d * dy, dx, dy, 3.4f, edge, 0.05f}).pile);
    return peak;
}

void settling_reaches_the_angle_of_repose() {
    Terrain t(60, 60, 0.25f);
    Cell tower;
    tower.gravel = 3.0f;
    t.fill({7.0f, 7.0f, 7.9f, 7.9f}, tower);
    const double before = t.gravel_volume();
    t.settle({0.0f, 0.0f, 15.0f, 15.0f}, 400);
    CHECK(near(t.gravel_volume(), before, 1e-3));
    float steepest = 0.0f;
    for (int iy = 0; iy < 59; ++iy)
        for (int ix = 0; ix < 59; ++ix)
            steepest = std::max({steepest, std::abs(t.cell(ix, iy).surface() - t.cell(ix + 1, iy).surface()),
                                 std::abs(t.cell(ix, iy).surface() - t.cell(ix, iy + 1).surface())});
    CHECK(steepest < 0.7f * 0.25f + 0.01f);
}

// Three piles in a row, one blade pass at a 15 cm layer: gravel is conserved, the lane behind the blade is
// an even layer, the blade carries a pile, and what spills off the blade ends widens the layer.
void blade_spreads_a_layer() {
    Terrain t(400, 40, 0.25f);
    for (float x : {20.0f, 26.0f, 32.0f}) t.dump_gravel(x, 5.0f, 5.0f);
    const double before = t.gravel_volume();
    const float pile = drive_blade(t, 12.0f, 5.0f, 1.0f, 0.0f, 28.0f, 0.15f);
    CHECK(near(t.gravel_volume(), before, 0.01));
    CHECK(pile > 1.0f);
    bool even = true, held_back = true;
    for (float x = 20.0f; x < 35.0f; x += 0.25f) even &= near(t.at(x, 5.0f).surface(), 0.15, 0.02);
    for (float x = 37.0f; x < 39.7f; x += 0.25f)  // under the dozer, just behind the edge at 40 m
        for (float y = 4.0f; y <= 6.0f; y += 0.25f) held_back &= t.at(x, y).surface() <= 0.15f + 1e-4f;
    CHECK(even);
    CHECK(held_back);
    double spill = 0.0;  // beyond the blade ends (half width 1.7 m)
    for (float x = 15.0f; x < 40.0f; x += 0.25f) spill += t.at(x, 7.1f).gravel + t.at(x, 2.9f).gravel;
    CHECK(spill > 0.5);
}

// The blade moves gravel, never mud: with the edge below the mud it strips gravel off and leaves the ruts as they were;
// with the edge above the mud it fills the ruts so the top is flat.
void blade_cuts_gravel_not_mud() {
    Terrain t(200, 40, 0.25f);
    Cell mud;
    mud.hard = -0.5f;
    mud.soft = 0.5f;
    t.fill({0.0f, 0.0f, 50.0f, 10.0f}, mud);
    t.press(25.0f, 5.0f, 0.5f, 30.0f, 0.3f);  // a rut along the lane
    Terrain stripped = t;
    stripped.dump_gravel(20.0f, 5.0f, 6.0f);
    drive_blade(stripped, 12.0f, 5.0f, 1.0f, 0.0f, 20.0f, -1.0f);
    CHECK(near(stripped.at(20.0f, 5.0f).rut, 0.3, 1e-6));
    CHECK(stripped.at(20.0f, 5.0f).gravel < 1e-3f);

    t.dump_gravel(20.0f, 5.0f, 6.0f);
    drive_blade(t, 12.0f, 5.0f, 1.0f, 0.0f, 12.0f, 0.1f);
    CHECK(t.at(21.0f, 5.0f).gravel > t.at(21.0f, 4.0f).gravel + 0.25f);  // thicker in the rut
    CHECK(near(t.at(21.0f, 5.0f).surface(), t.at(21.0f, 4.0f).surface(), 0.02));
}

// Nothing in the blade depends on the grid axes: pushing diagonally conserves gravel and carries a pile too.
void blade_works_at_any_heading() {
    Terrain t(200, 200, 0.25f);
    t.dump_gravel(25.0f, 25.0f, 6.0f);
    const double before = t.gravel_volume();
    const float k = 0.70710678f;
    const float pile = drive_blade(t, 15.0f, 15.0f, k, k, 20.0f, 0.15f);
    CHECK(near(t.gravel_volume(), before, 0.01));
    CHECK(pile > 1.0f);
    CHECK(near(t.at(25.0f, 25.0f).surface(), 0.15, 0.02));
}

// Gravel tipped on a finished road lies on top of the asphalt, and a blade can push it off again
// without touching the road underneath.
void gravel_on_a_road_stays_on_top() {
    Terrain t(200, 40, 0.25f);
    const Rect road{5.0f, 2.0f, 45.0f, 8.0f};
    Cell base;
    base.gravel = 0.2f;
    t.fill(road, base);
    CHECK(t.pave(road, 0.15f) > 0);
    t.roll(road);
    const Cell paved = t.at(25.0f, 5.0f);
    t.dump_gravel(25.0f, 5.0f, 5.0f);
    const Cell& c = t.at(25.0f, 5.0f);
    CHECK(c.paving == Paving::Rolled && c.gravel == paved.gravel && c.loose > 0.5f);
    CHECK(c.surface() > paved.surface() + 0.5f);
    const double before = t.gravel_volume();
    drive_blade(t, 15.0f, 5.0f, 1.0f, 0.0f, 25.0f, -1.0f);  // edge below the road: it scrapes down to the asphalt
    CHECK(near(t.gravel_volume(), before, 0.01));
    CHECK(t.at(25.0f, 5.0f).loose < 1e-3f);
    CHECK(t.at(25.0f, 5.0f).gravel == paved.gravel && t.at(25.0f, 5.0f).paving == Paving::Rolled);
    Cell stray;
    stray.loose = 0.1f;  // loose gravel on unpaved ground is not a valid cell
    CHECK(t.apply({{0, stray}}) == 1);
}

// A blade set below the road rips the asphalt up into a loose base again, keeping every stone; without ripping
// the same pass leaves the road alone.
void blade_rips_up_a_road() {
    auto road = [] {
        Terrain t(200, 40, 0.25f);
        Cell base;
        base.gravel = 0.2f;
        t.fill({5.0f, 2.0f, 45.0f, 8.0f}, base);
        t.pave({5.0f, 2.0f, 45.0f, 8.0f}, 0.15f);
        t.roll({5.0f, 2.0f, 45.0f, 8.0f});
        return t;
    };
    Terrain kept = road(), ripped = road();
    const double before = ripped.gravel_volume();
    drive_blade(kept, 15.0f, 5.0f, 1.0f, 0.0f, 10.0f, 0.1f);  // edge inside the road, no ripping
    CHECK(kept.at(20.0f, 5.0f).paving == Paving::Rolled && kept.at(20.0f, 5.0f).gravel == 0.2f);
    int broken = 0;
    for (float d = 0.0f; d <= 10.0f; d += 0.05f) broken += ripped.push({15.0f + d, 5.0f, 1, 0, 3.4f, 0.1f, 0.05f, true}).ripped;
    CHECK(broken > 0);
    CHECK(ripped.at(20.0f, 5.0f).paving == Paving::None);
    CHECK(ripped.at(20.0f, 5.0f).surface() < 0.1f + 1e-3f);  // cut down to the edge, the rest pushed on
    CHECK(near(ripped.gravel_volume(), before, 0.01));
    CHECK(ripped.at(20.0f, 7.5f).paving == Paving::Rolled);  // beyond the blade's ends the road is untouched
}

void paving_needs_a_base() {
    Terrain t(40, 40, 0.25f);
    const Rect r{2, 2, 8, 8};
    CHECK(t.pave(r, 0.15f) == 0);
    t.dump_gravel(5.0f, 5.0f, 10.0f);
    t.grade(r);
    const int paved = t.pave(r, 0.15f);
    CHECK(paved > 0);
    CHECK(t.at(5.0f, 5.0f).paving == Paving::Laid);
    CHECK(t.roll(r) == paved);
    CHECK(t.at(5.0f, 5.0f).paving == Paving::Rolled);
}

void ground_pressure_decides_sinkage() {
    const SoilParams soil;
    Cell mud;
    mud.soft = 1.0f;
    const VehicleSpec truck = presets::truck_6x6(presets::highway_tyre());
    const VehicleSpec dozer = presets::dozer();
    const float wheel_load = truck.mass * 9.81f * truck.wheels[0].load_fraction;
    const float track_load = dozer.mass * 9.81f * 0.5f;

    const float highway = equilibrium_sinkage(mud, soil, presets::highway_tyre(), wheel_load);
    const float wide = equilibrium_sinkage(mud, soil, presets::mud_tyre(), wheel_load);
    const float track = equilibrium_sinkage(mud, soil, dozer.tyre, track_load);
    CHECK(highway > wide);
    CHECK(wide > track);  // the heavier machine sinks less on tracks

    Cell road = mud;
    road.gravel = 0.3f;
    CHECK(equilibrium_sinkage(road, soil, presets::highway_tyre(), wheel_load) < 0.25f * highway);

    // Ground that is already compacted this deep costs nothing more to cross.
    Cell rutted = mud;
    rutted.rut = wide;
    CHECK(compaction_resistance(mud, soil, presets::mud_tyre(), wheel_load) > 0.0f);
    CHECK(compaction_resistance(rutted, soil, presets::mud_tyre(), wheel_load) == 0.0f);
}

void differential_maths() {
    Shaft left, right, axle;
    left.wheel = 0;
    right.wheel = 1;
    axle.children = {left, right};
    // Wheel 1 is ten times easier to spin and has no load; wheel 0 resists.
    const std::vector<float> a{0.05f, 0.5f}, b{-40.0f, 0.0f};
    std::vector<float> torque(2);

    axle.lock = Lock::Open;
    distribute(axle, {}, 1000.0f, a, b, torque);
    CHECK(near(torque[0], 500.0, 1e-3) && near(torque[1], 500.0, 1e-3));

    axle.lock = Lock::Locked;
    distribute(axle, {}, 1000.0f, a, b, torque);
    CHECK(near(torque[0] + torque[1], 1000.0, 1e-2));
    CHECK(near(a[0] * torque[0] + b[0], a[1] * torque[1] + b[1], 1e-3));  // equal acceleration
    CHECK(torque[0] > torque[1]);                                          // torque goes to the wheel that resists

    std::vector<float> omega{2.0f, 10.0f};
    enforce_locks(axle, {}, omega);
    CHECK(near(omega[0], omega[1], 1e-5));

    const Gearbox box = presets::truck_6x6(presets::highway_tyre()).gearbox;
    CHECK(near(box.ratio(1, true), box.ratio(1, false) * box.low_range, 1e-3));
    CHECK(box.ratio(0, false) == 0.0f && box.ratio(-1, false) < 0.0f);
}

// The classic case for a diff lock: one wheel of each driven axle hanging in the air.
void diff_lock_gets_a_crossed_up_truck_moving() {
    auto distance_after_10s = [](bool lock) {
        Terrain t(400, 48, 0.25f);
        VehicleSpec truck = presets::truck_6x6(presets::highway_tyre());
        truck.wheels[2].load_fraction = 0.0f;   // rear-left of the first rear axle
        truck.wheels[3].load_fraction = 0.35f;
        truck.wheels[5].load_fraction = 0.0f;   // rear-right of the second
        truck.wheels[4].load_fraction = 0.35f;
        Rig rig(truck, t, SoilParams{}, 10.0f, 6.0f);
        ShiftController box;
        DriverInput in;
        in.gear = 1;
        in.throttle = 1.0f;
        in.diff_lock = lock;
        run(rig, box, in, 10.0f);
        return rig.x() - 10.0f;
    };
    const float open = distance_after_10s(false);
    const float locked = distance_after_10s(true);
    CHECK(open < 2.0f);
    CHECK(locked > 20.0f);
}

void hill() {
    Terrain t(400, 48, 0.25f);
    for (int ix = 0; ix < 400; ++ix) {  // 10% grade, paved
        Cell c;
        c.hard = 0.1f * (ix + 0.5f) * 0.25f;
        c.paving = Paving::Rolled;
        t.fill({ix * 0.25f + 0.01f, 0.0f, ix * 0.25f + 0.02f, 12.0f}, c);
    }
    Rig rolling(presets::truck_6x6(presets::highway_tyre()), t, SoilParams{}, 50.0f, 6.0f);
    Controls neutral;
    neutral.gear = 0;
    for (int i = 0; i < 5000; ++i) rolling.step(neutral, 1e-3f);
    CHECK(rolling.x() < 49.0f);  // rolls back

    Rig held(presets::truck_6x6(presets::highway_tyre()), t, SoilParams{}, 50.0f, 6.0f);
    Controls braked = neutral;
    braked.brake = 1.0f;
    for (int i = 0; i < 5000; ++i) held.step(braked, 1e-3f);
    CHECK(std::abs(held.x() - 50.0f) < 0.5f);

    Rig climbing(presets::truck_6x6(presets::highway_tyre()), t, SoilParams{}, 50.0f, 6.0f);
    CHECK(drive_to(climbing, kRoadDriver, 90.0f).reached);
}

void the_loop() {
    const VehicleSpec stock = presets::truck_6x6(presets::highway_tyre());
    const VehicleSpec equipped = presets::truck_6x6(presets::mud_tyre());

    presets::Course course = presets::mud_crossing();
    {
        Rig rig(stock, course.terrain, SoilParams{}, course.start_x, course.lane_y);
        const RunResult r = drive_to(rig, kRoadDriver, course.goal_x);
        CHECK(!r.reached);
        CHECK(r.x > course.mud.x0 && r.x < course.mud.x1);
    }

    course = presets::mud_crossing();
    Rig mud_run(equipped, course.terrain, SoilParams{}, course.start_x, course.lane_y);
    const RunResult through_mud = drive_to(mud_run, kOffroadDriver, course.goal_x);
    CHECK(through_mud.reached);

    {
        presets::Course c = presets::mud_crossing();
        Rig rig(presets::dozer(), c.terrain, SoilParams{}, c.start_x, c.lane_y);
        CHECK(drive_to(rig, kRoadDriver, c.goal_x).reached);
        CHECK(rig.mean_sinkage() < mud_run.mean_sinkage() + 1.0f);
    }

    for (int i = 0; i < 14; ++i) course.terrain.dump_gravel(course.mud.x0 + 3.5f + 7.15f * i, course.lane_y, 10.0f);
    course.terrain.grade(course.road);
    Rig gravel_run(stock, course.terrain, SoilParams{}, course.start_x, course.lane_y);
    const RunResult on_gravel = drive_to(gravel_run, kRoadDriver, course.goal_x);
    CHECK(on_gravel.reached);  // the truck that got stuck now crosses
    CHECK(on_gravel.time < 0.6f * through_mud.time);

    course.terrain.grade(course.road);
    CHECK(course.terrain.pave(course.road, 0.15f) > 0);
    course.terrain.roll(course.road);
    Rig asphalt_run(stock, course.terrain, SoilParams{}, course.start_x, course.lane_y);
    const RunResult on_asphalt = drive_to(asphalt_run, kRoadDriver, course.goal_x);
    CHECK(on_asphalt.reached);
    CHECK(on_asphalt.time <= on_gravel.time * 1.02f);
    CHECK(on_asphalt.fuel_kg < on_gravel.fuel_kg);
}

void traffic_wears_the_track_out() {
    presets::Course course = presets::mud_crossing();
    const VehicleSpec equipped = presets::truck_6x6(presets::mud_tyre());
    float first = 0.0f, last = 0.0f;
    bool stuck = false;
    for (int pass = 0; pass < 30 && !stuck; ++pass) {
        Rig rig(equipped, course.terrain, SoilParams{}, course.start_x, course.lane_y);
        const RunResult r = drive_to(rig, kOffroadDriver, course.goal_x);
        stuck = !r.reached;
        if (pass == 0) first = r.time;
        if (!stuck) last = r.time;
    }
    CHECK(stuck || last > first);
}

VehicleSpec stock_truck() { return presets::truck_6x6(presets::highway_tyre()); }

// Clutch down, first gear, let the clutch out with no throttle: the idle governor pulls the truck away.
float idle_creep(bool low_range) {
    Terrain t(800, 48, 0.25f);
    Rig rig(stock_truck(), t, SoilParams{}, 10.0f, 6.0f);
    Controls u;
    u.gear = 1;
    u.clutch = 1.0f;
    u.low_range = low_range;
    run(rig, u, 0.3f);
    CHECK(rig.gear() == 1 && rig.speed() == 0.0f);
    for (int i = 0; i < 2000; ++i) {
        u.clutch = 1.0f - i / 2000.0f;
        rig.step(u, kDt);
    }
    run(rig, u, 6.0f);
    CHECK(rig.engine_running());
    CHECK(rig.clutch_locked());
    return rig.speed();
}

void clutch_launch_and_low_range() {
    const VehicleSpec truck = stock_truck();
    const float idle_speed =
        truck.engine.idle_rpm * (2.0f * 3.14159265f / 60.0f) / truck.gearbox.ratio(1, false) * truck.wheel_radius;
    const float high = idle_creep(false);
    const float low = idle_creep(true);
    CHECK(near(high, idle_speed, 0.1 * idle_speed));
    CHECK(near(low, 0.5 * high, 0.05 * high));  // low range: half the speed at the same revs
}

void dumping_the_clutch_stalls() {
    Terrain t(400, 48, 0.25f);
    Rig rig(stock_truck(), t, SoilParams{}, 10.0f, 6.0f);
    Controls u;
    u.gear = 6;
    u.clutch = 1.0f;
    u.brake = 1.0f;
    run(rig, u, 0.3f);
    u.clutch = 0.0f;
    run(rig, u, 2.0f);
    CHECK(!rig.engine_running());
    CHECK(std::abs(rig.speed()) < 0.1f);

    u.gear = 0;
    u.starter = true;
    run(rig, u, 1.5f);
    CHECK(rig.engine_running());
}

void gears_grind_unless_matched() {
    Terrain t(1600, 48, 0.25f);
    Rig rig(stock_truck(), t, SoilParams{}, 10.0f, 6.0f);
    Controls u;
    u.gear = 1;  // clutch engaged, engine idling, truck standing: it will not go in
    rig.step(u, kDt);
    CHECK(rig.gear() == 0 && rig.grinding());

    u.clutch = 1.0f;
    run(rig, u, 0.2f);
    CHECK(rig.gear() == 1 && !rig.grinding());
    u.throttle = 0.6f;
    for (int i = 0; i < 1000; ++i) {
        u.clutch = 1.0f - i / 1000.0f;
        rig.step(u, kDt);
    }
    u.throttle = 1.0f;
    for (int i = 0; i < 20000 && rig.engine_rpm() < 1800.0f; ++i) rig.step(u, kDt);
    CHECK(rig.engine_rpm() >= 1800.0f);

    // Float into second: neutral, lift, and lean on the lever. It goes in when the revs have fallen far enough.
    u.gear = 0;
    u.throttle = 0.0f;
    rig.step(u, kDt);
    CHECK(rig.gear() == 0);
    u.gear = 2;
    rig.step(u, kDt);
    CHECK(rig.grinding());
    float mismatch = 1e9f;
    for (int i = 0; i < 5000 && rig.gear() != 2; ++i) {
        mismatch = std::abs(rig.engine_rpm() - rig.input_rpm(2));
        rig.step(u, kDt);
    }
    CHECK(rig.gear() == 2);
    CHECK(mismatch < 200.0f);
    CHECK(rig.engine_running());
}

void sequential_box_shifts_itself() {
    Terrain t(1600, 48, 0.25f);
    Rig rig(stock_truck(), t, SoilParams{}, 10.0f, 6.0f);
    ShiftController box;
    box.mode = ShiftMode::Sequential;
    DriverInput in;
    in.shift = 1;
    rig.step(box.update(in, rig, kDt), kDt);
    in.shift = 0;
    run(rig, box, in, 1.0f);
    CHECK(rig.gear() == 1 && rig.speed() == 0.0f && rig.engine_running());  // in gear, standing, clutch held open

    in.throttle = 1.0f;
    run(rig, box, in, 4.0f);
    CHECK(rig.speed() > 2.0f);
    in.shift = 1;
    rig.step(box.update(in, rig, kDt), kDt);
    in.shift = 0;
    run(rig, box, in, 1.0f);
    CHECK(rig.gear() == 2 && !box.shifting() && rig.engine_running());

    // Lift and brake to a stop in gear: it must not stall.
    in.throttle = 0.0f;
    in.brake = 1.0f;
    run(rig, box, in, 6.0f);
    CHECK(std::abs(rig.speed()) < 0.05f && rig.engine_running());
}

void automatic_box_reaches_top_gear_and_reverses() {
    Terrain t(4000, 48, 0.25f);
    Cell road;
    road.paving = Paving::Rolled;
    t.fill({0, 0, 1000, 12}, road);
    Rig rig(stock_truck(), t, SoilParams{}, 100.0f, 6.0f);
    ShiftController box;
    DriverInput in;
    in.gear = 1;
    in.throttle = 1.0f;
    run(rig, box, in, 45.0f);
    CHECK(rig.gear() == 6);
    CHECK(rig.speed() > 25.0f);

    in.throttle = 0.0f;
    in.brake = 1.0f;
    run(rig, box, in, 25.0f);
    CHECK(std::abs(rig.speed()) < 0.05f && rig.engine_running());
    const float x0 = rig.x();
    in.brake = 0.0f;
    in.gear = -1;
    in.throttle = 0.5f;
    run(rig, box, in, 5.0f);
    CHECK(rig.gear() == -1 && rig.x() < x0 - 2.0f);
}

// A dozer reverses through its own three gears, a little faster than it goes forward.
void dozer_reverses_as_fast_as_it_goes_forward() {
    Terrain t(4000, 48, 0.25f);
    auto top_speed = [&](int dir) {
        Rig rig(presets::dozer(), t, SoilParams{}, 500.0f, 6.0f);
        ShiftController box;
        DriverInput in;
        in.gear = dir;
        in.throttle = 1.0f;
        run(rig, box, in, 30.0f);
        CHECK(rig.gear() == 3 * dir);
        return std::abs(rig.speed());
    };
    const float forward = top_speed(1), reverse = top_speed(-1);
    CHECK(forward > 3.0f);
    CHECK(reverse > forward && reverse < 1.3f * forward);
}

// Gravel in the bed weighs on the rear bogie: more load there, deeper in the mud, slower to accelerate.
void payload_loads_the_rear_bogie() {
    presets::Course c = presets::mud_crossing();
    Rig empty(presets::truck_6x6(presets::mud_tyre()), c.terrain, SoilParams{}, 80.0f, c.lane_y);
    presets::Course c2 = presets::mud_crossing();
    Rig loaded(presets::truck_6x6(presets::mud_tyre()), c2.terrain, SoilParams{}, 80.0f, c2.lane_y);
    loaded.set_payload(18000.0f);  // 10 m3 of gravel
    Controls idle;
    empty.step(idle, kDt);
    loaded.step(idle, kDt);
    const auto e = empty.wheel_telemetry(), l = loaded.wheel_telemetry();
    CHECK(near(l[2].load - e[2].load, 18000.0 * 9.81 * 0.225, 1.0));  // rear: 90% of the payload over four wheels
    CHECK(near(l[0].load - e[0].load, 18000.0 * 9.81 * 0.05, 1.0));   // front: the rest
    CHECK(l[2].sinkage > e[2].sinkage * 1.3f);
    CHECK(near(loaded.mass(), 14000.0 + 18000.0, 1e-3));
}

// The handbrake parks a truck on a 10% grade with the engine idling in neutral.
void handbrake_holds_on_a_slope() {
    Terrain t(400, 48, 0.25f);
    for (int ix = 0; ix < 400; ++ix) {
        Cell c;
        c.hard = 0.1f * (ix + 0.5f) * 0.25f;
        c.paving = Paving::Rolled;
        t.fill({ix * 0.25f + 0.01f, 0.0f, ix * 0.25f + 0.02f, 12.0f}, c);
    }
    Rig rig(stock_truck(), t, SoilParams{}, 50.0f, 6.0f);
    Controls parked;
    parked.handbrake = true;
    run(rig, parked, 20.0f);
    CHECK(std::abs(rig.x() - 50.0f) < 0.05f);
    rig.set_payload(18000.0f);  // and loaded
    run(rig, parked, 20.0f);
    CHECK(std::abs(rig.x() - 50.0f) < 0.1f);
}

// Sequential: up is a higher gear in either direction, and reverse is refused at speed.
void sequential_reverse() {
    Terrain t(4000, 48, 0.25f);
    Rig dozer(presets::dozer(), t, SoilParams{}, 500.0f, 6.0f);
    ShiftController box;
    box.mode = ShiftMode::Sequential;
    DriverInput in;
    auto press = [&](int shift) {
        in.shift = shift;
        dozer.step(box.update(in, dozer, kDt), kDt);
        in.shift = 0;
        run(dozer, box, in, 1.0f);
    };
    press(-1);
    CHECK(box.selected() == -1);
    press(+1);
    press(+1);
    press(+1);
    CHECK(box.selected() == -3 && dozer.gear() == -3);
    press(-1);
    press(-1);
    press(-1);
    CHECK(box.selected() == 0);

    Rig truck(stock_truck(), t, SoilParams{}, 100.0f, 6.0f);
    ShiftController tbox;
    tbox.mode = ShiftMode::Sequential;
    DriverInput tin;
    tin.throttle = 1.0f;
    tin.shift = 1;
    truck.step(tbox.update(tin, truck, kDt), kDt);
    tin.shift = 0;
    run(truck, tbox, tin, 5.0f);
    CHECK(truck.speed() > 2.0f);
    tin.throttle = 0.0f;
    for (int shift : {-1, -1}) {  // 1 -> N -> (R refused while rolling)
        tin.shift = shift;
        truck.step(tbox.update(tin, truck, kDt), kDt);
        tin.shift = 0;
        run(truck, tbox, tin, 0.5f);
    }
    CHECK(tbox.selected() == 0 && truck.gear() == 0 && truck.engine_running());

    Gearbox none = stock_truck().gearbox;
    none.reverse.clear();
    CHECK(none.ratio(-1, false) == 0.0f);  // no such gear: neutral, not a read past the end
}

// Every vehicle ticks, driven or not: a parked engine keeps idling and burning fuel.
void parked_vehicles_keep_running() {
    auto w = make_playground();
    const PlayerId me = w->join();
    const int parked = (w->vehicle_of(me) + 1) % 3;
    const float fuel0 = w->machine(parked).rig.fuel_kg();
    for (int i = 0; i < 100; ++i) w->tick();
    CHECK(w->machine(parked).rig.fuel_kg() > fuel0);
    CHECK(w->machine(parked).rig.engine_running());
    CHECK(near(w->time(), 2.0, 1e-9));
}

// No tipping near another vehicle: the gravel stays in the bed and the player is told once.
void no_tipping_near_a_vehicle() {
    auto w = make_playground();
    const PlayerId me = w->join();  // the stock truck at 14 m
    Machine& truck = w->machine(w->vehicle_of(me));
    w->machine(1).rig = Rig(w->machine(1).rig.spec(), w->terrain(), SoilParams{}, 60.0f, w->course().lane_y);  // parked at 60 m
    truck.rig = Rig(truck.rig.spec(), w->terrain(), SoilParams{}, 50.0f, w->course().lane_y);  // gravel would land at 56 m
    truck.gravel = kLoad;
    PlayerControls tip;
    tip.tip = true;
    w->controls(me, tip);
    for (int i = 0; i < 50; ++i) w->tick();
    CHECK(truck.gravel == kLoad);
    int refusals = 0;
    for (const Event& e : w->take_events()) refusals += e.kind == Event::Kind::TipBlocked;
    CHECK(refusals == 1);
}

// A whole dozer pushing a heap: behind the blade, under its own tracks, the gravel stays at the set layer.
// (Catches anything else letting the heap slump back under the machine.)
void dozer_does_not_climb_its_own_heap() {
    auto w = make_playground();
    const PlayerId me = w->join();
    w->command(me, Command::NextVehicle);
    w->command(me, Command::NextVehicle);
    Machine& dozer = w->machine(w->vehicle_of(me));
    CHECK(dozer.has_blade);
    const float y = w->course().lane_y;
    w->terrain().dump_gravel(44.0f, y, kLoad);  // firm ground, 10 m ahead of the dozer at 34 m
    w->command(me, Command::Cargo);  // blade down, 20 cm layer
    w->command(me, Command::Gear1);
    PlayerControls go;
    go.throttle = 1.0f;
    w->controls(me, go);
    float worst = 0.0f;
    for (int i = 0; i < 50 * 12; ++i) {
        w->tick();
        const float x = dozer.rig.x();
        for (float d = -1.5f; d <= 1.0f; d += 0.25f)  // under the tracks, behind the blade
            for (float side : {-1.0f, 1.0f}) worst = std::max(worst, w->terrain().at(x + d, y + side).surface());
    }
    CHECK(worst < 0.2f + 0.08f);
}

// Choosing reverse lifts the dozer's blade, once, and it stays up going forward again.
void dozer_lifts_its_blade_to_reverse() {
    auto w = make_playground();
    const PlayerId me = w->join();
    w->command(me, Command::NextVehicle);
    w->command(me, Command::NextVehicle);
    Machine& dozer = w->machine(w->vehicle_of(me));
    w->command(me, Command::Cargo);
    CHECK(dozer.blade);
    w->take_events();
    w->command(me, Command::Reverse);
    for (int i = 0; i < 50; ++i) w->tick();
    CHECK(!dozer.blade);
    int lifted = 0;
    for (const Event& e : w->take_events()) lifted += e.kind == Event::Kind::Blade && !e.on && e.value2 > 0.5f;
    CHECK(lifted == 1);
    w->command(me, Command::Gear1);
    for (int i = 0; i < 50; ++i) w->tick();
    CHECK(!dozer.blade);
}

// A load tipped standing still pours over the whole tailgate: a heap wider and lower than a cone from one point.
void tipping_pours_across_the_tailgate() {
    auto w = make_playground();
    const PlayerId me = w->join();
    Machine& truck = w->machine(w->vehicle_of(me));
    w->machine(1).rig = Rig(w->machine(1).rig.spec(), w->terrain(), SoilParams{}, 160.0f, w->course().lane_y);  // out of the way
    w->machine(2).rig = Rig(w->machine(2).rig.spec(), w->terrain(), SoilParams{}, 170.0f, w->course().lane_y);
    truck.rig = Rig(truck.rig.spec(), w->terrain(), SoilParams{}, 50.0f, w->course().lane_y);
    truck.gravel = kLoad;
    PlayerControls tip;
    tip.tip = true;
    w->controls(me, tip);
    for (int i = 0; i < 50 * 10; ++i) w->tick();
    CHECK(truck.gravel == 0.0f);
    CHECK(near(w->terrain().gravel_volume(), kLoad, 0.01));
    const float land = 50.0f + kTipAhead, y = w->course().lane_y;
    CHECK(w->terrain().at(land, y).gravel < 1.5f);  // a point-poured cone would stand about 1.6 m
    CHECK(w->terrain().at(land, y + 2.0f).gravel > 0.4f && w->terrain().at(land, y - 2.0f).gravel > 0.4f);
}

// A heap between the wheels does not hold a truck up: the chassis ploughs it ahead. Gravel is only lost where
// the wheels spin climbing it, which throws a little away.
void a_heap_between_the_wheels_is_ploughed() {
    auto w = make_playground();
    const PlayerId me = w->join();
    Machine& truck = w->machine(w->vehicle_of(me));
    truck.rig = Rig(truck.rig.spec(), w->terrain(), SoilParams{}, 20.0f, w->course().lane_y);
    w->terrain().dump_gravel(30.0f, w->course().lane_y, 3.0f);  // a 3 m3 heap on firm ground, 0.9 m high
    const double before = w->terrain().gravel_volume();
    w->command(me, Command::Gear1);
    PlayerControls go;
    go.throttle = 0.6f;
    w->controls(me, go);
    for (int i = 0; i < 50 * 15; ++i) w->tick();
    CHECK(truck.rig.x() > 40.0f);
    CHECK(w->terrain().gravel_volume() > 0.9 * before && w->terrain().gravel_volume() < before + 0.01);
    // The 1 m peak is cut to the chassis line: 40 cm clearance above the wheels, which ride on the heap's flanks.
    CHECK(w->terrain().at(30.0f, w->course().lane_y).surface() < 0.6f);
}

// The resting pose: level on flat ground; a wheel rolls up a step gradually; a track tilts onto a heap and
// lies level once its centre is on it.
void vehicles_sit_on_the_ground() {
    Terrain t(200, 48, 0.25f);
    Cell step;
    step.gravel = 0.4f;
    t.fill({20.0f, 0.0f, 50.0f, 12.0f}, step);
    Rig truck(stock_truck(), t, SoilParams{}, 10.0f, 6.0f);
    const Pose flat = truck.pose(10.0f);
    CHECK(near(flat.pitch, 0.0, 1e-6) && near(flat.z, 0.0, 1e-6) && near(flat.roll, 0.0, 1e-6));
    // Front wheels (2.2 m ahead, radius 0.55) 45 cm short of the step: the rim touches it, partly up, not on top.
    const float front = truck.pose(20.0f - 2.2f - 0.45f).wheel_z[0];
    CHECK(front > 0.05f && front < 0.35f);
    CHECK(truck.pose(20.0f - 2.2f - 0.45f).pitch > 0.0f);

    Rig dozer(presets::dozer(), t, SoilParams{}, 10.0f, 6.0f);
    const Pose climbing = dozer.pose(19.0f);  // tracks from 17.5 to 20.5: the front on the step
    CHECK(climbing.pitch > 0.1f);
    const Pose on = dozer.pose(21.0f);  // centre on the step, the rear overhanging
    CHECK(near(on.pitch, 0.0, 1e-4) && near(on.z, 0.4, 1e-3));
}

void exhaust_brake_holds_a_descent() {
    auto speed_after_descent = [](bool exhaust_brake) {
        Terrain t(2400, 48, 0.25f);
        for (int ix = 0; ix < 2400; ++ix) {  // 10% downhill, paved
            Cell c;
            c.hard = -0.1f * (ix + 0.5f) * 0.25f;
            c.paving = Paving::Rolled;
            t.fill({ix * 0.25f + 0.01f, 0.0f, ix * 0.25f + 0.02f, 12.0f}, c);
        }
        Rig rig(stock_truck(), t, SoilParams{}, 20.0f, 6.0f);
        ShiftController box;
        box.mode = ShiftMode::Sequential;
        DriverInput in;
        in.engine_brake = exhaust_brake;
        for (int g = 0; g < 2; ++g) {  // second gear
            in.shift = 1;
            rig.step(box.update(in, rig, kDt), kDt);
            in.shift = 0;
            run(rig, box, in, 1.0f);
        }
        run(rig, box, in, 25.0f);
        CHECK(rig.gear() == 2 && rig.engine_running());
        return rig.speed();
    };
    const float with = speed_after_descent(true);
    const float without = speed_after_descent(false);
    CHECK(with > 0.5f);
    CHECK(with < 0.6f * without);
}

// A client that only ever receives deltas ends up with the same ground as the host.
void deltas_replicate_the_terrain() {
    presets::Course host = presets::mud_crossing();
    presets::Course client = presets::mud_crossing();
    Rig rig(presets::truck_6x6(presets::mud_tyre()), host.terrain, SoilParams{}, host.start_x, host.lane_y);
    ShiftController box;
    DriverInput in;
    in.gear = 1;
    in.throttle = 1.0f;
    in.awd = in.diff_lock = true;
    std::size_t sent = 0;
    for (int i = 0; i < 30000; ++i) {
        rig.step(box.update(in, rig, kDt), kDt);
        if (i % 50 == 49) {  // 20 Hz
            const auto deltas = host.terrain.take_deltas();
            sent += deltas.size();
            client.terrain.apply(deltas);
        }
    }
    host.terrain.dump_gravel(100.0f, 6.0f, 10.0f);
    host.terrain.grade(host.road);
    host.terrain.pave(host.road, 0.01f);
    client.terrain.apply(host.terrain.take_deltas());
    CHECK(sent > 0);
    CHECK(host.terrain == client.terrain);
    CHECK(host.terrain.take_deltas().empty());
}

// The handbrake locks the rear bogie only: the rear wheels slide, the fronts keep rolling, and the truck stops.
void handbrake_locks_the_rear_axles() {
    Terrain ground(2000, 16, 0.25f);
    Rig rig(presets::truck_6x6(presets::highway_tyre()), ground, SoilParams{}, 10.0f, 2.0f);
    ShiftController box;
    DriverInput in;
    in.gear = 1;
    in.throttle = 1.0f;
    run(rig, box, in, 6.0f);
    const float v0 = rig.speed();
    in.throttle = 0.0f;
    in.handbrake = true;
    run(rig, box, in, 0.4f);
    const float r = rig.spec().wheel_radius, v = rig.speed();
    const auto w = rig.wheel_speeds();
    CHECK(v0 > 5.0f);
    CHECK(v > 1.0f && v < v0);
    for (int i = 2; i < 6; ++i) CHECK(w[i] * r < 0.2f * v);  // rear: sliding
    for (int i = 0; i < 2; ++i) CHECK(w[i] * r > 0.8f * v);  // front: rolling
    run(rig, box, in, 10.0f);
    CHECK(std::abs(rig.speed()) < 0.05f);
}

// Deltas come off the network: out-of-range indices and malformed cells are skipped, not written.
void bad_deltas_are_rejected() {
    presets::Course c = presets::mud_crossing();
    const presets::Course before = presets::mud_crossing();
    const auto size = static_cast<std::uint32_t>(c.terrain.nx() * c.terrain.ny());
    Cell nan_cell;
    nan_cell.soft = std::nanf("");
    Cell deep_rut;
    deep_rut.soft = 0.2f;
    deep_rut.rut = 0.3f;
    Cell negative;
    negative.gravel = -1.0f;
    Cell bad_paving;
    bad_paving.paving = static_cast<Paving>(7);
    CHECK(c.terrain.apply({{size, Cell{}}, {0xFFFFFFFFu, Cell{}}, {0, nan_cell}, {1, deep_rut}, {2, negative}, {3, bad_paving}}) == 6);
    CHECK(c.terrain == before.terrain);

    Cell good;
    good.gravel = 0.2f;
    CHECK(c.terrain.apply({{size, Cell{}}, {5, good}}) == 1);
    CHECK(c.terrain.cell(5, 0) == good);
}

}  // namespace

int main() {
    gravel_is_conserved();
    grading_fills_ruts();
    settling_reaches_the_angle_of_repose();
    blade_spreads_a_layer();
    blade_cuts_gravel_not_mud();
    blade_works_at_any_heading();
    gravel_on_a_road_stays_on_top();
    blade_rips_up_a_road();
    paving_needs_a_base();
    ground_pressure_decides_sinkage();
    differential_maths();
    diff_lock_gets_a_crossed_up_truck_moving();
    hill();
    the_loop();
    traffic_wears_the_track_out();
    deltas_replicate_the_terrain();
    bad_deltas_are_rejected();
    handbrake_locks_the_rear_axles();
    clutch_launch_and_low_range();
    dumping_the_clutch_stalls();
    gears_grind_unless_matched();
    sequential_box_shifts_itself();
    automatic_box_reaches_top_gear_and_reverses();
    dozer_reverses_as_fast_as_it_goes_forward();
    payload_loads_the_rear_bogie();
    handbrake_holds_on_a_slope();
    sequential_reverse();
    parked_vehicles_keep_running();
    no_tipping_near_a_vehicle();
    a_heap_between_the_wheels_is_ploughed();
    tipping_pours_across_the_tailgate();
    dozer_does_not_climb_its_own_heap();
    dozer_lifts_its_blade_to_reverse();
    vehicles_sit_on_the_ground();
    exhaust_brake_holds_a_descent();
    std::printf(failures ? "%d check(s) failed\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
