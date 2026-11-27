// The core loop in numbers: get stuck, get through the hard way, wear the track out, build a road, haul on it.
#include <algorithm>
#include <cstdio>

#include "inc/presets.h"

using namespace inc;

namespace {

constexpr float kCruise = 60.0f / 3.6f;

struct Setup {
    const char* label;
    VehicleSpec vehicle;
    Driver driver;
};

float deepest_rut(const Terrain& t, Rect r) {
    float deepest = 0.0f;
    for (float y = r.y0; y < r.y1; y += t.cell_size())
        for (float x = r.x0; x < r.x1; x += t.cell_size()) deepest = std::max(deepest, t.at(x, y).rut);
    return deepest;
}

RunResult run(const Setup& s, presets::Course& course) {
    Rig rig(s.vehicle, course.terrain, SoilParams{}, course.start_x, course.lane_y);
    return drive_to(rig, s.driver, course.goal_x);
}

void report(const char* label, const RunResult& r, const presets::Course& course) {
    const float mud_len = course.mud.x1 - course.mud.x0;
    if (r.reached)
        std::printf("  %-46s crossed   %6.1f s  %5.1f km/h avg  %5.2f L\n", label, r.time,
                    (r.x - course.start_x) / r.time * 3.6f, r.fuel_kg / 0.84f);
    else
        std::printf("  %-46s STUCK %3.0f m into the mud (of %.0f)\n", label,
                    std::clamp(r.x - course.mud.x0, 0.0f, mud_len), mud_len);
}

}  // namespace

int main() {
    const Driver road{kCruise, false, false, false};
    const Driver offroad{kCruise, false, true, true};
    const Setup stock{"highway tyres, RWD, open diffs", presets::truck_6x6(presets::highway_tyre()), road};
    const Setup stock_locked{"highway tyres, AWD + diff locks", presets::truck_6x6(presets::highway_tyre()), offroad};
    const Setup mud_rwd{"mud tyres, RWD, open diffs", presets::truck_6x6(presets::mud_tyre()), road};
    const Setup mud_full{"mud tyres, AWD + diff locks", presets::truck_6x6(presets::mud_tyre()), offroad};
    const Setup dozer{"tracked dozer", presets::dozer(), road};

    std::printf("260 m run, 100 m of it through half a metre of mud. Each line starts on untouched ground.\n");
    for (const Setup* s : {&stock, &stock_locked, &mud_rwd, &mud_full, &dozer}) {
        presets::Course course = presets::mud_crossing();
        report(s->label, run(*s, course), course);
    }

    std::printf("\nSame world from here on. The well-equipped truck keeps using the track:\n");
    presets::Course world = presets::mud_crossing();
    for (int pass = 1; pass <= 12; ++pass) {
        const RunResult r = run(mud_full, world);
        char label[64];
        std::snprintf(label, sizeof label, "pass %2d (deepest rut after: %2.0f cm)", pass,
                      deepest_rut(world.terrain, world.mud) * 100.0f);
        report(label, r, world);
        if (!r.reached) break;
    }

    std::printf("\nBuild a gravel road: 14 loads of 10 m3, then grade.\n");
    for (int i = 0; i < 14; ++i) world.terrain.dump_gravel(world.mud.x0 + 3.5f + 7.15f * i, world.lane_y, 10.0f);
    world.terrain.grade(world.road);
    std::printf("  gravel: %.0f cm on the crown, %.0f cm where the ruts were\n",
                world.terrain.at(110.0f, world.lane_y).gravel * 100.0f,
                world.terrain.at(110.0f, world.lane_y - 1.0f).gravel * 100.0f);
    report("stock truck on gravel, first pass", run(stock, world), world);
    report("stock truck on gravel, second pass", run(stock, world), world);

    std::printf("\nPave and roll it.\n");
    world.terrain.grade(world.road);
    const int paved = world.terrain.pave(world.road, 0.15f);
    world.terrain.roll(world.road);
    std::printf("  paved %.0f m2\n", paved * world.terrain.cell_size() * world.terrain.cell_size());
    report("stock truck on asphalt", run(stock, world), world);
    return 0;
}
