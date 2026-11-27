#pragma once

// The 3D view of the ground served at GET /3d by telemetry.h: plain WebGL2, no libraries, so it works offline.
// It loads /terrain.bin (written by terrain_blob() in play.cpp) when the ground changes, and follows the vehicles
// on the telemetry stream.
namespace telemetry {

inline constexpr const char* kView3dPage = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Incatramato 3D View</title>
<style>
:root { --panel: rgba(24,26,30,.86); --text: #e8e6e1; --dim: #9a9ea4; --line: #3a3e45; --accent: #e8a93a; }
@media (prefers-color-scheme: light) {
  :root:not([data-theme="dark"]) { --panel: rgba(255,255,255,.9); --text: #1d1f22; --dim: #5d6166; --line: #d4d2cd; --accent: #a86a08; }
}
* { box-sizing: border-box; }
html, body { margin: 0; height: 100%; overflow: hidden; background: #1b2a38; color: var(--text); font: 13px/1.35 system-ui, sans-serif; }
canvas { display: block; width: 100vw; height: 100vh; touch-action: none; }
.panel { position: fixed; background: var(--panel); border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px; }
#controls { top: 12px; left: 12px; width: 260px; max-width: calc(100vw - 24px); }
#readout { bottom: 12px; left: 12px; min-width: 230px; font-variant-numeric: tabular-nums; }
#legend { top: 12px; right: 12px; }
h1 { font-size: 13px; margin: 0 0 8px; font-weight: 600; }
label { display: block; margin: 4px 0; cursor: pointer; }
.row { display: flex; align-items: center; gap: 8px; margin-top: 8px; }
input[type=range] { flex: 1; }
button { font: inherit; color: var(--text); background: transparent; border: 1px solid var(--line); border-radius: 5px; padding: 3px 8px; cursor: pointer; }
button.on { border-color: var(--accent); color: var(--accent); }
.dim { color: var(--dim); }
.sw { display: inline-block; width: 12px; height: 12px; border-radius: 2px; vertical-align: -2px; margin-right: 6px; }
table { border-collapse: collapse; }
td { padding: 1px 6px 1px 0; }
td:last-child { text-align: right; }
a { color: var(--accent); }
#msg { position: fixed; inset: 0; display: none; align-items: center; justify-content: center; text-align: center; padding: 16px; }
@media (max-width: 520px) { #legend { display: none; } }
</style>
</head>
<body>
<canvas id="c"></canvas>
<div class="panel" id="controls">
  <h1>Ground in 3D <span class="dim" id="status">connecting</span></h1>
  <label><input type="radio" name="mode" value="surface" checked> Surface as it is</label>
  <label><input type="radio" name="mode" value="mud"> Without gravel: mud, ruts, firm ground</label>
  <label><input type="radio" name="mode" value="gravel"> Gravel thickness</label>
  <div class="row"><span>Height ×<span id="exv">2</span></span><input type="range" id="ex" min="1" max="6" step="0.5" value="2"></div>
  <div class="row"><button id="follow" class="on">Follow vehicle</button><a href="/">Telemetry</a></div>
  <p class="dim" style="margin:8px 0 0">Drag to orbit, right-drag or Shift-drag to pan, wheel to zoom. The readout is for the cell under the cross.</p>
</div>
<div class="panel" id="legend"></div>
<div class="panel" id="readout"></div>
<div id="msg"></div>
<script>
"use strict";
const $ = (id) => document.getElementById(id);
const canvas = $("c");
const gl = canvas.getContext("webgl2", { antialias: true });
if (!gl) { $("msg").style.display = "flex"; $("msg").textContent = "This browser has no WebGL2, which the 3D view needs."; throw new Error("no webgl2"); }

// ---------------------------------------------------------------- small matrix helpers (column-major)
const M = {
  mul(a, b) { const o = new Float32Array(16); for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++) { let s = 0; for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k]; o[c * 4 + r] = s; } return o; },
  persp(fov, asp, n, f) { const t = 1 / Math.tan(fov / 2), o = new Float32Array(16); o[0] = t / asp; o[5] = t; o[10] = (f + n) / (n - f); o[11] = -1; o[14] = 2 * f * n / (n - f); return o; },
  look(e, t, u) {
    let z = [e[0] - t[0], e[1] - t[1], e[2] - t[2]]; let l = Math.hypot(...z); z = z.map((v) => v / l);
    let x = [u[1] * z[2] - u[2] * z[1], u[2] * z[0] - u[0] * z[2], u[0] * z[1] - u[1] * z[0]]; l = Math.hypot(...x); x = x.map((v) => v / l);
    const y = [z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]];
    return new Float32Array([x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
      -(x[0] * e[0] + x[1] * e[1] + x[2] * e[2]), -(y[0] * e[0] + y[1] * e[1] + y[2] * e[2]), -(z[0] * e[0] + z[1] * e[1] + z[2] * e[2]), 1]);
  },
  box(cx, cy, cz, sx, sy, sz) { return new Float32Array([sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, sz, 0, cx, cy, cz, 1]); },
};

function program(vs, fs) {
  const p = gl.createProgram();
  for (const [type, src] of [[gl.VERTEX_SHADER, vs], [gl.FRAGMENT_SHADER, fs]]) {
    const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
    gl.attachShader(p, s);
  }
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  return p;
}

// Ground: heights unscaled in the buffer, exaggerated in the shader so the slider costs nothing.
const groundProg = program(`#version 300 es
in vec3 aPos; in vec2 aGrad; in vec3 aCol;
uniform mat4 uVP; uniform float uEx;
out vec3 vN; out vec3 vC; out vec2 vXY;
void main() { vN = normalize(vec3(-uEx * aGrad, 1.0)); vC = aCol; vXY = aPos.xy; gl_Position = uVP * vec4(aPos.xy, aPos.z * uEx, 1.0); }`,
`#version 300 es
precision highp float;
in vec3 vN; in vec3 vC; in vec2 vXY; uniform vec3 uL; out vec4 o;
float grid(vec2 p, float s) { vec2 q = p / s; vec2 g = abs(fract(q - 0.5) - 0.5) / fwidth(q); return 1.0 - min(min(g.x, g.y), 1.0); }
void main() {
  float lit = 0.42 + 0.58 * max(dot(normalize(vN), uL), 0.0);
  vec3 c = vC * lit * (1.0 - 0.10 * grid(vXY, 1.0) - 0.28 * grid(vXY, 10.0));
  o = vec4(c, 1.0);
}`);
const boxProg = program(`#version 300 es
in vec3 aPos; in vec3 aN; uniform mat4 uVP; uniform mat4 uM; out vec3 vN;
void main() { vN = aN; gl_Position = uVP * uM * vec4(aPos, 1.0); }`,
`#version 300 es
precision highp float;
in vec3 vN; uniform vec3 uC; uniform vec3 uL; out vec4 o;
void main() { o = vec4(uC * (0.45 + 0.55 * max(dot(normalize(vN), uL), 0.0)), 1.0); }`);

// A unit cube centred on the origin, with face normals.
const cube = (() => {
  const v = [];
  const faces = [[[1, 0, 0], [0, 1, 0], [0, 0, 1]], [[-1, 0, 0], [0, -1, 0], [0, 0, 1]], [[0, 1, 0], [-1, 0, 0], [0, 0, 1]],
                 [[0, -1, 0], [1, 0, 0], [0, 0, 1]], [[0, 0, 1], [1, 0, 0], [0, 1, 0]], [[0, 0, -1], [-1, 0, 0], [0, 1, 0]]];
  for (const [n, a, b] of faces) {
    const c = (s, t) => [0, 1, 2].map((i) => n[i] * 0.5 + a[i] * s * 0.5 + b[i] * t * 0.5);
    for (const [s, t] of [[-1, -1], [1, -1], [1, 1], [-1, -1], [1, 1], [-1, 1]]) v.push(...c(s, t), ...n);
  }
  const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
  const buf = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, buf); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(v), gl.STATIC_DRAW);
  const pos = gl.getAttribLocation(boxProg, "aPos"), nor = gl.getAttribLocation(boxProg, "aN");
  gl.enableVertexAttribArray(pos); gl.vertexAttribPointer(pos, 3, gl.FLOAT, false, 24, 0);
  gl.enableVertexAttribArray(nor); gl.vertexAttribPointer(nor, 3, gl.FLOAT, false, 24, 12);
  return { vao, count: 36 };
})();

