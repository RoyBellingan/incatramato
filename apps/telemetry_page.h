#pragma once

// The telemetry dashboard served at GET / by telemetry.h. Self-contained: no external scripts or fonts, so it
// works offline and on a tablet on the same network.
namespace telemetry {

inline constexpr const char* kPage = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Incatramato Telemetry</title>
<style>
:root {
  --bg: #15171b; --panel: #1e2126; --line: #2e3238; --text: #e8e6e1; --dim: #8c9096;
  --green: #4cc061; --amber: #e8a93a; --red: #e0574a; --blue: #5aa7e6; --violet: #a98be0;
  --firm: #8a7458; --mud: #5a3f2a; --gravel: #9a9893; --asphalt: #3a3b3f; --sky: #1b2a38;
}
@media (prefers-color-scheme: light) {
  :root:not([data-theme="dark"]) {
    --bg: #f2f1ee; --panel: #ffffff; --line: #dcdad5; --text: #1d1f22; --dim: #6b6e73;
    --green: #2f9a44; --amber: #b97a12; --red: #c63d31; --blue: #2b78bd; --violet: #7a5cc0;
    --firm: #b09a7c; --mud: #7a5a40; --gravel: #a9a7a1; --asphalt: #4a4b50; --sky: #dfe9f2;
  }
}
* { box-sizing: border-box; }
html, body { margin: 0; background: var(--bg); color: var(--text); font: 14px/1.35 system-ui, sans-serif; }
.num { font-variant-numeric: tabular-nums; }
header { display: flex; flex-wrap: wrap; align-items: center; gap: 8px 28px; padding: 12px 16px; border-bottom: 1px solid var(--line); }
header .name { font-size: 18px; font-weight: 600; }
header .mode { color: var(--dim); font-size: 12px; letter-spacing: .06em; text-transform: uppercase; }
header .big { font-size: 30px; font-weight: 600; }
header .unit { color: var(--dim); font-size: 12px; margin-left: 3px; }
header .gear { font-size: 30px; font-weight: 700; min-width: 1.6em; text-align: center; border: 1px solid var(--line); border-radius: 6px; padding: 0 8px; }
.status { margin-left: auto; display: flex; align-items: center; gap: 8px; color: var(--dim); font-size: 12px; }
.dot { width: 10px; height: 10px; border-radius: 50%; background: var(--red); }
.dot.live { background: var(--green); } .dot.paused { background: var(--amber); }
main { display: grid; grid-template-columns: repeat(auto-fit, minmax(330px, 1fr)); gap: 12px; padding: 12px 16px 24px; }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 12px 14px; min-width: 0; }
.card h2 { margin: 0 0 10px; font-size: 12px; font-weight: 600; letter-spacing: .06em; text-transform: uppercase; color: var(--dim); }
.wide { grid-column: 1 / -1; }
.note { color: var(--dim); font-size: 12px; margin: 8px 0 0; }
.bar { display: grid; grid-template-columns: 110px 1fr 92px; align-items: center; gap: 8px; margin: 6px 0; }
.bar .track { position: relative; height: 12px; background: var(--line); border-radius: 3px; overflow: hidden; }
.bar .fill { position: absolute; top: 0; bottom: 0; left: 0; background: var(--blue); }
.bar .mark { position: absolute; top: -2px; bottom: -2px; width: 2px; background: var(--text); opacity: .6; }
.bar .zero { position: absolute; top: -2px; bottom: -2px; left: 50%; width: 1px; background: var(--dim); }
.bar .val { text-align: right; font-variant-numeric: tabular-nums; }
.lamps { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 10px; }
.lamp { font-size: 11px; font-weight: 600; letter-spacing: .05em; padding: 3px 8px; border-radius: 4px; background: var(--line); color: var(--dim); }
.lamp.on { color: var(--bg); }
.lamp.on.g { background: var(--green); } .lamp.on.a { background: var(--amber); } .lamp.on.r { background: var(--red); }
canvas { display: block; width: 100%; }
#plan { height: 170px; }
#profile { height: 230px; }
.charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 14px; }
.charts canvas { height: 130px; }
.table-wrap { overflow-x: auto; margin-top: 10px; }
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; font-size: 13px; }
th, td { padding: 4px 8px; text-align: right; border-bottom: 1px solid var(--line); white-space: nowrap; }
th { color: var(--dim); font-weight: 500; font-size: 12px; }
th:first-child, td:first-child { text-align: left; }
.hidden { display: none; }
@media (max-width: 480px) { header { padding: 10px 16px; } main { padding: 10px 16px; } .bar { grid-template-columns: 90px 1fr 80px; } }
</style>
</head>
<body>
<header>
  <div><div class="name" id="name">Waiting for the game…</div><div class="mode" id="mode"></div></div>
  <div class="gear num" id="gear">–</div>
  <div><span class="big num" id="speed">0</span><span class="unit">km/h</span></div>
  <div><span class="big num" id="rpm">0</span><span class="unit">rpm</span></div>
  <a href="/3d" style="color:var(--blue)">3D view of the ground</a>
  <div class="status"><span class="dot" id="dot"></span><span id="status">connecting</span></div>
