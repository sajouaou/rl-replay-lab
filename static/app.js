import * as THREE from "three";
import { OrbitControls } from "/static/vendor/OrbitControls.js";

const $ = (id) => document.getElementById(id);
const TEAM = [0x2f7bff, 0xff8a1f];
const TEAM_CSS = ["#4d8dff", "#ff9a3c"];
const GHOST = 0xff5ccf;
// hitbox length, width, height, offset forward, offset up (game units)
const HITBOX = {
  Octane: [118.0, 84.2, 36.2, 13.9, 20.8], Dominus: [127.9, 83.3, 31.3, 9.0, 15.8], Plank: [128.8, 84.7, 29.4, 9.0, 12.1],
  Breakout: [131.5, 80.5, 30.3, 12.5, 11.8], Hybrid: [127.0, 82.2, 34.2, 13.9, 20.8], Merc: [120.7, 76.7, 41.7, 11.4, 21.5],
};
const MODES = [["ghost", "Fantôme : refait le vrai trajet"], ["removed", "Retirée"], ["nexto", "Nexto (IA) : joue à sa place"],
  ["free", "Libre : physique, sans pilote"], ["bot", "Bot simple : fonce sur le ballon"], ["control", "Je conduis"]];
const MODE_TAG = { ghost: "", control: " (toi)", bot: " (bot)", free: " (libre)", nexto: " (Nexto)" };
const BALL_R = 91.25;

/* ---------- coordinates: game (x, y, z), z up, left-handed -> three (x, z, y), y up ---------- */
const P = (x, y, z) => new THREE.Vector3(x, z, y);
function basisQuat(f, u) {
  const r = [u[1] * f[2] - u[2] * f[1], u[2] * f[0] - u[0] * f[2], u[0] * f[1] - u[1] * f[0]];
  const m = new THREE.Matrix4().makeBasis(P(...f), P(...u), P(...r));
  return new THREE.Quaternion().setFromRotationMatrix(m);
}
function replayQuat(q) {
  const [w, x, y, z] = q;
  return basisQuat([1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)],
                   [2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)]);
}

/* ---------- renderer, scene, camera ---------- */
const renderer = new THREE.WebGLRenderer({ canvas: $("view"), antialias: true });
renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.outputColorSpace = THREE.SRGBColorSpace;
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x0a1020);
scene.fog = new THREE.Fog(0x0a1020, 16000, 32000);
const camera = new THREE.PerspectiveCamera(70, 1, 10, 60000);
camera.position.set(-4300, 2000, -900);
const orbit = new OrbitControls(camera, renderer.domElement);
orbit.enableDamping = true;
orbit.maxPolarAngle = Math.PI * 0.49;
orbit.target.set(0, 0, 0);

scene.add(new THREE.HemisphereLight(0xd6e4ff, 0x1d2c1d, 1.0));
const sun = new THREE.DirectionalLight(0xffffff, 1.7);
sun.position.set(-1500, 9000, 900);
sun.castShadow = true;
sun.shadow.mapSize.set(4096, 4096);
Object.assign(sun.shadow.camera, { left: -6800, right: 6800, top: 6800, bottom: -6800, near: 1000, far: 16000 });
sun.shadow.bias = -0.0004;
scene.add(sun, sun.target);

function resize() {
  const w = window.innerWidth, h = window.innerHeight;
  renderer.setSize(w, h, false);
  camera.aspect = w / h;
  camera.updateProjectionMatrix();
}
window.addEventListener("resize", resize);
resize();

/* ---------- stadium ---------- */
function geomFromFlat(flat) {
  const pos = new Float32Array(flat.length);
  for (let i = 0; i < flat.length; i += 3) { pos[i] = flat[i]; pos[i + 1] = flat[i + 2]; pos[i + 2] = flat[i + 1]; }
  const g = new THREE.BufferGeometry();
  g.setAttribute("position", new THREE.BufferAttribute(pos, 3));
  g.computeVertexNormals();
  return g;
}

function floorTexture(d) {
  const W = 1024, H = 1500, cv = document.createElement("canvas");
  cv.width = W; cv.height = H;
  const c = cv.getContext("2d");
  const X = (x) => ((x + d.x) / (2 * d.x)) * W, Y = (y) => ((y + 6000) / 12000) * H, S = W / (2 * d.x);
  const a = d.corner - d.x, b = d.corner - d.y;
  const field = new Path2D();
  [[d.x, -a], [d.x, a], [b, d.y], [d.goal_hw, d.y], [d.goal_hw, d.goal_back], [-d.goal_hw, d.goal_back], [-d.goal_hw, d.y],
   [-b, d.y], [-d.x, a], [-d.x, -a], [-b, -d.y], [-d.goal_hw, -d.y], [-d.goal_hw, -d.goal_back], [d.goal_hw, -d.goal_back],
   [d.goal_hw, -d.y], [b, -d.y]].forEach(([x, y], i) => (i ? field.lineTo(X(x), Y(y)) : field.moveTo(X(x), Y(y))));
  field.closePath();
  c.save(); c.clip(field);
  for (let k = 0; k < 24; k++) {           // mowing stripes, tinted per half
    const y0 = -6000 + k * 500;
    const blueHalf = y0 + 250 < 0;
    c.fillStyle = blueHalf ? (k % 2 ? "#2a5a3e" : "#2f6545") : (k % 2 ? "#3c5a33" : "#44653a");
    c.fillRect(0, Y(y0), W, Y(y0 + 500) - Y(y0) + 1);
  }
  c.globalAlpha = 0.16;
  c.fillStyle = "#3d7bff"; c.fillRect(0, Y(-6000), W, Y(0) - Y(-6000));
  c.fillStyle = "#ff8a1f"; c.fillRect(0, Y(0), W, Y(6000) - Y(0));
  c.globalAlpha = 1;
  c.strokeStyle = "rgba(255,255,255,0.75)"; c.lineWidth = 3;
  c.beginPath(); c.moveTo(X(-d.x), Y(0)); c.lineTo(X(d.x), Y(0)); c.stroke();
  c.beginPath(); c.arc(X(0), Y(0), 1000 * S, 0, 2 * Math.PI); c.stroke();
  for (const sy of [-1, 1]) {
    const y1 = sy * d.y, y2 = sy * (d.y - 1200);          // box in front of each goal
    c.strokeRect(X(-1600), Math.min(Y(y1), Y(y2)), 3200 * S, Math.abs(Y(y2) - Y(y1)));
    c.beginPath(); c.moveTo(X(-d.goal_hw), Y(y1)); c.lineTo(X(d.goal_hw), Y(y1)); c.lineWidth = 6; c.stroke(); c.lineWidth = 3;
  }
  c.restore();
  c.strokeStyle = "rgba(255,255,255,0.35)"; c.lineWidth = 4; c.stroke(field);
  const tex = new THREE.CanvasTexture(cv);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 8;
  return tex;
}