// ---------------------------------------------------------------- ground data
let ground = null;   // { nx, ny, cs, top, gravel, loose, rut, soft, paving } in metres (Float32Array) and paving (Uint8Array)
let mesh = null;     // { vao, pos, grad, col, count }
let mode = "surface", exaggeration = 2;
const COL = { firm: [139, 115, 85], mud: [70, 48, 30], gravel: [150, 150, 140], laid: [92, 92, 98], rolled: [38, 38, 42] };

function parse(buf) {
  const dv = new DataView(buf);
  if (String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3)) !== "INCT") throw new Error("not a terrain snapshot");
  const version = dv.getUint32(4, true), nx = dv.getUint16(8, true), ny = dv.getUint16(10, true), cs = dv.getFloat32(12, true);
  const n = nx * ny, mm = (arr) => Float32Array.from(arr, (v) => v / 1000);
  return { version, nx, ny, cs,
    top: mm(new Int16Array(buf, 16, n)), gravel: mm(new Uint16Array(buf, 16 + 2 * n, n)), loose: mm(new Uint16Array(buf, 16 + 4 * n, n)),
    rut: mm(new Uint16Array(buf, 16 + 6 * n, n)), soft: mm(new Uint16Array(buf, 16 + 8 * n, n)), paving: new Uint8Array(buf, 16 + 10 * n, n) };
}

