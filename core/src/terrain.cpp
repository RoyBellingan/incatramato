#include "inc/terrain.h"

#include <algorithm>
#include <cmath>

namespace inc {
namespace {

constexpr float kTanRepose = 0.7f;  // gravel stands at about 35 degrees

}  // namespace

Terrain::Terrain(int nx, int ny, float cell_size)
    : nx_(nx), ny_(ny), cs_(cell_size), cells_(static_cast<std::size_t>(nx) * ny), dirty_flag_(cells_.size(), 0) {}

int Terrain::clamp_x(float x) const { return std::clamp(static_cast<int>(std::floor(x / cs_)), 0, nx_ - 1); }
int Terrain::clamp_y(float y) const { return std::clamp(static_cast<int>(std::floor(y / cs_)), 0, ny_ - 1); }

Terrain::Span Terrain::span(Rect r) const { return {clamp_x(r.x0), clamp_x(r.x1), clamp_y(r.y0), clamp_y(r.y1)}; }

const Cell& Terrain::at(float x, float y) const { return cells_[index(clamp_x(x), clamp_y(y))]; }

void Terrain::mark(std::size_t i) {
    if (!dirty_flag_[i]) {
        dirty_flag_[i] = 1;
        dirty_.push_back(static_cast<std::uint32_t>(i));
    }
}

void Terrain::fill(Rect r, const Cell& c) {
    const Span s = span(r);
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            cells_[index(ix, iy)] = c;
            mark(index(ix, iy));
        }
}

void Terrain::press(float x, float y, float width, float length, float depth) {
    if (depth <= 0.0f) return;
    const Span s = span({x - 0.5f * length, y - 0.5f * width, x + 0.5f * length, y + 0.5f * width});
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            Cell& c = cells_[index(ix, iy)];
            if (c.paving != Paving::None) continue;
            const float d = std::min(depth, c.soft);
            if (d > c.rut) {
                c.rut = d;
                mark(index(ix, iy));
            }
        }
}

void Terrain::dig(float x, float y, float width, float length, float amount) {
    if (amount <= 0.0f) return;
    const Span s = span({x - 0.5f * length, y - 0.5f * width, x + 0.5f * length, y + 0.5f * width});
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            Cell& c = cells_[index(ix, iy)];
            if (c.movable() > 0.0f) {
                c.movable() = std::max(0.0f, c.movable() - amount);  // spinning wheels throw the aggregate away
            } else if (c.paving != Paving::None) {
                continue;
            } else if (c.rut < c.soft) {
                c.rut = std::min(c.soft, c.rut + amount);
            } else {
                continue;
            }
            mark(index(ix, iy));
        }
}

void Terrain::dump_gravel(float x, float y, float volume) {
    if (volume <= 0.0f) return;
    constexpr float kPi = 3.14159265f;
    const float radius = std::cbrt(3.0f * volume / (kPi * kTanRepose));
    const float height = radius * kTanRepose;
    const Span s = span({x - radius, y - radius, x + radius, y + radius});

    auto cone = [&](int ix, int iy) {
        const float d = std::hypot((ix + 0.5f) * cs_ - x, (iy + 0.5f) * cs_ - y);
        return d < radius ? height * (1.0f - d / radius) : 0.0f;
    };
    double sum = 0.0;
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) sum += cone(ix, iy);

    if (sum <= 0.0) {  // pile smaller than a cell
        const std::size_t i = index(clamp_x(x), clamp_y(y));
        cells_[i].movable() += volume / (cs_ * cs_);
        mark(i);
        return;
    }
    // Rescale so the discretised pile holds exactly the dumped volume.
    const float scale = static_cast<float>(volume / (sum * cs_ * cs_));
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            const float h = cone(ix, iy);
            if (h > 0.0f) {
                cells_[index(ix, iy)].movable() += h * scale;
                mark(index(ix, iy));
            }
        }
}

void Terrain::grade(Rect r) {
    const Span s = span(r);
    double total = 0.0;
    float lo = 1e30f, hi = -1e30f;
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            const Cell& c = cells_[index(ix, iy)];
            if (c.paving != Paving::None) continue;
            total += c.gravel;
            lo = std::min(lo, c.mud_top());
            hi = std::max(hi, c.mud_top());
        }
    if (total <= 0.0) return;

    // Find the level T at which the same amount of gravel, laid on the mud, has a flat top.
    auto held = [&](double level) {
        double sum = 0.0;
        for (int iy = s.y0; iy <= s.y1; ++iy)
            for (int ix = s.x0; ix <= s.x1; ++ix) {
                const Cell& c = cells_[index(ix, iy)];
                if (c.paving == Paving::None) sum += std::max(0.0, level - c.mud_top());
            }
        return sum;
    };
    double a = lo, b = hi + total;
    for (int i = 0; i < 60; ++i) {
        const double mid = 0.5 * (a + b);
        (held(mid) < total ? a : b) = mid;
    }
    const double level = 0.5 * (a + b);
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            Cell& c = cells_[index(ix, iy)];
            if (c.paving != Paving::None) continue;
            const float g = static_cast<float>(std::max(0.0, level - c.mud_top()));
            if (g != c.gravel) {
                c.gravel = g;
                mark(index(ix, iy));
            }
        }
}