async function buildStadium() {
  const d = await (await fetch("/api/arena")).json();
  const dims = d.dims;
  const floor = new THREE.Mesh(new THREE.PlaneGeometry(2 * dims.x, 12000),
    new THREE.MeshStandardMaterial({ map: floorTexture(dims), roughness: 0.95, transparent: true, alphaTest: 0.5 }));
  floor.rotation.x = -Math.PI / 2;
  floor.receiveShadow = true;
  scene.add(floor);
  const ground = new THREE.Mesh(new THREE.PlaneGeometry(60000, 60000), new THREE.MeshStandardMaterial({ color: 0x0c1410 }));
  ground.rotation.x = -Math.PI / 2; ground.position.y = -2;
  scene.add(ground);

  const walls = geomFromFlat(d.arena);
  const cols = new Float32Array(walls.attributes.position.count * 3);
  const blue = new THREE.Color(0x3d7bff), orange = new THREE.Color(0xff8a1f), tmp = new THREE.Color();
  for (let i = 0; i < walls.attributes.position.count; i++) {
    const y = walls.attributes.position.getZ(i);
    tmp.copy(blue).lerp(orange, THREE.MathUtils.clamp((y + 2500) / 5000, 0, 1));
    cols.set([tmp.r, tmp.g, tmp.b], i * 3);
  }
  walls.setAttribute("color", new THREE.BufferAttribute(cols, 3));
  scene.add(new THREE.Mesh(walls, new THREE.MeshStandardMaterial({
    vertexColors: true, transparent: true, opacity: 0.09, side: THREE.DoubleSide, depthWrite: false, roughness: 0.2 })));
  scene.add(new THREE.LineSegments(new THREE.EdgesGeometry(walls, 20),
    new THREE.LineBasicMaterial({ color: 0x9fbcff, transparent: true, opacity: 0.28 })));

  const goals = geomFromFlat(d.goals);
  const half = goals.attributes.position.count / 2;   // build_goals() writes the +y goal first
  [[0, half, 1], [half, half, 0]].forEach(([start, count, team]) => {
    const g = goals.clone(); g.setDrawRange(start, count);
    scene.add(new THREE.Mesh(g, new THREE.MeshStandardMaterial({ color: TEAM[team], transparent: true, opacity: 0.22, side: THREE.DoubleSide, depthWrite: false })));
  });
  for (const [sy, team] of [[1, 1], [-1, 0]]) {
    const mat = new THREE.MeshStandardMaterial({ color: TEAM[team], emissive: TEAM[team], emissiveIntensity: 0.6 });
    for (const sx of [-1, 1]) {
      const post = new THREE.Mesh(new THREE.CylinderGeometry(14, 14, dims.goal_h, 12), mat);
      post.position.copy(P(sx * dims.goal_hw, sy * dims.y, dims.goal_h / 2)); scene.add(post);
    }
    const bar = new THREE.Mesh(new THREE.CylinderGeometry(14, 14, 2 * dims.goal_hw, 12), mat);
    bar.rotation.z = Math.PI / 2; bar.position.copy(P(0, sy * dims.y, dims.goal_h)); scene.add(bar);
  }
}

/* ---------- pads, ball, cars ---------- */
const padGroup = new THREE.Group();
scene.add(padGroup);
let padMeshes = [];
function buildPads(pads) {
  padGroup.clear();
  padMeshes = pads.map((p) => {
    const g = new THREE.Group();
    const r = p.big ? 150 : 70;
    const base = new THREE.Mesh(new THREE.CylinderGeometry(r, r, 6, 24), new THREE.MeshStandardMaterial({ color: 0x2a3340, roughness: 0.6 }));
    base.position.y = 3; g.add(base);
    const glowMat = new THREE.MeshStandardMaterial({ color: 0xffb340, emissive: 0xff9a1f, emissiveIntensity: 1.2 });
    const glow = p.big ? new THREE.Mesh(new THREE.SphereGeometry(50, 16, 12), glowMat) : new THREE.Mesh(new THREE.CylinderGeometry(34, 34, 8, 16), glowMat);
    glow.position.y = p.big ? 110 : 12; g.add(glow);
    g.position.copy(P(p.x, p.y, 0));
    padGroup.add(g);
    return { g, glow, big: p.big, x: p.x, y: p.y };
  });
}
function setPad(k, active) {
  const m = padMeshes[k]; if (!m) return;
  m.glow.visible = active;
}

