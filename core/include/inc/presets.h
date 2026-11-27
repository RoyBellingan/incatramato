#pragma once

#include "inc/rig.h"

// Hard-coded content for the prototype. These become data files (the mod format) once the game loads assets.
namespace inc::presets {

Tyre highway_tyre();
Tyre mud_tyre();

VehicleSpec truck_6x6(Tyre tyre);  // part-time AWD, lockable axle and inter-axle differentials
VehicleSpec dozer();               // two tracks on a solid drive

// 300 m x 12 m strip of firm ground with a 100 m stretch of half-metre-deep mud across it.
struct Course {
    Terrain terrain;
    float lane_y;
    float start_x, goal_x;
    Rect mud;   // the mud section
    Rect road;  // where a road across it would go
};
Course mud_crossing();

}  // namespace inc::presets