const mix = (a, b, t) => [0, 1, 2].map((i) => a[i] + (b[i] - a[i]) * t);
function colour(i) {
  const g = ground, clamp = (v) => Math.max(0, Math.min(1, v));
  const base = g.soft[i] > 0 ? COL.mud.map((v) => v * (1.3 - 0.9 * g.rut[i] / Math.max(g.soft[i], 0.01))) : COL.firm;
  if (mode === "mud") return g.paving[i] ? mix(base, [60, 60, 70], 0.35) : base;
  if (mode === "gravel") {
    const t = g.gravel[i] + g.loose[i];
    if (t < 0.005) return mix(base, [40, 40, 44], 0.5);
    if (t < 0.15) return mix([40, 90, 200], [80, 160, 240], t / 0.15);     // too thin to pave on
    return mix([60, 190, 90], [235, 120, 40], clamp((t - 0.15) / 0.6));    // enough base; orange for heaps
  }
  if (g.paving[i]) return mix(g.paving[i] === 2 ? COL.rolled : COL.laid, COL.gravel, clamp(g.loose[i] / 0.15));
  return mix(base, COL.gravel, clamp(g.gravel[i] / 0.15));
}
const height = (i) => mode === "mud" ? ground.top[i] - ground.gravel[i] - ground.loose[i] : ground.top[i];

function buildMesh() {
  const { nx, ny } = ground, n = nx * ny;
  const vao = gl.createVertexArray(); gl.bindVertexArray(vao);
  const mk = (name, size) => {
    const b = gl.createBuffer(), loc = gl.getAttribLocation(groundProg, name);
    gl.bindBuffer(gl.ARRAY_BUFFER, b); gl.bufferData(gl.ARRAY_BUFFER, n * size * 4, gl.DYNAMIC_DRAW);
    gl.enableVertexAttribArray(loc); gl.vertexAttribPointer(loc, size, gl.FLOAT, false, 0, 0);
    return b;
  };
  const m = { vao, pos: mk("aPos", 3), grad: mk("aGrad", 2), col: mk("aCol", 3), count: (nx - 1) * (ny - 1) * 6, nx, ny };
  const idx = new Uint32Array(m.count);
  let k = 0;
  for (let y = 0; y + 1 < ny; y++) for (let x = 0; x + 1 < nx; x++) {
    const a = y * nx + x, b = a + 1, c = a + nx, d = c + 1;
    idx.set([a, b, d, a, d, c], k); k += 6;
  }
  const ib = gl.createBuffer(); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, ib); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);
  return m;
}