function ballTexture() {
  const cv = document.createElement("canvas"); cv.width = 1024; cv.height = 512;
  const c = cv.getContext("2d");
  c.fillStyle = "#cfd7df"; c.fillRect(0, 0, 1024, 512);
  c.strokeStyle = "#7f8b98"; c.lineWidth = 5;
  const s = 46, h = s * Math.sqrt(3);
  for (let row = -1; row < 512 / h + 1; row++) for (let col = -1; col < 1024 / (1.5 * s) + 1; col++) {
    const cx = col * 1.5 * s, cy = row * h + (col % 2 ? h / 2 : 0);
    c.beginPath();
    for (let k = 0; k < 6; k++) { const a = (Math.PI / 3) * k; c.lineTo(cx + s * Math.cos(a), cy + s * Math.sin(a)); }
    c.closePath(); c.stroke();
  }
  const t = new THREE.CanvasTexture(cv); t.colorSpace = THREE.SRGBColorSpace; return t;
}
const BALL_TEX = ballTexture();
function makeBall() {
  const m = new THREE.Mesh(new THREE.SphereGeometry(BALL_R, 40, 28),
    new THREE.MeshStandardMaterial({ map: BALL_TEX, roughness: 0.45, metalness: 0.1 }));
  m.castShadow = true;
  scene.add(m);
  return m;
}

function labelSprite(text, color) {
  const cv = document.createElement("canvas"); cv.width = 512; cv.height = 96;
  const c = cv.getContext("2d");
  c.font = "600 52px Barlow, sans-serif";
  const w = Math.min(500, c.measureText(text).width + 36);
  c.fillStyle = "rgba(8,12,22,0.75)"; c.beginPath(); c.roundRect((512 - w) / 2, 10, w, 76, 14); c.fill();
  c.fillStyle = color; c.textAlign = "center"; c.textBaseline = "middle"; c.fillText(text, 256, 50);
  const tex = new THREE.CanvasTexture(cv); tex.colorSpace = THREE.SRGBColorSpace;
  const s = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, depthTest: false, transparent: true }));
  s.scale.set(420, 79, 1); s.position.y = 170; s.renderOrder = 10;
  return s;
}

function makeCar(team, hitbox, name) {
  const [L, W, H, ox, oz] = HITBOX[hitbox] || HITBOX.Octane;
  const g = new THREE.Group();
  const paint = new THREE.MeshStandardMaterial({ color: TEAM[team], roughness: 0.35, metalness: 0.35 });
  const dark = new THREE.MeshStandardMaterial({ color: 0x10161f, roughness: 0.3, metalness: 0.5 });
  const tire = new THREE.MeshStandardMaterial({ color: 0x15181c, roughness: 0.9 });
  const body = new THREE.Mesh(new THREE.BoxGeometry(L, H * 0.7, W), paint);
  body.position.set(ox, oz - H * 0.15, 0);
  const nose = new THREE.Mesh(new THREE.BoxGeometry(L * 0.3, H * 0.35, W * 0.92), paint);
  nose.position.set(ox + L * 0.36, oz - H * 0.32, 0);
  const cabin = new THREE.Mesh(new THREE.BoxGeometry(L * 0.42, H * 0.55, W * 0.78), dark);
  cabin.position.set(ox - L * 0.1, oz + H * 0.38, 0);
  g.add(body, nose, cabin);
  for (const [fx, r] of [[0.33, 13], [-0.3, 16]]) for (const sz of [-1, 1]) {
    const wg = new THREE.CylinderGeometry(r, r, 14, 18); wg.rotateX(Math.PI / 2);
    const w = new THREE.Mesh(wg, tire);
    w.position.set(ox + fx * L, -17 + r, sz * (W / 2 - 5)); g.add(w);
  }
  const flameMat = new THREE.MeshBasicMaterial({ color: 0xffb347, transparent: true, opacity: 0.85 });
  const fg = new THREE.ConeGeometry(17, 110, 14); fg.rotateZ(Math.PI / 2);
  const flame = new THREE.Mesh(fg, flameMat);
  flame.position.set(ox - L / 2 - 52, oz - H * 0.15, 0); flame.visible = false; flame.userData.fixed = true;
  g.add(flame);
  g.traverse((o) => { if (o.isMesh && o !== flame) o.castShadow = true; });
  const label = labelSprite(name, TEAM_CSS[team]);
  g.add(label);
  scene.add(g);
  return { g, flame, label, mats: [paint, dark, tire] };
}

function setOpacity(obj, a) {
  if (obj.userData.alpha === a) return;
  obj.userData.alpha = a;
  obj.traverse((o) => {
    if (!o.isMesh || !o.material || o.userData.fixed) return;
    o.material.transparent = a < 1;
    o.material.opacity = a;
    o.material.depthWrite = a >= 1;
    o.material.needsUpdate = true;
    o.castShadow = a >= 1;
  });
}
function disposeObj(obj) {
  scene.remove(obj);
  obj.traverse((o) => { if (o.geometry) o.geometry.dispose(); if (o.material) { if (o.material.map && o.material.map !== BALL_TEX) o.material.map.dispose(); o.material.dispose(); } });
}

/* ---------- lines: prediction and real path ---------- */
function makeLine(color, dashed) {
  const geo = new THREE.BufferGeometry();
  geo.setAttribute("position", new THREE.BufferAttribute(new Float32Array(3 * 240), 3));
  const mat = dashed ? new THREE.LineDashedMaterial({ color, dashSize: 120, gapSize: 80, transparent: true, opacity: 0.95 })
                     : new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.55 });
  const line = new THREE.Line(geo, mat);
  line.frustumCulled = false;
  scene.add(line);
  return line;
}
function setLine(line, pts) {
  const arr = line.geometry.attributes.position.array;
  const n = Math.min(pts.length, arr.length / 3);
  for (let i = 0; i < n; i++) { arr[3 * i] = pts[i][0]; arr[3 * i + 1] = pts[i][2]; arr[3 * i + 2] = pts[i][1]; }
  line.geometry.setDrawRange(0, n);
  line.geometry.attributes.position.needsUpdate = true;
  if (line.material.isLineDashedMaterial) line.computeLineDistances();
}
const predLine = makeLine(GHOST, true);
const realLine = makeLine(0xffffff, false);
predLine.visible = realLine.visible = false;