</header>
<main>
  <section class="card">
    <h2>Driveline</h2>
    <div id="driveline"></div>
    <div class="lamps" id="lamps"></div>
  </section>
  <section class="card">
    <h2>Forces along the track</h2>
    <div id="forces"></div>
    <p class="note">Positive pushes the vehicle forward. Resistances are passive: they stop motion but never start it.</p>
  </section>
  <section class="card hidden" id="blade-card">
    <h2>Blade</h2>
    <div id="blade"></div>
  </section>
  <section class="card wide">
    <h2>Wheels and tracks</h2>
    <canvas id="plan"></canvas>
    <div class="table-wrap"><table id="wheels"></table></div>
    <p class="note">No suspension yet: the rig is a straight-line model and each wheel carries a fixed share of the weight. Suspension travel arrives with the 3D vehicle (milestone M1); sinkage and ruts are live.</p>
  </section>
  <section class="card wide">
    <h2>Ground under the vehicle (left wheel track, vertical scale exaggerated)</h2>
    <canvas id="profile"></canvas>
  </section>
  <section class="card wide">
    <h2>Last 30 seconds</h2>
    <div class="charts">
      <canvas id="c-speed"></canvas><canvas id="c-rpm"></canvas><canvas id="c-force"></canvas>
      <canvas id="c-sink"></canvas><canvas id="c-slip"></canvas><canvas id="c-fuel"></canvas>
    </div>
  </section>
</main>
<script>
"use strict";
const $ = (id) => document.getElementById(id);
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const kN = (n) => (n / 1000).toFixed(1);
const WINDOW = 30;
let hist = [];
let last = null, lastAt = 0;

function bar(label, value, max, text, opts = {}) {
  const centred = !!opts.centred;
  const frac = Math.max(-1, Math.min(1, max > 0 ? value / max : 0));
  const left = centred ? (frac < 0 ? 50 + frac * 50 : 50) : 0;
  const width = centred ? Math.abs(frac) * 50 : Math.max(0, frac) * 100;
  const colour = opts.colour || "var(--blue)";
  const marks = (opts.marks || []).map((m) => `<div class="mark" style="left:${Math.max(0, Math.min(100, m * 100))}%"></div>`).join("");
  return `<div class="bar"><div>${label}</div><div class="track">${centred ? '<div class="zero"></div>' : ""}` +
    `<div class="fill" style="left:${left}%;width:${width}%;background:${colour}"></div>${marks}</div><div class="val">${text}</div></div>`;
}
function lamp(label, on, colour) { return `<span class="lamp ${on ? "on " + colour : ""}">${label}</span>`; }