template <class Wall>
void Terrain::settle_span(Span s, int passes, Wall wall) {
    // Pairwise exchange between neighbours (right, down and both diagonals, so each pair is visited once).
    // Gravel moves from the higher surface to the lower one until the slope is back at the angle of repose;
    // moving a quarter of the excess per visit keeps cells that share neighbours from overshooting.
    // Only loose gravel moves (on asphalt, the layer on top); mud and the road base stay put. Each move is a
    // transfer, so the total is conserved.
    static constexpr int kOffsets[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
    const float limit[2] = {kTanRepose * cs_, kTanRepose * cs_ * 1.41421356f};
    for (int pass = 0; pass < passes; ++pass) {
        bool moved = false;
        for (int iy = s.y0; iy <= s.y1; ++iy)
            for (int ix = s.x0; ix <= s.x1; ++ix) {
                for (int k = 0; k < 4; ++k) {
                    const int jx = ix + kOffsets[k][0], jy = iy + kOffsets[k][1];
                    if (jx < s.x0 || jx > s.x1 || jy > s.y1) continue;
                    const std::size_t a = index(ix, iy), b = index(jx, jy);
                    const float diff = cells_[a].surface() - cells_[b].surface();
                    const float lim = limit[k >= 2];
                    if (std::abs(diff) <= lim) continue;
                    const bool down = diff > 0.0f;  // a is higher: gravel flows a -> b
                    const std::size_t from = down ? a : b, to = down ? b : a;
                    if (down ? wall(jx, jy) : wall(ix, iy)) continue;
                    const float amount = std::min(cells_[from].movable(), 0.25f * (std::abs(diff) - lim));
                    if (amount <= 0.0f) continue;
                    cells_[from].movable() -= amount;
                    cells_[to].movable() += amount;
                    mark(from);
                    mark(to);
                    moved = true;
                }
            }
        if (!moved) break;
    }
}

void Terrain::settle(Rect r, int passes) {
    settle_span(span(r), passes, [](int, int) { return false; });
}

PushResult Terrain::push(const Blade& b) {
    constexpr float kHeapDepth = 0.5f;  // cut gravel lands in this band just ahead of the edge [m]
    constexpr float kPileReach = 4.0f;  // gravel above the edge this far ahead counts as pushed [m]
    constexpr float kUnder = 3.5f;      // behind the edge the dozer stands on the ground; nothing slumps there [m]
    constexpr float kSpill = 3.0f;      // room either side for the windrows [m]
    const float half = 0.5f * b.width, band = std::max(cs_, b.swept);
    const float ax = -b.dir_y, ay = b.dir_x;  // across the blade

    // Box around everything the pass can touch: under the dozer, the blade, the heap and its spill.
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (float along : {-kUnder, kPileReach + 1.0f})
        for (float across : {-half - kSpill, half + kSpill}) {
            const float px = b.x + along * b.dir_x + across * ax, py = b.y + along * b.dir_y + across * ay;
            x0 = std::min(x0, px), x1 = std::max(x1, px), y0 = std::min(y0, py), y1 = std::max(y1, py);
        }
    const Span s = span({x0, y0, x1, y1});
    auto local = [&](int ix, int iy, float& along, float& across) {
        const float dx = (ix + 0.5f) * cs_ - b.x, dy = (iy + 0.5f) * cs_ - b.y;
        along = dx * b.dir_x + dy * b.dir_y;
        across = dx * ax + dy * ay;
    };
    auto floor_of = [&](const Cell& c) { return std::max(b.edge, c.fixed_top()); };  // it moves gravel, not mud or road

    // Where the heap goes. Without room for it (off the grid) the blade cuts nothing.
    std::vector<std::size_t> heap;
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            float along, across;
            local(ix, iy, along, across);
            if (along > 0.0f && along <= kHeapDepth && std::abs(across) <= half) heap.push_back(index(ix, iy));
        }

    double cut = 0.0;
    if (!heap.empty()) {
        for (int iy = s.y0; iy <= s.y1; ++iy)
            for (int ix = s.x0; ix <= s.x1; ++ix) {
                float along, across;
                local(ix, iy, along, across);
                if (along > 0.0f || along <= -band || std::abs(across) > half) continue;
                Cell& c = cells_[index(ix, iy)];
                const float take = std::min(c.movable(), c.surface() - floor_of(c));
                if (take <= 0.0f) continue;
                c.movable() -= take;
                cut += take;
                mark(index(ix, iy));
            }
        const auto share = static_cast<float>(cut / static_cast<double>(heap.size()));
        if (share > 0.0f)
            for (std::size_t i : heap) {
                cells_[i].movable() += share;
                mark(i);
            }
    }

    settle_span(s, 4, [&](int ix, int iy) {
        float along, across;
        local(ix, iy, along, across);
        return along <= 0.0f && along > -kUnder && std::abs(across) <= half;
    });

    // What the blade pushes is the heap against it: gravel above the edge connected to the band at the blade face.
    // A pile further ahead is not resisting yet.
    const int w = s.x1 - s.x0 + 1, hgt = s.y1 - s.y0 + 1;
    std::vector<char> seen(static_cast<std::size_t>(w) * hgt, 0);
    std::vector<std::pair<int, int>> todo;
    auto above = [&](int ix, int iy) {
        const Cell& c = cells_[index(ix, iy)];
        return std::clamp(c.surface() - floor_of(c), 0.0f, c.movable());
    };
    auto in_zone = [&](int ix, int iy) {
        float along, across;
        local(ix, iy, along, across);
        return along > 0.0f && along <= kPileReach && std::abs(across) <= half;
    };
    auto visit = [&](int ix, int iy, bool seed) {
        if (ix < s.x0 || ix > s.x1 || iy < s.y0 || iy > s.y1) return;
        char& v = seen[static_cast<std::size_t>(iy - s.y0) * w + (ix - s.x0)];
        if (v || !in_zone(ix, iy) || above(ix, iy) <= 1e-4f) return;
        float along, across;
        local(ix, iy, along, across);
        if (seed && along > kHeapDepth) return;
        v = 1;
        todo.emplace_back(ix, iy);
    };
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) visit(ix, iy, true);
    double pile = 0.0;
    while (!todo.empty()) {
        const auto [ix, iy] = todo.back();
        todo.pop_back();
        pile += above(ix, iy);
        visit(ix + 1, iy, false), visit(ix - 1, iy, false), visit(ix, iy + 1, false), visit(ix, iy - 1, false);
    }
    const double area = static_cast<double>(cs_) * cs_;
    return {static_cast<float>(cut * area), static_cast<float>(pile * area)};
}