/* ---------- app state ---------- */
let R = null;                 // loaded replay (viewer json)
let replayBall = null, replayCars = [];
let simBall = null, simCars = new Map();
let mode = "replay";          // "replay" | "sim"
let t = 0, playing = false, speed = 0.5;
let modes = {};               // player index -> mode
let focus = 0;                // player index followed by car cameras
let forkT = null, sim = null, padMap = null, placing = false;
const padPickups = [];

/* ---------- replay sampling ---------- */
function frameAt(time) {
  const T = R.time; let lo = 0, hi = T.length - 1;
  if (time <= T[0]) return [0, 0];
  if (time >= T[hi]) return [hi, 0];
  while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (T[mid] <= time) lo = mid; else hi = mid; }
  return [lo, (time - T[lo]) / Math.max(T[lo + 1] - T[lo], 1e-6)];
}
function lerp3(a, b, k) { return !a ? null : !b ? a : [a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k]; }
function slerpQ(a, b, k) {
  if (!a) return null;
  const qa = replayQuat(a);
  if (!b) return qa;
  return qa.slerp(replayQuat(b), k);
}
function sample(time) {
  const [i, k] = frameAt(time), j = Math.min(i + 1, R.time.length - 1);
  return {
    i,
    ball: { p: lerp3(R.ball.pos[i], R.ball.pos[j], k), q: slerpQ(R.ball.quat[i], R.ball.quat[j], k) },
    cars: R.players.map((pl) => ({ p: lerp3(pl.pos[i], pl.pos[j], k), q: slerpQ(pl.quat[i], pl.quat[j], k),
                                   boost: pl.boost[i], on: !!pl.boosting[i] })),
  };
}
function clockText(i) {
  const s = R.clock[i], m = Math.floor(s / 60), r = String(s % 60).padStart(2, "0");
  return (R.ot_frame !== null && i > R.ot_frame ? "+" : "") + `${m}:${r}`;
}
function scoreAt(time) {
  const s = [0, 0];
  R.goals.forEach((g) => { if (g.t <= time) s[g.team]++; });
  return s;
}

/* ---------- loading ---------- */
function status(text) { $("status").textContent = text; }

async function loadList() {
  status("lecture du dossier…");
  const list = await (await fetch("/api/replays")).json();
  const sel = $("replaySel");
  sel.innerHTML = "";
  list.forEach((e) => {
    if (e.error) return;
    const o = document.createElement("option");
    o.value = e.file.replace(/\.replay$/, "");
    const mine = e.my_team === null || e.my_team === undefined ? "" : (e.score[e.my_team] > e.score[1 - e.my_team] ? " · victoire" : " · défaite");
    o.textContent = `${e.date.slice(0, 10)} · ${e.team_size}v${e.team_size} · ${e.map} · ${e.score[0]}–${e.score[1]}${mine}`;
    sel.appendChild(o);
  });
  status(`${list.length} replays`);
  if (sel.options.length) loadReplay(sel.value);
}

async function loadReplay(key) {
  stopSim();
  status("analyse du replay… (quelques secondes la première fois)");
  $("playBtn").disabled = $("forkBtn").disabled = $("timeline").disabled = true;
  const res = await fetch(`/api/replay/${key}`);
  if (!res.ok) { status("erreur : " + (await res.text()).slice(0, 160)); return; }
  R = await res.json();
  replayCars.forEach((c) => disposeObj(c.g));
  if (replayBall) disposeObj(replayBall);
  replayBall = makeBall();
  replayCars = R.players.map((p) => makeCar(p.team, p.hitbox, p.name));
  buildPads(R.pads);
  padPickups.length = 0;
  R.pads.forEach(() => padPickups.push([]));
  R.pad_events.forEach((e) => { if (e.pad !== null && padPickups[e.pad]) padPickups[e.pad].push(e.t); });
  modes = {}; R.players.forEach((p, k) => { modes[k] = "ghost"; });
  focus = Math.max(0, R.players.findIndex((p) => p.me));
  buildPlayerRows();
  const tl = $("timeline");
  tl.min = R.time[0]; tl.max = R.time[R.time.length - 1];
  t = R.goals.length ? Math.max(R.time[0], R.goals[0].t - 6) : R.time[0];
  tl.value = t;
  buildMarks();
  buildSaves();
  $("playBtn").disabled = $("forkBtn").disabled = tl.disabled = false;
  status(`${R.mode} · ${R.map}`);
  playing = false; $("playBtn").textContent = "Lecture";
}

function buildMarks() {
  const box = $("marks"); box.innerHTML = "";
  const t0 = R.time[0], span = R.time[R.time.length - 1] - t0;
  const add = (time, cls, title) => {
    const s = document.createElement("span"); s.className = cls; s.style.left = (100 * (time - t0) / span) + "%"; s.title = title; box.appendChild(s);
  };
  const meIdx = R.players.findIndex((p) => p.me);
  R.touches.forEach((tc) => { if (tc.player === meIdx) add(tc.t, "me", "ta touche"); });
  R.goals.forEach((g) => add(g.t, "goal" + g.team, "but " + (g.team ? "orange" : "bleu")));
  (R.saves || []).forEach((s) => add(s.t, "save" + s.team, "arrêt de " + R.players[s.player].name));
  if (forkT !== null) add(forkT, "fork", "départ de la simulation");
  if (forkT !== null && releaseAt !== null) add(forkT + releaseAt, "rel", "le ballon devient simulé");
}

