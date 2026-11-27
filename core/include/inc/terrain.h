#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace inc {

enum class Paving : std::uint8_t { None, Laid, Rolled };

// One column of ground. Heights and thicknesses in metres.
struct Cell {
    float hard = 0.0f;    // elevation of firm ground
    float soft = 0.0f;    // thickness of soft soil (mud) above firm ground
    float gravel = 0.0f;  // thickness of aggregate lying on the mud
    float rut = 0.0f;     // how far traffic has pushed the mud surface down, 0..soft
    Paving paving = Paving::None;
    float loose = 0.0f;   // gravel lying on top of asphalt. On unpaved ground all the gravel is loose and this is 0

    float mud_top() const { return hard + soft - rut; }
    float surface() const { return mud_top() + gravel + loose; }
    // Gravel that can still be pushed, slump or be thrown by wheels: under asphalt it is bound into the road.
    float movable() const { return paving == Paving::None ? gravel : loose; }
    float& movable() { return paving == Paving::None ? gravel : loose; }
    float fixed_top() const { return surface() - movable(); }  // what is left if every loose stone is pushed away
    bool operator==(const Cell&) const = default;
};

struct CellDelta {
    std::uint32_t index;
    Cell cell;
};

struct Rect {
    float x0, y0, x1, y1;
};

// A dozer blade, as the ground sees it: a straight cutting edge at some height, pushing in some direction.
// Works for any heading, so the same call serves a one-lane prototype and a free-roaming 3D vehicle.
struct Blade {
    float x, y;                    // centre of the cutting edge [m]
    float dir_x = 1.0f, dir_y = 0.0f;  // unit vector the blade pushes towards
    float width = 3.4f;            // [m]
    float edge = 0.0f;             // elevation of the cutting edge [m]
    float swept = 0.0f;            // how far the edge moved since the last call [m]
};

struct PushResult {
    float cut;   // gravel cut and pushed this call [m3]
    float pile;  // gravel held above the edge in front of the blade [m3]; what the blade is pushing
};

// Regular grid of cells. Every edit records the touched cells, so a host can
// send only what changed (take_deltas) and a client can replay it (apply).
class Terrain {
public:
    Terrain(int nx, int ny, float cell_size);

    int nx() const { return nx_; }
    int ny() const { return ny_; }
    float cell_size() const { return cs_; }

    const Cell& cell(int ix, int iy) const { return cells_[index(ix, iy)]; }
    const Cell& at(float x, float y) const;  // cell containing the point, clamped to the grid
    void fill(Rect r, const Cell& c);

    // Traffic. width is the extent across the direction of travel (y), length along it (x).
    void press(float x, float y, float width, float length, float depth);
    void dig(float x, float y, float width, float length, float amount);

    // Road building. All of them conserve material.
    void dump_gravel(float x, float y, float volume);  // drops a conical pile; on asphalt it lies on top
    void grade(Rect r);                                // levels the gravel inside r at once (an ideal grader pass)
    // A blade pass: gravel above the edge is cut and heaped just ahead of the blade, the heap slumps to the
    // angle of repose (forward, into hollows, off the blade ends) but not back through the blade. Call it
    // every step while the blade is down.
    PushResult push(const Blade& b);
    void settle(Rect r, int passes = 8);               // lets gravel slump to its angle of repose
    int pave(Rect r, float min_base);                  // lays asphalt where gravel >= min_base; returns cells paved
    int roll(Rect r);                                  // compacts laid asphalt; returns cells rolled

    double gravel_volume() const;

    std::vector<CellDelta> take_deltas();
    // Deltas arrive from the network, so each one is checked before it is written: the index must be inside the
    // grid and the cell well-formed (finite, non-negative layers, 0 <= rut <= soft, a known paving state,
    // loose gravel only on asphalt).
    // Bad deltas are skipped and the rest applied. Returns how many were skipped; anything but 0 means this
    // copy no longer matches the sender and should ask for a full resync.
    std::size_t apply(const std::vector<CellDelta>& deltas);

    bool operator==(const Terrain& o) const { return cells_ == o.cells_; }

private:
    struct Span {
        int x0, x1, y0, y1;  // inclusive
    };
    std::size_t index(int ix, int iy) const { return static_cast<std::size_t>(iy) * nx_ + ix; }
    int clamp_x(float x) const;
    int clamp_y(float y) const;
    Span span(Rect r) const;
    void mark(std::size_t i);
    template <class Wall>
    void settle_span(Span s, int passes, Wall wall);  // wall(ix, iy): this cell takes no gravel

    int nx_, ny_;
    float cs_;
    std::vector<Cell> cells_;
    std::vector<std::uint8_t> dirty_flag_;
    std::vector<std::uint32_t> dirty_;
};

}  // namespace inc