function wheelName(w, wheels) {
  if (wheels.length === 2) return (w.y < 0 ? "Left" : "Right") + " track";
  const xs = [...new Set(wheels.map((q) => q.x))].sort((a, b) => b - a);
  return "Axle " + (xs.indexOf(w.x) + 1) + (w.y < 0 ? " L" : " R");
}
function slipColour(s) { const a = Math.abs(s); return a < 0.15 ? css("--green") : a < 0.5 ? css("--amber") : css("--red"); }

function fit(canvas) {
  const r = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, r.width, r.height);
  return [ctx, r.width, r.height];
}

function drawPlan(d) {
  const [ctx, W, H] = fit($("plan"));
  const ws = d.wheels; if (!ws.length) return;
  const xs = ws.map((w) => w.x), ys = ws.map((w) => w.y);
  const tl = d.tyre.length, tw = d.tyre.width;
  const x0 = Math.min(...xs) - tl, x1 = Math.max(...xs) + tl, y0 = Math.min(...ys) - tw, y1 = Math.max(...ys) + tw;
  const s = Math.min((W - 40) / (x1 - x0), (H - 30) / (y1 - y0));
  const cx = W / 2, cy = H / 2, mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
  const px = (x) => cx + (x - mx) * s, py = (y) => cy + (y - my) * s;
  ctx.strokeStyle = css("--line"); ctx.lineWidth = 2;  // the body, between the wheels or tracks
  const bx0 = px(Math.min(...xs) - tl / 2), bx1 = px(Math.max(...xs) + tl / 2), by0 = py(Math.min(...ys) + tw * 0.6), by1 = py(Math.max(...ys) - tw * 0.6);
  if (by1 > by0) ctx.strokeRect(bx0, by0, bx1 - bx0, by1 - by0);
  ctx.fillStyle = css("--dim"); ctx.font = "11px system-ui"; ctx.textAlign = "right"; ctx.fillText("front →", W - 6, 14);
  for (const w of ws) {
    const L = tl * s, T = tw * s, X = px(w.x) - L / 2, Y = py(w.y) - T / 2;
    ctx.fillStyle = css("--line"); ctx.fillRect(X, Y, L, T);
    const use = w.max > 0 ? Math.min(1, Math.abs(w.traction) / w.max) : 0;
    ctx.fillStyle = slipColour(w.slip); ctx.fillRect(X, Y + T * (1 - use), L, T * use);
    if (w.brake > 0.01) { ctx.strokeStyle = css("--red"); ctx.lineWidth = 2; ctx.strokeRect(X - 2, Y - 2, L + 4, T + 4); }
    ctx.fillStyle = css("--text"); ctx.textAlign = "center"; ctx.font = "11px system-ui";
    ctx.fillText(`${kN(w.load)} kN`, px(w.x), w.y < 0 ? Y - 5 : Y + T + 13);
  }
}