let releaseAt = null;         // sim seconds when the ball stopped following the replay
function updateSyncBadge() {
  const b = $("syncBadge");
  b.hidden = mode !== "sim" || !sim;
  if (b.hidden) return;
  b.className = "badge " + (sim.locked ? "locked" : "free");
  b.textContent = sim.locked ? "Ballon = replay" : `Ballon simulé depuis +${sim.rel.toFixed(1).replace(".", ",")} s`;
  if (!sim.locked && releaseAt === null) { releaseAt = sim.rel; buildMarks(); }
}

/* ---------- saves: list, timeline diamonds, markers on the pitch ---------- */
const saveGroup = new THREE.Group();
scene.add(saveGroup);
let saveMarks = [];

function goTo(time, play) {
  stopSim();
  t = Math.min(Math.max(time, R.time[0]), R.time[R.time.length - 1]);
  $("timeline").value = t;
  playing = play;
  $("playBtn").textContent = play ? "Pause" : "Lecture";
}

function setModes(fn) {
  Object.keys(modes).forEach((k) => { modes[k] = fn(R.players[k], Number(k)); $("mode" + k).value = modes[k]; });
}

function whatIfWithout(s, replacement = "removed") {
  stopSim();
  setModes(() => "ghost");
  modes[s.player] = replacement; $("mode" + s.player).value = replacement;
  t = Math.max(R.time[0], s.t - 1.5);
  $("timeline").value = t;
  fork(t);
}

function buildSaves() {
  saveGroup.children.slice().forEach(disposeObj);
  saveGroup.clear();
  saveMarks = [];
  const list = $("savesList"); list.innerHTML = "";
  const saves = R.saves || [];
  const meIdx = R.players.findIndex((p) => p.me);
  const mine = saves.filter((s) => s.player === meIdx);
  $("savesSummary").textContent = !saves.length ? "Aucun arrêt dans ce replay." :
    `${saves.length} arrêt${saves.length > 1 ? "s" : ""} compté${saves.length > 1 ? "s" : ""} par le jeu : ` +
    `${saves.filter((s) => s.team === 0).length} bleu, ${saves.filter((s) => s.team === 1).length} orange.` +
    (meIdx >= 0 ? ` Toi : ${mine.length}, dont ${mine.filter((s) => s.going_in).length} où le ballon rentrait vraiment.` : "");
  saves.forEach((s) => {
    const name = R.players[s.player].name, clock = clockText(frameAt(s.t)[0]);
    const col = TEAM[s.team];
    const g = new THREE.Group();
    const ring = new THREE.Mesh(new THREE.RingGeometry(130, 175, 40),
      new THREE.MeshBasicMaterial({ color: col, transparent: true, opacity: 0.9, side: THREE.DoubleSide, depthWrite: false }));
    ring.rotation.x = -Math.PI / 2; ring.position.y = 4;
    const h = Math.max(s.pos[2], 100);
    const pole = new THREE.Mesh(new THREE.CylinderGeometry(5, 5, h, 8), new THREE.MeshBasicMaterial({ color: col, transparent: true, opacity: 0.6 }));
    pole.position.y = h / 2;
    const dot = new THREE.Mesh(new THREE.SphereGeometry(30, 14, 10), new THREE.MeshBasicMaterial({ color: s.going_in ? 0xffffff : 0x8a96a3 }));
    dot.position.y = s.pos[2];
    const label = labelSprite(`${clock} ${name}`, TEAM_CSS[s.team]);
    label.position.y = s.pos[2] + 230;
    g.add(ring, pole, dot, label);
    g.position.copy(P(s.pos[0], s.pos[1], 0));
    saveGroup.add(g);

    const row = document.createElement("div");
    row.className = "save" + (s.team ? " orange" : "");
    row.innerHTML = '<span class="when"></span><span class="who"></span><span class="why"></span>' +
      '<div class="row"><button type="button">Voir</button><button type="button">Sans cet arrêt</button>' +
      '<button type="button">Nexto à sa place</button></div>';
    row.querySelector(".when").textContent = clock;
    row.querySelector(".who").textContent = name + (s.player === meIdx ? " (toi)" : "");
    const why = row.querySelector(".why");
    why.textContent = s.going_in ? `Le ballon rentrait ${String(s.eta).replace(".", ",")} s plus tard sans cette touche.`
                                 : "Compté par le jeu, mais le ballon seul ne rentrait pas dans les 3 s.";
    why.classList.toggle("in", s.going_in);
    const [watch, without, nexto] = row.querySelectorAll("button");
    watch.addEventListener("click", () => goTo(s.t - 2.5, true));
    without.addEventListener("click", () => whatIfWithout(s));
    nexto.addEventListener("click", () => whatIfWithout(s, "nexto"));
    list.appendChild(row);
    saveMarks.push({ ring, row, s });
  });
}

function updateSaves(time) {
  saveGroup.visible = $("showSaves").checked;
  saveMarks.forEach((m) => {
    const near = Math.abs(time - m.s.t) < 2.5;
    m.ring.scale.setScalar(near ? 1.4 : 1);
    m.row.classList.toggle("near", near);
  });
}

