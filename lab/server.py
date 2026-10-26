"""Local web server for the replay lab: http://127.0.0.1:8765

  python -m lab.server [DEMOS_DIR]
"""
import asyncio
import json
import os
import sys
import time

import numpy as np
from aiohttp import WSMsgType, web

from . import arena_mesh
from . import session as session_mod
from .parse_replay import view_path
from .session import Session

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "cache")
STATIC = os.path.join(ROOT, "static")
DEFAULT_DEMOS = os.path.expanduser(
    "/mnt/data/SteamLibrary/steamapps/compatdata/252950/pfx/drive_c/users/steamuser/Documents/My Games/Rocket League/TAGame/Demos")
FPS = 60
parse_lock = asyncio.Lock()   # one replay parse at a time (each needs ~600-800 MB)


async def run_parser(*args):
    proc = await asyncio.create_subprocess_exec(sys.executable, "-m", "lab.parse_replay", *args, cwd=ROOT,
                                                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
    out, err = await proc.communicate()
    if proc.returncode != 0:
        raise web.HTTPInternalServerError(text=(err or out).decode()[-800:])


async def replays(request):
    path = os.path.join(CACHE, "replays.json")
    async with parse_lock:
        await run_parser("list", request.app["demos"], path)
    return web.FileResponse(path)


async def replay(request):
    out = await ensure_parsed(request.app, os.path.basename(request.match_info["key"]))
    return web.FileResponse(out, headers={"Cache-Control": "no-cache"})


async def ensure_parsed(app, key):
    out = view_path(CACHE, key)
    src = os.path.join(app["demos"], key + ".replay")
    if not os.path.exists(src):
        raise web.HTTPNotFound(text="replay introuvable")
    async with parse_lock:
        if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
            await run_parser("parse", src, CACHE)
    return out


async def saves(request):
    """Saves of a replay for the BakkesMod plugin, one per line: frame;clock;name;team;going_in;eta"""
    out = await ensure_parsed(request.app, _hex_key(request.match_info["key"]))
    v = json.load(open(out))
    ot = v.get("ot_frame")
    lines = []
    for s in v.get("saves", []):
        f = s["frame"]
        sec = v["clock"][f]
        clock = ("+" if ot is not None and f > ot else "") + f"{sec // 60}:{sec % 60:02d}"
        name = v["players"][s["player"]]["name"].replace(";", ",")
        lines.append(f"{f};{clock};{name};{s['team']};{int(s['going_in'])};{s['eta'] if s['eta'] is not None else ''}")
    return web.Response(text="\n".join(lines))


def _hex_key(raw):
    """The game may format the replay id differently from the file name: keep the hex digits only."""
    return "".join(c for c in raw if c in "0123456789abcdefABCDEF").upper()


_scene_cache = {}


async def scene(request):
    """What really happened from a replay frame on, for the BakkesMod plugin (plain text):
      map;<map>        p;<name>;<team>  (one per car, in order)
      f <t> <ball x y z vx vy vz wx wy wz>
      c <car> <present> <x y z vx vy vz wx wy wz pitch yaw roll boost01>   (after each f line)"""
    key = _hex_key(request.match_info["key"])
    view = await ensure_parsed(request.app, key)
    if key not in _scene_cache:
        _scene_cache.clear()                                   # keep one replay in memory
        _scene_cache[key] = (session_mod.load_npz(os.path.join(CACHE, key + ".npz")), json.load(open(view))["map"])
    r, map_name = _scene_cache[key]
    f0 = min(max(int(request.query.get("frame", 0)), 0), len(r.time) - 1)
    seconds = min(float(request.query.get("seconds", 10)), 30.0)
    lines = [f"map;{map_name}"] + [f"p;{p.name.replace(';', ',')};{p.team}" for p in r.players]
    rot = [replay_rotators(p.quat) for p in r.players]
    num = lambda xs: " ".join(f"{v:.6g}" for v in xs)
    i = f0
    while i < len(r.time) and r.time[i] - r.time[f0] <= seconds:
        lines.append(f"f {r.time[i] - r.time[f0]:.4f} " + num(np.concatenate([r.ball_pos[i], r.ball_vel[i], r.ball_angvel[i]])))
        for k, p in enumerate(r.players):
            if p.valid[i]:
                boost = 0.33 if np.isnan(p.boost[i]) else p.boost[i] / 255.0
                lines.append(f"c {k} 1 " + num(np.concatenate([p.pos[i], p.vel[i], p.angvel[i], rot[k][i], [boost]])))
            else:
                lines.append(f"c {k} 0")
        i += 1
    return web.Response(text="\n".join(lines))


def replay_rotators(quat):
    """Replay quaternions (w, x, y, z) -> Unreal rotators (pitch, yaw, roll) in rotation units."""
    w, x, y, z = (np.nan_to_num(quat[:, c]) for c in range(4))
    fz = 2 * (x * z - w * y)
    fx, fy = 1 - 2 * (y * y + z * z), 2 * (x * y + w * z)
    rz, uz = 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)
    k = 32768 / np.pi
    return np.stack([np.arcsin(np.clip(fz, -1, 1)) * k, np.arctan2(fy, fx) * k, np.arctan2(-rz, uz) * k], 1).round()