int Terrain::pave(Rect r, float min_base) {
    const Span s = span(r);
    int count = 0;
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            Cell& c = cells_[index(ix, iy)];
            if (c.paving == Paving::None && c.gravel >= min_base) {
                c.paving = Paving::Laid;
                mark(index(ix, iy));
                ++count;
            }
        }
    return count;
}

int Terrain::roll(Rect r) {
    const Span s = span(r);
    int count = 0;
    for (int iy = s.y0; iy <= s.y1; ++iy)
        for (int ix = s.x0; ix <= s.x1; ++ix) {
            Cell& c = cells_[index(ix, iy)];
            if (c.paving == Paving::Laid) {
                c.paving = Paving::Rolled;
                mark(index(ix, iy));
                ++count;
            }
        }
    return count;
}

double Terrain::gravel_volume() const {
    double sum = 0.0;
    for (const Cell& c : cells_) sum += c.gravel + c.loose;
    return sum * cs_ * cs_;
}

std::vector<CellDelta> Terrain::take_deltas() {
    std::vector<CellDelta> out;
    out.reserve(dirty_.size());
    for (std::uint32_t i : dirty_) {
        out.push_back({i, cells_[i]});
        dirty_flag_[i] = 0;
    }
    dirty_.clear();
    return out;
}

namespace {

bool well_formed(const Cell& c) {
    const bool finite = std::isfinite(c.hard) && std::isfinite(c.soft) && std::isfinite(c.gravel) && std::isfinite(c.rut) &&
                        std::isfinite(c.loose);
    const bool known_paving = c.paving == Paving::None || c.paving == Paving::Laid || c.paving == Paving::Rolled;
    const bool loose_on_road = c.loose >= 0.0f && (c.paving != Paving::None || c.loose == 0.0f);
    return finite && known_paving && loose_on_road && c.soft >= 0.0f && c.gravel >= 0.0f && c.rut >= 0.0f && c.rut <= c.soft;
}

}  // namespace

std::size_t Terrain::apply(const std::vector<CellDelta>& deltas) {
    std::size_t rejected = 0;
    for (const CellDelta& d : deltas) {
        if (d.index >= cells_.size() || !well_formed(d.cell)) {
            ++rejected;
            continue;
        }
        cells_[d.index] = d.cell;
    }
    return rejected;
}

}  // namespace inc