function buildPlayerRows() {
  const box = $("players"); box.innerHTML = "";
  R.players.forEach((p, k) => {
    const row = document.createElement("div");
    row.className = "prow" + (p.team ? " orange" : "");
    const name = document.createElement("span"); name.className = "pname"; name.textContent = p.name;
    if (p.me) { const s = document.createElement("small"); s.textContent = "toi"; name.appendChild(s); }
    const cam = document.createElement("button"); cam.type = "button"; cam.textContent = "Suivre";
    cam.addEventListener("click", () => { focus = k; if ($("camSel").value === "free" || $("camSel").value === "top") $("camSel").value = "ballcam"; onCamChange(); });
    const sel = document.createElement("select"); sel.id = "mode" + k; sel.setAttribute("aria-label", "Rôle de " + p.name);
    MODES.forEach(([v, txt]) => { const o = document.createElement("option"); o.value = v; o.textContent = txt; sel.appendChild(o); });
    sel.value = modes[k];
    sel.addEventListener("change", () => {
      if (sel.value === "control") Object.keys(modes).forEach((j) => { if (modes[j] === "control") { modes[j] = "ghost"; $("mode" + j).value = "ghost"; } });
      modes[k] = sel.value;
      if (sel.value === "control") { focus = k; $("camSel").value = "ballcam"; onCamChange(); }
      if (mode === "sim") fork(forkT);
    });
    row.append(name, cam, sel);
    box.appendChild(row);
  });
  const quick = document.createElement("div");
  quick.className = "row";
  const meTeam = (R.players.find((p) => p.me) || { team: -1 }).team;
  [["Adversaires en Nexto", (p) => (p.team !== meTeam ? "nexto" : "ghost")],
   ["Tous en Nexto", () => "nexto"],
   ["Tous en fantôme", () => "ghost"]].forEach(([txt, fn]) => {
    const b = document.createElement("button"); b.type = "button"; b.textContent = txt;
    b.addEventListener("click", () => { setModes(fn); if (mode === "sim") fork(forkT); });
    quick.appendChild(b);
  });
  box.appendChild(quick);
}

/* ---------- simulation link ---------- */
let ws = null;
function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onmessage = (e) => onMessage(JSON.parse(e.data));
  ws.onclose = () => { ws = null; setTimeout(connect, 1000); };
}
connect();
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

function fork(at) {
  if (!R) return;
  forkT = at;
  send({ type: "fork", key: R.key, t: at, modes, speed });
  mode = "sim"; playing = false; sim = null; releaseAt = null;
  simCars.forEach((c) => disposeObj(c.g)); simCars.clear();
  if (!simBall) simBall = makeBall();
  simBall.visible = true;
  R.players.forEach((p, k) => {
    if (modes[k] === "removed") return;
    simCars.set(k, makeCar(p.team, p.hitbox, p.name + (MODE_TAG[modes[k]] || "")));
  });
  $("modeBadge").textContent = "Simulation"; $("modeBadge").classList.add("sim");
  $("forkBtn").hidden = true; $("simBtns").hidden = false; $("ballBox").hidden = false;
  $("timeline").disabled = true; $("playBtn").disabled = true;
  $("simPause").textContent = "Pause";
  $("banner").hidden = true;
  buildMarks();
}

function stopSim() {
  if (mode !== "sim") return;
  send({ type: "stop" });
  mode = "replay"; sim = null; releaseAt = null;
  $("syncBadge").hidden = true;
  simCars.forEach((c) => disposeObj(c.g)); simCars.clear();
  if (simBall) simBall.visible = false;
  predLine.visible = realLine.visible = false;
  replayBall && setOpacity(replayBall, 1);
  replayCars.forEach((c) => setOpacity(c.g, 1));
  $("modeBadge").textContent = "Replay"; $("modeBadge").classList.remove("sim");
  $("forkBtn").hidden = false; $("simBtns").hidden = true; $("ballBox").hidden = true;
  $("timeline").disabled = false; $("playBtn").disabled = false;
  $("banner").hidden = true; $("boostMeter").hidden = true;
  if (forkT !== null) { t = forkT; $("timeline").value = t; }
}

function onMessage(m) {
  if (m.type === "status") { status(m.text); return; }
  if (m.type === "forked") {
    if (R) status(`${R.mode} · ${R.map}`);
    padMap = m.pads.map(([x, y]) => {
      let best = -1, bd = 1e9;
      padMeshes.forEach((p, k) => { const dd = Math.hypot(p.x - x, p.y - y); if (dd < bd) { bd = dd; best = k; } });
      return bd < 300 ? best : -1;
    });
  } else if (m.type === "state" && mode === "sim") {
    sim = m;
    if (m.pred) setLine(predLine, [m.ball.p, ...m.pred]);
    (m.events || []).forEach((e) => {
      if (e.type === "goal") {
        const b = $("banner");
        b.innerHTML = `But pour les ${e.team === 0 ? "bleus" : "orange"} !<small>${e.t.toFixed(2).replace(".", ",")} s après le départ de la simulation. Recommencer, ou changer le ballon pour continuer.</small>`;
        b.style.borderColor = TEAM_CSS[e.team]; b.hidden = false;
        $("simPause").textContent = "Reprendre";
      }
    });
  }
}

/* ---------- applying states to 3D objects ---------- */
function placeCar(car, p, q, on, visible) {
  car.g.visible = visible && !!p;
  if (!p) return;
  car.g.position.copy(P(p[0], p[1], p[2]));
  if (q) car.g.quaternion.copy(q);
  car.flame.visible = !!on;
}

function updateReplayLayer(time, ghostAlpha) {
  if (!R) return null;
  const s = sample(time);
  const show = ghostAlpha === 1 || $("showReal").checked;
  if (s.ball.p) { replayBall.position.copy(P(...s.ball.p)); if (s.ball.q) replayBall.quaternion.copy(s.ball.q); }
  replayBall.visible = show && !!s.ball.p;
  setOpacity(replayBall, ghostAlpha);
  s.cars.forEach((c, k) => {
    placeCar(replayCars[k], c.p, c.q, c.on, show);
    setOpacity(replayCars[k].g, ghostAlpha);
    replayCars[k].label.visible = $("showNames").checked && (ghostAlpha === 1 || modes[k] === "removed");
  });
  if (mode === "replay") {
    padMeshes.forEach((pm, k) => {
      const ups = padPickups[k] || [], cd = pm.big ? 10 : 4;
      let active = true;
      for (const u of ups) { if (u <= time && time < u + cd) { active = false; break; } }
      setPad(k, active);
    });
  }
  return s;
}