function drawProfile(d) {
  const [ctx, W, H] = fit($("profile"));
  const g = d.ground; if (!g || !g.top.length) return;
  const n = g.top.length, xEnd = g.x0 + g.dx * (n - 1), r = d.wheel_radius;
  let lo = Math.min(...g.hard), hi = Math.max(...g.top) + 2 * r + 0.3;
  if (d.blade && d.blade.down) hi = Math.max(hi, d.blade.edge + 0.5);
  lo -= 0.3;
  const pad = 34, sx = (x) => pad + (x - g.x0) / (xEnd - g.x0) * (W - pad - 8), sy = (z) => H - 18 - (z - lo) / (hi - lo) * (H - 28);
  ctx.fillStyle = css("--sky"); ctx.fillRect(pad, 0, W - pad, H - 18);
  const band = (fromArr, toArr, colour) => {
    ctx.fillStyle = colour; ctx.beginPath();
    for (let i = 0; i < n; i++) ctx.lineTo(sx(g.x0 + i * g.dx), sy(toArr[i]));
    for (let i = n - 1; i >= 0; i--) ctx.lineTo(sx(g.x0 + i * g.dx), fromArr ? sy(fromArr[i]) : H - 18);
    ctx.closePath(); ctx.fill();
  };
  band(null, g.hard, css("--firm"));
  band(g.hard, g.mud, css("--mud"));
  band(g.mud, g.base, css("--gravel"));
  ctx.fillStyle = css("--asphalt");
  for (let i = 0; i < n; i++) if (g.paved[i]) ctx.fillRect(sx(g.x0 + i * g.dx) - 1, sy(g.base[i]) - 3, (W - pad) / n + 2, 4);
  band(g.base, g.top, css("--gravel"));
  // axis
  ctx.fillStyle = css("--dim"); ctx.font = "11px system-ui"; ctx.textAlign = "right";
  for (let z = Math.ceil(lo * 2) / 2; z <= hi; z += 0.5) { ctx.fillText(z.toFixed(1), pad - 4, sy(z) + 4); ctx.fillRect(pad, sy(z), 4, 1); }
  ctx.textAlign = "center";
  for (let x = Math.ceil(g.x0 / 5) * 5; x <= xEnd; x += 5) ctx.fillText(x + " m", sx(x), H - 4);
  // wheels on the left side, sitting on the ground they have sunk into
  ctx.strokeStyle = css("--text"); ctx.lineWidth = 2;
  const seen = new Set();
  for (const w of d.wheels) {
    if (w.y > 0 || seen.has(w.x)) continue; seen.add(w.x);
    const wx = d.x + w.x, i = Math.round((wx - g.x0) / g.dx);
    if (i < 0 || i >= n) continue;
    const zc = g.top[i] + r, rx = r / (xEnd - g.x0) * (W - pad - 8), ry = r / (hi - lo) * (H - 28);
    ctx.fillStyle = slipColour(w.slip); ctx.globalAlpha = 0.35;
    ctx.beginPath();
    if (d.tyre.length > 2 * r) {  // a track: a long loop round the sprocket and idler
      const half = d.tyre.length / 2 / (xEnd - g.x0) * (W - pad - 8);
      ctx.roundRect(sx(wx) - half, sy(zc) - ry, 2 * half, 2 * ry, ry);
    } else {
      ctx.ellipse(sx(wx), sy(zc), rx, ry, 0, 0, 2 * Math.PI);
    }
    ctx.fill(); ctx.globalAlpha = 1; ctx.stroke();
  }
  if (d.blade && d.blade.down) {
    ctx.strokeStyle = css("--amber"); ctx.lineWidth = 3;
    ctx.beginPath(); ctx.moveTo(sx(d.blade.x), sy(d.blade.edge)); ctx.lineTo(sx(d.blade.x), sy(d.blade.edge + 1.1)); ctx.stroke();
    ctx.setLineDash([4, 4]); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(sx(d.blade.x - 4), sy(d.blade.edge)); ctx.lineTo(sx(d.blade.x + 4), sy(d.blade.edge)); ctx.stroke();
    ctx.setLineDash([]);
  }
  ctx.fillStyle = css("--text"); ctx.textAlign = "left";
  ctx.fillText("▲ " + d.x.toFixed(1) + " m", sx(d.x) - 4, 14);
}