function fillMesh() {
  const { nx, ny, cs } = ground, n = nx * ny;
  if (!mesh || mesh.nx !== nx || mesh.ny !== ny) mesh = buildMesh();
  const pos = new Float32Array(n * 3), grad = new Float32Array(n * 2), col = new Float32Array(n * 3);
  const h = new Float32Array(n);
  for (let i = 0; i < n; i++) h[i] = height(i);
  for (let y = 0; y < ny; y++) for (let x = 0; x < nx; x++) {
    const i = y * nx + x;
    pos.set([(x + 0.5) * cs, (y + 0.5) * cs, h[i]], i * 3);
    const xl = Math.max(x - 1, 0), xr = Math.min(x + 1, nx - 1), yd = Math.max(y - 1, 0), yu = Math.min(y + 1, ny - 1);
    grad[i * 2] = (h[y * nx + xr] - h[y * nx + xl]) / ((xr - xl) * cs || 1);
    grad[i * 2 + 1] = (h[yu * nx + x] - h[yd * nx + x]) / ((yu - yd) * cs || 1);
    const c = colour(i); col.set([c[0] / 255, c[1] / 255, c[2] / 255], i * 3);
  }
  gl.bindVertexArray(mesh.vao);
  for (const [b, data] of [[mesh.pos, pos], [mesh.grad, grad], [mesh.col, col]]) { gl.bindBuffer(gl.ARRAY_BUFFER, b); gl.bufferSubData(gl.ARRAY_BUFFER, 0, data); }
}

function cellAt(wx, wy) {
  if (!ground) return -1;
  const x = Math.floor(wx / ground.cs), y = Math.floor(wy / ground.cs);
  if (x < 0 || y < 0 || x >= ground.nx || y >= ground.ny) return -1;
  return y * ground.nx + x;
}

let loaded = -1, loading = false, lastFetch = 0;
async function loadGround(version) {
  if (loading || version === loaded || performance.now() - lastFetch < 250) return;
  loading = true; lastFetch = performance.now();
  try {
    const r = await fetch("/terrain.bin?v=" + version, { cache: "no-store" });
    if (r.ok) { ground = parse(await r.arrayBuffer()); loaded = ground.version; fillMesh(); }
  } catch (e) { /* the game may have closed; the stream reports it */ }
  loading = false;
}

// ---------------------------------------------------------------- camera
const cam = { tx: 20, ty: 6, yaw: -2.2, pitch: 0.55, dist: 28, follow: true };
let state = null;
function eye() {
  const tz = targetZ();
  return [cam.tx + cam.dist * Math.cos(cam.pitch) * Math.cos(cam.yaw), cam.ty + cam.dist * Math.cos(cam.pitch) * Math.sin(cam.yaw), tz + cam.dist * Math.sin(cam.pitch)];
}
function targetZ() { const i = cellAt(cam.tx, cam.ty); return i < 0 ? 0 : height(i) * exaggeration; }