function updateSimLayer() {
  if (!sim) return;
  simBall.position.copy(P(...sim.ball.p));
  simBall.quaternion.copy(basisQuat(sim.ball.f, sim.ball.u));
  sim.cars.forEach((c) => {
    const car = simCars.get(c.i); if (!car) return;
    const replayOn = c.m === "ghost" && R.players[c.i].boosting[frameAt(sim.rt)[0]];
    placeCar(car, c.d ? null : c.p, basisQuat(c.f, c.u), c.on || replayOn, true);
    car.label.visible = $("showNames").checked;
  });
  if (padMap) [...sim.pads].forEach((ch, k) => { if (padMap[k] >= 0) setPad(padMap[k], ch === "1"); });
  const drv = sim.cars.find((c) => c.m === "control");
  $("boostMeter").hidden = !drv;
  if (drv) $("boostVal").textContent = drv.b;
}

/* ---------- cameras ---------- */
const camPos = new THREE.Vector3(), camLook = new THREE.Vector3();
function activeBall() { return mode === "sim" && simBall ? simBall : replayBall; }
function activeCar() { return (mode === "sim" && simCars.get(focus)) || replayCars[focus]; }
function onCamChange() {
  const v = $("camSel").value;
  orbit.enabled = v === "free";
  camera.up.set(v === "top" ? 1 : 0, v === "top" ? 0 : 1, 0);
  if (v === "free") { const b = activeBall(); if (b) orbit.target.copy(b.position); }
  const name = R && R.players[focus] ? R.players[focus].name : "";
  $("camHint").textContent = v === "ballcam" || v === "carcam" ? `Voiture suivie : ${name}. Change avec « Suivre ».` : v === "free" ? "Clic gauche : tourner, clic droit : déplacer, molette : zoom." : "";
}
$("camSel").addEventListener("change", onCamChange);

function updateCamera(dt) {
  const v = $("camSel").value, ball = activeBall(), car = activeCar();
  const k = 1 - Math.exp(-dt * 8);
  if (v === "free") {
    if ($("followBall").checked && ball && ball.visible) {
      const delta = ball.position.clone().sub(orbit.target).multiplyScalar(k);
      orbit.target.add(delta); camera.position.add(delta);
    }
    orbit.update();
    return;
  }
  if (v === "top") {
    camPos.set(0, 15500, 0); camera.position.lerp(camPos, k); camera.lookAt(0, 0, 0); return;
  }
  if (!car || !car.g.visible || !ball) return;
  const cp = car.g.position;
  if (v === "ballcam") {
    const dir = ball.position.clone().sub(cp); dir.y = 0;
    if (dir.lengthSq() < 1) dir.set(1, 0, 0);
    dir.normalize();
    camPos.copy(cp).addScaledVector(dir, -300).add(new THREE.Vector3(0, 125, 0));
    camLook.copy(ball.position);
  } else {
    const fwd = new THREE.Vector3(1, 0, 0).applyQuaternion(car.g.quaternion); fwd.y = 0;
    if (fwd.lengthSq() < 1e-4) fwd.set(1, 0, 0);
    fwd.normalize();
    camPos.copy(cp).addScaledVector(fwd, -320).add(new THREE.Vector3(0, 130, 0));
    camLook.copy(cp).addScaledVector(fwd, 500).add(new THREE.Vector3(0, 40, 0));
  }
  camera.position.lerp(camPos, 1 - Math.exp(-dt * 12));
  camera.lookAt(camLook);
}

/* ---------- driving input ---------- */
const keys = new Set();
const typing = (e) => ["INPUT", "SELECT", "TEXTAREA"].includes(e.target.tagName) && e.target.type !== "checkbox" && e.target.type !== "range";
const driving = () => mode === "sim" && Object.values(modes).includes("control");
window.addEventListener("keydown", (e) => {
  if (typing(e)) return;
  if (driving()) {
    keys.add(e.code);
    if (["Space", "ArrowUp", "ArrowDown", "ControlLeft"].includes(e.code)) e.preventDefault();
  } else if (e.code === "Space" && mode === "replay" && R) { e.preventDefault(); togglePlay(); }
});
window.addEventListener("keyup", (e) => keys.delete(e.code));
window.addEventListener("blur", () => keys.clear());

function readControls() {
  const k = (c) => (keys.has(c) ? 1 : 0);
  let throttle = k("KeyW") - k("KeyS"), steer = k("KeyD") - k("KeyA");
  let pitch = k("KeyS") - k("KeyW"), roll = k("KeyE") - k("KeyQ");
  let jump = keys.has("Space"), boost = keys.has("ShiftLeft") || keys.has("ShiftRight");
  let handbrake = keys.has("ControlLeft") || keys.has("KeyC");
  const pad = navigator.getGamepads ? [...navigator.getGamepads()].find((g) => g) : null;
  if (pad) {
    const dz = (v) => (Math.abs(v) < 0.12 ? 0 : v);
    const btn = (i) => (pad.buttons[i] ? pad.buttons[i].value : 0);
    const lx = dz(pad.axes[0] || 0), ly = dz(pad.axes[1] || 0);
    if (lx || ly || btn(7) || btn(6) || btn(0) || btn(1) || btn(2)) {
      throttle = btn(7) - btn(6); steer = lx; pitch = ly; roll = btn(5) - btn(4);
      jump = btn(0) > 0.5; boost = btn(1) > 0.5; handbrake = btn(2) > 0.5;
    }
  }
  let yaw = steer;
  if (handbrake && !roll) { roll = steer; yaw = 0; }       // air roll: steering becomes roll
  if (boost) throttle = Math.max(throttle, 1);
  return { throttle, steer, pitch, yaw, roll, jump, boost, handbrake };
}