function chart(id, title, unit, series, opts = {}) {
  const [ctx, W, H] = fit($(id));
  const pad = { l: 40, r: 8, t: 18, b: 16 };
  ctx.fillStyle = css("--dim"); ctx.font = "11px system-ui"; ctx.textAlign = "left";
  ctx.fillText(title + (unit ? " (" + unit + ")" : ""), pad.l, 11);
  if (hist.length < 2) return;
  const t1 = hist[hist.length - 1].t, t0 = t1 - WINDOW;
  let lo = opts.min ?? Infinity, hi = opts.max ?? -Infinity;
  for (const p of hist) for (const s of series) { const v = s.get(p); if (v < lo) lo = v; if (v > hi) hi = v; }
  for (const ref of opts.refs || []) { lo = Math.min(lo, ref.v); hi = Math.max(hi, ref.v); }
  if (!(hi > lo)) { hi = lo + 1; }
  const span = hi - lo; hi += span * 0.08; if (opts.min === undefined) lo -= span * 0.08;
  const sx = (t) => pad.l + (t - t0) / WINDOW * (W - pad.l - pad.r), sy = (v) => pad.t + (1 - (v - lo) / (hi - lo)) * (H - pad.t - pad.b);
  ctx.strokeStyle = css("--line"); ctx.lineWidth = 1; ctx.textAlign = "right";
  for (let k = 0; k <= 3; k++) {
    const v = lo + (hi - lo) * k / 3, y = sy(v);
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(W - pad.r, y); ctx.stroke();
    ctx.fillText(Math.abs(v) >= 100 ? v.toFixed(0) : v.toFixed(1), pad.l - 4, y + 4);
  }
  for (const ref of opts.refs || []) {
    ctx.strokeStyle = css(ref.colour); ctx.setLineDash([4, 4]);
    ctx.beginPath(); ctx.moveTo(pad.l, sy(ref.v)); ctx.lineTo(W - pad.r, sy(ref.v)); ctx.stroke(); ctx.setLineDash([]);
  }
  let lx = W - pad.r;
  ctx.textAlign = "right";
  for (const s of [...series].reverse()) {
    ctx.strokeStyle = css(s.colour); ctx.lineWidth = 1.6; ctx.beginPath();
    hist.forEach((p, i) => { const x = sx(p.t), y = sy(s.get(p)); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
    ctx.stroke();
    const latest = s.get(hist[hist.length - 1]);
    const label = `${s.label} ${Math.abs(latest) >= 100 ? latest.toFixed(0) : latest.toFixed(1)}`;
    ctx.fillStyle = css(s.colour); ctx.fillText(label, lx, 11); lx -= ctx.measureText(label).width + 12;
  }
}

function render(d) {
  $("name").textContent = d.vehicle;
  $("mode").textContent = d.mode + (d.shifting ? " · shifting" : "");
  $("gear").textContent = d.gear < 0 ? (d.reverse_gears > 1 ? "R" + -d.gear : "R") : d.gear === 0 ? "N" : String(d.gear);
  $("speed").textContent = (d.speed * 3.6).toFixed(1);
  $("rpm").textContent = Math.round(d.rpm);

  const capacity = Math.max(d.clutch_capacity, 1);
  $("driveline").innerHTML =
    bar("Engine", d.rpm, d.redline * 1.05, `${Math.round(d.rpm)} rpm`, { colour: d.rpm > d.redline ? "var(--red)" : "var(--amber)", marks: [d.idle / (d.redline * 1.05), 1 / 1.05] }) +
    bar("Engine torque", d.engine_torque, d.max_engine_torque, `${Math.round(d.engine_torque)} N·m`, { centred: true }) +
    bar("Clutch torque", d.clutch_torque, capacity, `${Math.round(d.clutch_torque)} N·m`, { centred: true, colour: d.clutch_locked ? "var(--green)" : "var(--amber)" }) +
    bar("Clutch slip", d.clutch_slip_rpm, 2500, `${Math.round(d.clutch_slip_rpm)} rpm`, { centred: true, colour: "var(--amber)" }) +
    bar("Throttle", d.pedals.throttle, 1, `${Math.round(d.pedals.throttle * 100)} %`, { colour: "var(--green)" }) +
    bar("Brake", d.pedals.brake, 1, `${Math.round(d.pedals.brake * 100)} %`, { colour: "var(--red)" }) +
    bar("Clutch pedal", d.pedals.clutch, 1, `${Math.round(d.pedals.clutch * 100)} %`, { colour: "var(--amber)" }) +
    bar("Fuel", d.fuel_rate_lph, 80, `${d.fuel_rate_lph.toFixed(1)} L/h`, { colour: "var(--violet)" });
  $("lamps").innerHTML =
    lamp("AWD", d.awd, "g") + lamp("DIFF LOCK", d.lock, "a") + lamp("LOW", d.low, "a") + lamp("ENGINE BRAKE", d.engine_brake, "g") +
    lamp("HANDBRAKE", d.handbrake, "r") + lamp("CLUTCH LOCKED", d.clutch_locked, "g") + lamp("GRIND", d.grinding, "r") + lamp("STALLED", !d.running, "r");

  const f = d.forces, weight = d.mass * 9.81, scale = Math.max(weight * 0.6, Math.abs(f.traction), f.resistance + f.belly + f.external);
  const net = f.traction + f.grade - Math.sign(d.speed || f.traction) * (f.resistance + f.belly + f.external);
  $("forces").innerHTML =
    bar("Traction", f.traction, scale, `${kN(f.traction)} kN`, { centred: true, colour: "var(--green)" }) +
    bar("Rolling + compaction", -f.resistance, scale, `${kN(f.resistance)} kN`, { centred: true, colour: "var(--amber)" }) +
    bar("Slope", f.grade, scale, `${kN(f.grade)} kN`, { centred: true, colour: "var(--violet)" }) +
    bar("Belly drag", -f.belly, scale, `${kN(f.belly)} kN`, { centred: true, colour: "var(--red)" }) +
    bar("Blade / towed", -f.external, scale, `${kN(f.external)} kN`, { centred: true, colour: "var(--red)" }) +
    bar("Net (moving)", net, scale, `${kN(net)} kN`, { centred: true, colour: "var(--blue)" }) +
    bar("Sinkage", d.sinkage, Math.max(d.clearance * 1.4, 0.01), `${(d.sinkage * 100).toFixed(0)} / ${(d.clearance * 100).toFixed(0)} cm`,
        { colour: d.sinkage > d.clearance ? "var(--red)" : "var(--amber)", marks: [1 / 1.4] });

  $("blade-card").classList.toggle("hidden", !d.blade);
  if (d.blade) {
    $("blade").innerHTML =
      bar("Blade", d.blade.down ? 1 : 0, 1, d.blade.down ? "down" : "up", { colour: "var(--amber)" }) +
      (d.blade.grade
        ? bar("Grade: layer set", d.blade.layer, 0.5, `${(d.blade.layer * 100).toFixed(0)} cm`, { centred: true, colour: "var(--gravel)" })
        : bar("Manual: edge set", d.blade.height, 1.0, `${(d.blade.height * 100).toFixed(0)} cm`, { centred: true, colour: "var(--gravel)" })) +
      bar("Pushing", d.blade.pile, 10, `${d.blade.pile.toFixed(2)} m³`, { colour: "var(--gravel)" }) +
      bar("Push force", d.blade.force, 100000, `${kN(d.blade.force)} kN`, { colour: "var(--red)" }) +
      `<p class="note">Edge at ${d.blade.edge.toFixed(2)} m: ${(d.blade.from_ground * 100).toFixed(0)} cm from the undisturbed ground, ` +
      `${(d.blade.from_tracks * 100).toFixed(0)} cm from the track line. ` +
      (d.blade.grade ? "Grade control holds the layer above the ground (C: manual)."
                     : "Manual: the edge is set against the tracks and pitches with the machine (C: grade control).") + `</p>`;
  }

  const rows = d.wheels.map((w) => {
    const use = w.max > 0 ? Math.abs(w.traction) / w.max : 0;
    const surface = w.soft > 0.5 ? (w.gravel > 0.01 ? "mud + gravel" : "mud") : w.gravel > 0.01 ? "gravel" : "hard";
    return `<tr><td>${wheelName(w, d.wheels)}</td><td>${kN(w.load)}</td><td>${(w.sink * 100).toFixed(1)}</td><td>${(w.rut * 100).toFixed(1)}</td>` +
      `<td>${(w.gravel * 100).toFixed(0)}</td><td>${surface}</td><td style="color:${slipColour(w.slip)}">${(w.slip * 100).toFixed(0)}</td>` +
      `<td>${kN(w.traction)}</td><td>${kN(w.max)}</td><td>${(use * 100).toFixed(0)}</td><td>${kN(w.resist)}</td>` +
      `<td>${(w.torque / 1000).toFixed(2)}</td><td>${(w.brake * 100).toFixed(0)}</td><td>${(w.omega * d.wheel_radius * 3.6).toFixed(1)}</td></tr>`;
  }).join("");
  $("wheels").innerHTML = "<tr><th>Wheel</th><th>Load kN</th><th>Sink cm</th><th>Rut cm</th><th>Gravel cm</th><th>Surface</th>" +
    "<th>Slip %</th><th>Traction kN</th><th>Grip kN</th><th>Grip used %</th><th>Resist kN</th><th>Drive kN·m</th><th>Brake %</th><th>Rim km/h</th></tr>" + rows;

  drawPlan(d);
  drawProfile(d);
  const blade = (p) => p.blade;
  chart("c-speed", "Speed", "km/h", [{ label: "speed", colour: "--blue", get: (p) => p.speed }], { min: 0 });
  chart("c-rpm", "Engine", "rpm", [{ label: "rpm", colour: "--amber", get: (p) => p.rpm }], { min: 0, refs: [{ v: d.redline, colour: "--red" }] });
  chart("c-force", "Forces", "kN", [
    { label: "traction", colour: "--green", get: (p) => p.traction },
    { label: "resistance", colour: "--amber", get: (p) => p.resist },
    { label: "blade", colour: "--red", get: blade }]);
  chart("c-sink", "Sinkage", "cm", [{ label: "sink", colour: "--violet", get: (p) => p.sink }], { min: 0, refs: [{ v: d.clearance * 100, colour: "--red" }] });
  chart("c-slip", "Wheel slip", "%", [
    { label: "mean", colour: "--blue", get: (p) => p.slip_mean },
    { label: "worst", colour: "--red", get: (p) => p.slip_max }]);
  chart("c-fuel", "Fuel", "L/h", [{ label: "fuel", colour: "--violet", get: (p) => p.fuel }], { min: 0 });
}

function record(d) {
  if (hist.length && (d.t < hist[hist.length - 1].t || d.vehicle !== last?.vehicle)) hist = [];
  const slips = d.wheels.map((w) => Math.abs(w.slip) * 100);
  hist.push({ t: d.t, speed: d.speed * 3.6, rpm: d.rpm, traction: d.forces.traction / 1000,
    resist: (d.forces.resistance + d.forces.belly) / 1000, blade: d.forces.external / 1000, sink: d.sinkage * 100,
    slip_mean: slips.reduce((a, b) => a + b, 0) / Math.max(1, slips.length), slip_max: Math.max(0, ...slips), fuel: d.fuel_rate_lph });
  while (hist.length && hist[0].t < d.t - WINDOW) hist.shift();
}

function setStatus(kind, text) { $("dot").className = "dot " + kind; $("status").textContent = text; }

const source = new EventSource("/stream");
source.onopen = () => setStatus("live", "live");
source.onerror = () => setStatus("", "game not running, retrying");
source.onmessage = (e) => {
  let d; try { d = JSON.parse(e.data); } catch { return; }
  if (!d.wheels) return;
  record(d); last = d; lastAt = performance.now();
  render(d);
  setStatus("live", "live");
};
setInterval(() => { if (last && performance.now() - lastAt > 1500 && source.readyState === 1) setStatus("paused", "no updates (game paused?)"); }, 500);
window.addEventListener("resize", () => { if (last) render(last); });
</script>
</body>
</html>
)PAGE";

}  // namespace telemetry