let drag = null;
canvas.addEventListener("contextmenu", (e) => e.preventDefault());
canvas.addEventListener("pointerdown", (e) => { drag = { x: e.clientX, y: e.clientY, pan: e.button === 2 || e.shiftKey }; canvas.setPointerCapture(e.pointerId); });
canvas.addEventListener("pointerup", () => { drag = null; });
canvas.addEventListener("pointermove", (e) => {
  if (!drag) return;
  const dx = e.clientX - drag.x, dy = e.clientY - drag.y; drag.x = e.clientX; drag.y = e.clientY;
  if (drag.pan) {
    const s = cam.dist * 0.0018, c = Math.cos(cam.yaw), sn = Math.sin(cam.yaw);
    cam.tx += (dx * sn - dy * c) * s; cam.ty += (-dx * c - dy * sn) * s;  // the ground follows the cursor
    setFollow(false);
  } else {
    cam.yaw -= dx * 0.006; cam.pitch = Math.max(0.05, Math.min(1.5, cam.pitch + dy * 0.006));
  }
});
canvas.addEventListener("wheel", (e) => { e.preventDefault(); cam.dist = Math.max(3, Math.min(250, cam.dist * Math.exp(e.deltaY * 0.0012))); }, { passive: false });
function setFollow(on) { cam.follow = on; $("follow").classList.toggle("on", on); }
$("follow").addEventListener("click", () => setFollow(!cam.follow));
for (const r of document.querySelectorAll("input[name=mode]")) r.addEventListener("change", () => { mode = r.value; legend(); if (ground) fillMesh(); });
$("ex").addEventListener("input", () => { exaggeration = parseFloat($("ex").value); $("exv").textContent = exaggeration; });

function legend() {
  const sw = (c, t) => `<div><span class="sw" style="background:rgb(${c.map(Math.round).join(",")})"></span>${t}</div>`;
  $("legend").innerHTML = mode === "gravel"
    ? sw([60, 130, 220], "Gravel under 15 cm: too thin to pave") + sw([60, 190, 90], "15 cm or more: ready to pave") + sw([235, 120, 40], "Heaps (75 cm and up)")
    : mode === "mud"
    ? sw(COL.firm, "Firm ground") + sw(COL.mud.map((v) => v * 1.3), "Mud, untouched") + sw(COL.mud.map((v) => v * 0.4), "Mud, rutted to the bottom") + sw(mix(COL.mud, [60, 60, 70], 0.35), "Under the road")
    : sw(COL.firm, "Firm ground") + sw(COL.mud, "Mud (darker: deeper ruts)") + sw(COL.gravel, "Gravel") + sw(COL.laid, "Asphalt, laid") + sw(COL.rolled, "Asphalt, rolled");
}
legend();

// ---------------------------------------------------------------- live data
const source = new EventSource("/stream");
source.onopen = () => ($("status").textContent = "live");
source.onerror = () => ($("status").textContent = "game not running, retrying");
source.onmessage = (e) => {
  let d; try { d = JSON.parse(e.data); } catch { return; }
  state = d; $("status").textContent = "live";
  if (d.terrain_version !== undefined) loadGround(d.terrain_version);
};

function readout() {
  const i = cellAt(cam.tx, cam.ty);
  if (i < 0) { $("readout").innerHTML = '<span class="dim">Cross outside the ground</span>'; return; }
  const g = ground, cm = (v) => (v * 100).toFixed(1) + " cm", m = (v) => v.toFixed(3) + " m";
  const paving = ["none", "laid", "rolled"][g.paving[i]];
  $("readout").innerHTML = `<table>
    <tr><td class="dim">Cell</td><td>x ${((i % g.nx) + 0.5) * g.cs} m, y ${(Math.floor(i / g.nx) + 0.5) * g.cs} m</td></tr>
    <tr><td class="dim">Surface</td><td>${m(g.top[i])}</td></tr>
    <tr><td class="dim">Gravel</td><td>${cm(g.gravel[i])}${g.paving[i] ? " (road base)" : ""}</td></tr>
    <tr><td class="dim">Loose on road</td><td>${cm(g.loose[i])}</td></tr>
    <tr><td class="dim">Mud</td><td>${cm(g.soft[i])}, rut ${cm(g.rut[i])}</td></tr>
    <tr><td class="dim">Asphalt</td><td>${paving}</td></tr></table>`;
}

// ---------------------------------------------------------------- drawing
const L = (() => { const v = [0.35, -0.45, 0.82], l = Math.hypot(...v); return new Float32Array(v.map((x) => x / l)); })();
function hex(s) { const n = parseInt(s.slice(1), 16); return [(n >> 16 & 255) / 255, (n >> 8 & 255) / 255, (n & 255) / 255]; }
function groundZ(wx, wy) { const i = cellAt(wx, wy); return i < 0 ? 0 : ground.top[i] * exaggeration; }