async def nexto(request):
    """Nexto's actions for the cars the BakkesMod plugin gives it; one line per car: id a0..a7"""
    data = await request.json()
    if session_mod._brain is None:
        await asyncio.get_running_loop().run_in_executor(None, session_mod.nexto_brain)
    actions = session_mod.nexto_brain().act_json(data)
    return web.Response(text="\n".join(f"{cid} " + " ".join(f"{float(x):g}" for x in a) for cid, a in actions.items()))


async def arena(request):
    return web.json_response(arena_mesh.visual_mesh())


async def index(request):
    return web.FileResponse(os.path.join(STATIC, "index.html"))


async def ws_handler(request):
    ws = web.WebSocketResponse(heartbeat=20)
    await ws.prepare(request)
    s = Session(CACHE)
    alive = True

    async def loop():
        acc, last, frame = 0.0, time.perf_counter(), 0
        while alive and not ws.closed:
            await asyncio.sleep(1 / FPS)
            now = time.perf_counter()
            dt, last = min(now - last, 0.1), now
            if s.sim is None:
                continue
            if not s.paused:
                acc += dt * 120 * s.speed
                n = int(acc)
                acc -= n
                s.step(min(n, 24))
            frame += 1
            await ws.send_str(json.dumps(s.state(with_prediction=frame % 6 == 0)))

    task = asyncio.create_task(loop())
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                continue
            m = json.loads(msg.data)
            kind = m.get("type")
            if kind == "fork":
                if "nexto" in m.get("modes", {}).values() and session_mod._brain is None:
                    await ws.send_str(json.dumps({"type": "status", "text": "chargement de Nexto… (quelques secondes, une seule fois)"}))
                    await asyncio.get_running_loop().run_in_executor(None, session_mod.nexto_brain)
                s.fork(m["key"], float(m["t"]), m.get("modes", {}), m.get("ball"), m.get("speed"))
                await ws.send_str(json.dumps({"type": "forked", "t0": s.t0, "pads": s.pad_positions()}))
            elif kind == "controls":
                s.set_controls(m.get("c", {}))
            elif kind == "pause":
                s.paused = bool(m.get("value", True))
            elif kind == "speed":
                s.speed = float(m.get("value", 1.0))
            elif kind == "reset":
                s.reset()
            elif kind == "set_ball" and s.sim:
                s.set_ball(m.get("pos"), m.get("vel"))
            elif kind == "stop":
                s.sim = None
    finally:
        alive = False
        task.cancel()
    return ws


def main():
    demos = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("RL_DEMOS", DEFAULT_DEMOS)
    os.makedirs(CACHE, exist_ok=True)
    app = web.Application()
    app["demos"] = demos
    app.add_routes([web.get("/", index), web.get("/api/replays", replays), web.get("/api/replay/{key}", replay),
                    web.get("/api/arena", arena), web.get("/api/saves/{key}", saves), web.post("/api/nexto", nexto),
                    web.get("/api/scene/{key}", scene),
                    web.get("/ws", ws_handler), web.static("/static", STATIC)])
    print(f"Labo replays : http://127.0.0.1:8765  (replays : {demos})", flush=True)
    web.run_app(app, host="127.0.0.1", port=8765, print=None)


if __name__ == "__main__":
    main()
