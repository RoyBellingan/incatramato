#include "inc/presets.h"

namespace inc::presets {
namespace {

Shaft wheel(int i) {
    Shaft s;
    s.wheel = i;
    return s;
}

Shaft diff(Lock lock, std::vector<Shaft> children, bool awd_only = false) {
    Shaft s;
    s.lock = lock;
    s.children = std::move(children);
    s.awd_only = awd_only;
    return s;
}

}  // namespace

// Grip: firm earth, wet firm ground, loose gravel, rolled asphalt.
Tyre highway_tyre() { return {"highway", 0.30f, 0.30f, 0.8f, {0.55f, 0.30f, 0.36f, 0.90f}, 1.0f}; }
// Open lugs: better on anything loose, a smaller rubber patch on asphalt.
Tyre mud_tyre() { return {"mud", 0.45f, 0.50f, 1.3f, {0.60f, 0.38f, 0.42f, 0.75f}, 1.6f}; }

VehicleSpec truck_6x6(Tyre tyre) {
    VehicleSpec v;
    v.name = "6x6 truck";
    v.mass = 14000.0f;
    v.wheel_radius = 0.55f;
    v.wheel_inertia = 20.0f;
    v.clearance = 0.40f;
    v.brake_torque = 12000.0f;
    // Handbrake on the rear bogie; the bed sits over it, so it takes 90% of the payload.
    v.wheels = {{2.2f, -1.0f, 0.150f, false, 0.05f},   {2.2f, 1.0f, 0.150f, false, 0.05f},
                {-1.6f, -1.0f, 0.175f, true, 0.225f},  {-1.6f, 1.0f, 0.175f, true, 0.225f},
                {-2.9f, -1.0f, 0.175f, true, 0.225f},  {-2.9f, 1.0f, 0.175f, true, 0.225f}};
    v.engine = {{{600, 900}, {1000, 1300}, {1400, 1400}, {1800, 1300}, {2200, 1100}, {2500, 900}},
                650, 2500, 350, 2.5f, 180, 600, 400};
    v.clutch = {3000.0f};
    v.gearbox = {{7.0f, 4.0f, 2.5f, 1.6f, 1.0f, 0.75f}, {6.5f}, 5.5f, 2.0f};
    // Transfer case -> front axle (only with AWD) and rear bogie -> two axles.
    v.driveline = diff(Lock::WithAwd,
                       {diff(Lock::WithDiffLock, {wheel(0), wheel(1)}, true),
                        diff(Lock::WithDiffLock, {diff(Lock::WithDiffLock, {wheel(2), wheel(3)}),
                                                  diff(Lock::WithDiffLock, {wheel(4), wheel(5)})})});
    v.tyre = std::move(tyre);
    return v;
}

VehicleSpec dozer() {
    VehicleSpec v;
    v.name = "tracked dozer";
    v.mass = 20000.0f;
    v.wheel_radius = 0.45f;
    v.wheel_inertia = 80.0f;
    v.clearance = 0.45f;
    v.brake_torque = 40000.0f;
    v.wheels = {{0.0f, -1.0f, 0.5f, true}, {0.0f, 1.0f, 0.5f, true}};
    v.engine = {{{800, 900}, {1200, 1100}, {1600, 1050}, {2000, 900}, {2200, 800}}, 800, 2200, 400, 3.0f, 160, 400, 400};
    v.clutch = {2500.0f};
    v.gearbox = {{3.0f, 1.8f, 1.1f}, {2.6f, 1.55f, 0.95f}, 22.0f, 1.0f};  // reverse about 15% faster, for the run back
    v.driveline = diff(Lock::Locked, {wheel(0), wheel(1)});
    // Steel grousers: about 0.9 on earth and 0.6 at the bottom of a muddy rut, but only 0.45 on asphalt or concrete. In mud they shear
    // the soil along the whole track, which tyres cannot: about 1.5 times a mud tyre's grip there.
    v.tyre = {"track", 0.60f, 3.00f, 1.8f, {0.90f, 0.60f, 0.50f, 0.45f}, 2.0f};
    return v;
}

Course mud_crossing() {
    Course c{Terrain(1200, 48, 0.25f), 6.0f, 20.0f, 280.0f, {60.0f, 0.0f, 160.0f, 12.0f}, {58.0f, 3.5f, 162.0f, 8.5f}};
    Cell mud;
    mud.hard = -0.5f;
    mud.soft = 0.5f;
    c.terrain.fill(c.mud, mud);
    c.terrain.take_deltas();  // the initial state is not an edit
    return c;
}

}  // namespace inc::presets