function frame() {
  const dpr = window.devicePixelRatio || 1, w = Math.round(canvas.clientWidth * dpr), h = Math.round(canvas.clientHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
  gl.viewport(0, 0, w, h);
  gl.clearColor(0.11, 0.16, 0.22, 1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  gl.enable(gl.DEPTH_TEST);
  if (state && cam.follow && state.vehicles) {
    const me = state.vehicles.find((v) => v.active);
    if (me) { cam.tx += (me.x - cam.tx) * 0.15; cam.ty += (me.y - cam.ty) * 0.15; }
  }
  const vp = M.mul(M.persp(0.8, w / h, 0.2, 800), M.look(eye(), [cam.tx, cam.ty, targetZ()], [0, 0, 1]));

  if (mesh && ground) {
    gl.useProgram(groundProg);
    gl.uniformMatrix4fv(gl.getUniformLocation(groundProg, "uVP"), false, vp);
    gl.uniform1f(gl.getUniformLocation(groundProg, "uEx"), exaggeration);
    gl.uniform3fv(gl.getUniformLocation(groundProg, "uL"), L);
    gl.bindVertexArray(mesh.vao);
    gl.drawElements(gl.TRIANGLES, mesh.count, gl.UNSIGNED_INT, 0);
  }

  gl.useProgram(boxProg);
  gl.uniformMatrix4fv(gl.getUniformLocation(boxProg, "uVP"), false, vp);
  gl.uniform3fv(gl.getUniformLocation(boxProg, "uL"), L);
  gl.bindVertexArray(cube.vao);
  const uM = gl.getUniformLocation(boxProg, "uM"), uC = gl.getUniformLocation(boxProg, "uC");
  const box = (cx, cy, cz, sx, sy, sz, c) => { gl.uniformMatrix4fv(uM, false, M.box(cx, cy, cz, sx, sy, sz)); gl.uniform3fv(uC, c); gl.drawArrays(gl.TRIANGLES, 0, cube.count); };
  if (state && state.vehicles && ground) {
    for (const v of state.vehicles) {
      const r = v.radius, tw = v.tyre.width, tl = v.tyre.length, track = tl > 2 * r;
      let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity, zs = 0;
      for (const wh of v.wheels) {
        const wx = v.x + wh.x, wy = v.y + wh.y, z = groundZ(wx, wy);
        box(wx, wy, z + r, track ? tl : 2 * r, tw, 2 * r, [0.12, 0.12, 0.13]);
        x0 = Math.min(x0, wx - tl / 2); x1 = Math.max(x1, wx + tl / 2); y0 = Math.min(y0, wy - tw / 2); y1 = Math.max(y1, wy + tw / 2); zs += z;
      }
      const zb = zs / v.wheels.length + 2 * r;
      box((x0 + x1) / 2, (y0 + y1) / 2, zb + 0.5, x1 - x0 + 0.4, y1 - y0, 1.0, hex(v.paint));
      if (v.active) box(x1 - 0.6, (y0 + y1) / 2, zb + 1.5, 1.2, y1 - y0 - 0.2, 1.0, hex(v.paint).map((c) => c * 0.8));
      if (v.blade) box(v.blade.x, v.y, v.blade.edge * exaggeration + 0.55, 0.15, 3.4, 1.1, v.blade.down ? [0.9, 0.66, 0.22] : [0.55, 0.56, 0.58]);
    }
  }
  if (ground) {  // the cross: where the readout reads
    const tz = targetZ();
    box(cam.tx, cam.ty, tz + 0.6, 0.06, 0.06, 1.2, [1, 1, 1]);
    box(cam.tx, cam.ty, tz + 0.02, 0.8, 0.05, 0.04, [1, 1, 1]);
    box(cam.tx, cam.ty, tz + 0.02, 0.05, 0.8, 0.04, [1, 1, 1]);
    readout();
  }
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
</script>
</body>
</html>
)PAGE";

}  // namespace telemetry
