#pragma once

#include <string>

#include "inc/terrain.h"

namespace inc {

// Bekker pressure-sinkage and Janosi-Hanamoto shear parameters for the soft layer.
// The defaults are placeholders of a plausible order of magnitude for wet clay. They are not calibrated.
struct SoilParams {
    float n = 0.5f;          // sinkage exponent
    float k_c = 13e3f;       // cohesive modulus [Pa/m^(n-1)]
    float k_phi = 200e3f;    // frictional modulus [Pa/m^n]
    float cohesion = 4e3f;   // [Pa]
    float tan_phi = 0.23f;   // tangent of the internal friction angle
    float shear_k = 0.025f;  // shear deformation modulus [m]
    float dig_rate = 0.1f;   // rut depth gained per metre of wheel slip [m/m]
    float rut_drag = 0.5f;   // extra rolling resistance coefficient per metre of rut depth
    float viscous = 0.15f;   // growth of soft-ground resistance with speed [s/m]
};

// A tyre or a track: all the ground model needs is the contact patch and a few grip factors.
struct Tyre {
    std::string name;
    float width;      // contact patch across the direction of travel [m]
    float length;     // contact patch along it [m]
    float soft_grip;  // multiplier on traction in mud (tread, lugs, grousers)
    float hard_grip;  // multiplier on friction on firm ground, gravel and asphalt
    float crr_scale;  // multiplier on rolling resistance on hard surfaces
};

struct Contact {
    float max_traction;  // traction at full slip [N]
    float slip_gain;     // traction = max_traction * (1 - exp(-|slip| * slip_gain))
    float rolling;       // rolling resistance [N]
    float softness;      // 1 on mud, 0 on hard surfaces
    bool diggable;       // slip erodes this surface
};

// Sinkage into the soft layer under a static load, limited by the layer thickness.
// Gravel on top spreads the load (2:1 method), which is how a gravel road carries a truck over mud.
float equilibrium_sinkage(const Cell& cell, const SoilParams& soil, const Tyre& tyre, float load);

// Force needed to compact the ground at the leading edge of the contact patch.
// Zero if earlier traffic already compacted it at least as deep.
float compaction_resistance(const Cell& ahead, const SoilParams& soil, const Tyre& tyre, float load);

Contact contact(const Cell& under, const SoilParams& soil, const Tyre& tyre, float load);

}  // namespace inc