/* ---------- UI events ---------- */
function togglePlay() {
  if (mode !== "replay" || !R) return;
  if (!playing && t >= R.time[R.time.length - 1] - 0.05) t = R.time[0];
  playing = !playing;
  $("playBtn").textContent = playing ? "Pause" : "Lecture";
}
$("playBtn").addEventListener("click", togglePlay);
$("timeline").addEventListener("input", (e) => { t = parseFloat(e.target.value); });
$("speedSel").addEventListener("change", (e) => { speed = parseFloat(e.target.value); send({ type: "speed", value: speed }); });
$("replaySel").addEventListener("change", (e) => loadReplay(e.target.value));
$("forkBtn").addEventListener("click", () => fork(t));
$("simPause").addEventListener("click", () => {
  const pause = !(sim && sim.paused);
  send({ type: "pause", value: pause });
  $("simPause").textContent = pause ? "Reprendre" : "Pause";
  if (!pause) $("banner").hidden = true;
});
$("simReset").addEventListener("click", () => fork(forkT));
$("simBack").addEventListener("click", stopSim);
$("panelToggle").addEventListener("click", () => {
  const off = document.body.classList.toggle("nopanel");
  $("panelToggle").setAttribute("aria-expanded", String(!off));
});

const ballFields = ["bx", "by", "bz", "bvx", "bvy", "bvz"];
function ballInputsFocused() { return ballFields.includes(document.activeElement && document.activeElement.id); }
function readBallInputs() { const v = ballFields.map((id) => parseFloat($(id).value) || 0); return { pos: v.slice(0, 3), vel: v.slice(3) }; }
$("ballApply").addEventListener("click", () => { const b = readBallInputs(); send({ type: "set_ball", pos: b.pos, vel: b.vel }); $("banner").hidden = true; });
$("ballStop").addEventListener("click", () => send({ type: "set_ball", vel: [0, 0, 0] }));
$("ballSlow").addEventListener("click", () => sim && send({ type: "set_ball", vel: sim.ball.v.map((x) => x * 0.5) }));
$("ballFast").addEventListener("click", () => sim && send({ type: "set_ball", vel: sim.ball.v.map((x) => x * 1.5) }));
$("ballPlace").addEventListener("click", () => { placing = !placing; $("ballPlace").classList.toggle("active", placing); });

const ray = new THREE.Raycaster(), groundPlane = new THREE.Plane(new THREE.Vector3(0, 1, 0), -BALL_R - 2);
let downAt = null;
renderer.domElement.addEventListener("pointerdown", (e) => { downAt = [e.clientX, e.clientY]; });
renderer.domElement.addEventListener("pointerup", (e) => {
  if (!placing || !downAt || Math.hypot(e.clientX - downAt[0], e.clientY - downAt[1]) > 5) return;
  ray.setFromCamera(new THREE.Vector2((e.clientX / window.innerWidth) * 2 - 1, -(e.clientY / window.innerHeight) * 2 + 1), camera);
  const hit = new THREE.Vector3();
  if (ray.ray.intersectPlane(groundPlane, hit)) {
    send({ type: "set_ball", pos: [hit.x, hit.z, BALL_R + 2], vel: [0, 0, 0] });
    placing = false; $("ballPlace").classList.remove("active"); $("banner").hidden = true;
  }
});

/* ---------- main loop ---------- */
let last = performance.now(), lastSend = 0;
function frame(now) {
  const dt = Math.min((now - last) / 1000, 0.1); last = now;
  if (R) {
    if (mode === "replay") {
      if (playing) {
        t += dt * speed;
        if (t >= R.time[R.time.length - 1]) { t = R.time[R.time.length - 1]; playing = false; $("playBtn").textContent = "Lecture"; }
        $("timeline").value = t;
      }
      const s = updateReplayLayer(t, 1);
      updateSaves(t);
      const sc = scoreAt(t);
      $("scBlue").textContent = sc[0]; $("scOrange").textContent = sc[1];
      $("clock").textContent = clockText(s.i);
      $("tlabel").textContent = `${(t - R.time[0]).toFixed(1).replace(".", ",")} s`;
    } else if (sim) {
      updateReplayLayer(sim.rt, 0.28);
      updateSimLayer();
      updateSyncBadge();
      updateSaves(sim.rt);
      $("timeline").value = sim.rt;
      const base = scoreAt(forkT);
      $("scBlue").textContent = base[0] + sim.score[0]; $("scOrange").textContent = base[1] + sim.score[1];
      $("clock").textContent = clockText(frameAt(sim.rt)[0]);
      $("tlabel").textContent = `sim +${sim.t.toFixed(1).replace(".", ",")} s`;
      predLine.visible = $("showPred").checked;
      const [i0] = frameAt(sim.rt), [i1] = frameAt(sim.rt + 4);
      setLine(realLine, R.ball.pos.slice(i0, i1 + 1).filter(Boolean).slice(0, 240));
      realLine.visible = $("showReal").checked;
      if (!ballInputsFocused()) ballFields.forEach((id, n) => { $(id).value = Math.round(n < 3 ? sim.ball.p[n] : sim.ball.v[n - 3]); });
      if (driving() && now - lastSend > 14) { send({ type: "controls", c: readControls() }); lastSend = now; }
    }
  }
  updateCamera(dt);
  renderer.render(scene, camera);
  requestAnimationFrame(frame);
}

buildStadium().then(() => { onCamChange(); loadList(); requestAnimationFrame(frame); });
