"""Prepare replays for the lab. Runs in its own short-lived process: the parser needs
~600-800 MB while reading a replay, and that memory only goes back to the system when
the process exits.

  python -m lab.parse_replay list DEMOS_DIR OUT_JSON      # index of all replays
  python -m lab.parse_replay parse REPLAY CACHE_DIR       # one replay -> .npz (sim) + .json (viewer)
"""
import glob
import json
import os
import sys

import numpy as np
import subtr_actor as sa

ME = os.environ.get("RL_PLAYER", "Anxoide")
MODES = {10: "Ranked 1v1", 11: "Ranked 2v2", 13: "Ranked 3v3", 1: "Casual 1v1", 2: "Casual 2v2",
         3: "Casual 3v3", 6: "Private"}


def build_list(demos_dir, out_json):
    old = {}
    if os.path.exists(out_json):
        old = {e["file"]: e for e in json.load(open(out_json))}
    out = []
    for path in glob.glob(os.path.join(demos_dir, "*.replay")):
        name = os.path.basename(path)
        st = os.stat(path)
        if name in old and old[name].get("mtime") == int(st.st_mtime):
            out.append(old[name])
            continue
        try:
            h = dict(sa.get_replay_meta(path)["replay_meta"]["all_headers"])
        except Exception as e:  # unreadable replay: list it so the user sees why
            out.append({"file": name, "mtime": int(st.st_mtime), "error": str(e)[:200]})
            continue
        stats = h.get("PlayerStats") or []
        me = next((p for p in stats if p.get("Name") == ME), None)
        out.append({
            "file": name, "mtime": int(st.st_mtime), "date": h.get("Date", "")[:16],
            "map": h.get("MapName"), "team_size": h.get("TeamSize"),
            "score": [h.get("Team0Score", 0), h.get("Team1Score", 0)],
            "my_team": me.get("Team") if me else None,
            "players": [[p.get("Name"), p.get("Team")] for p in stats],
        })
    out.sort(key=lambda e: e.get("date", ""), reverse=True)
    tmp = out_json + ".tmp"
    json.dump(out, open(tmp, "w"))
    os.replace(tmp, out_json)


def _r(a, nd=0):
    a = np.round(np.nan_to_num(a), nd)   # NaN frames are replaced by None afterwards
    return a.astype(int).tolist() if nd == 0 else a.tolist()


def _rows(arr, valid, nd=0):
    vals = _r(arr, nd)
    return [v if ok else None for v, ok in zip(vals, valid)]


VIEW_VERSION = 3   # bump when the viewer JSON changes; old cache files are then rebuilt


def view_path(cache_dir, key):
    return os.path.join(cache_dir, f"{key}.v{VIEW_VERSION}.json")


def find_saves(replay_path, r, players, touches):
    """Saves credited by the game (stats events), snapped to the saver's touch, then checked
    by rolling the ball alone forward: was it really going into the saver's net?"""
    from .sim import Sim, TICK
    from . import whatif as wi

    by_id = {p.remote: k for k, p in enumerate(players)}
    events = sa.get_stats_events(replay_path)["events"]
    out = []
    for e in events:
        pl = e["payload"]
        if pl.get("kind") != "core_player" or pl["payload"].get("saves_delta", 0) <= 0:
            continue
        p = pl["payload"]
        k = by_id.get(json.dumps(p["player"], sort_keys=True))
        if k is None:
            continue
        stat_t = float(p["time"])
        mine = [tc["time"] for tc in touches if tc["player"] == players[k].name and stat_t - 1.0 <= tc["time"] <= stat_t + 0.2]
        t = max(mine) if mine else stat_t
        i = wi.frame_at(r, t - 0.1)
        sim = Sim()
        sim.set_ball(r.ball_pos[i], r.ball_vel[i], r.ball_angvel[i])
        eta = None
        for tick in range(1, 361):
            sim.arena.step(1)
            if sim.goals:
                if sim.goals[0][1] == 1 - players[k].team:
                    eta = tick * TICK
                break
        j = wi.frame_at(r, t)
        out.append({"t": round(t, 3), "frame": int(j), "player": k, "team": players[k].team,
                    "pos": [round(float(v)) for v in r.ball_pos[j]],
                    "going_in": eta is not None, "eta": None if eta is None else round(eta - 0.1, 2)})
    del events
    return out


def parse(replay_path, cache_dir):
    from . import replay_data as rd
    from . import whatif as wi

    r = rd.load(replay_path)
    key = os.path.splitext(os.path.basename(replay_path))[0]
    os.makedirs(cache_dir, exist_ok=True)
    players = [p for p in r.players if p.team in (0, 1) and p.valid.any()]

    np.savez_compressed(
        os.path.join(cache_dir, key + ".npz"),
        time=r.time, secs=r.seconds_remaining, ball_pos=r.ball_pos, ball_vel=r.ball_vel,
        ball_angvel=r.ball_angvel, ball_valid=r.ball_valid,
        p_pos=np.stack([p.pos for p in players]), p_vel=np.stack([p.vel for p in players]),
        p_angvel=np.stack([p.angvel for p in players]), p_quat=np.stack([p.quat for p in players]),
        p_boost=np.stack([p.boost for p in players]), p_valid=np.stack([p.valid for p in players]),
        p_team=np.array([p.team for p in players]),
        p_names=np.array([p.name for p in players]), p_hitbox=np.array([p.hitbox for p in players]),
    )

    h = dict(r.meta["all_headers"])
    pad_index = {pad["pad_id"]: k for k, pad in enumerate(r.boost_pads)}
    names = [p.name for p in players]
    attributed = wi.attribute_touches(r)
    touches = [{"t": round(t["time"], 3), "player": names.index(t["player"]) if t["player"] in names else None,
                "team": t["team"]} for t in attributed]
    ot = wi.overtime_start(r)
    view = {
        "key": key, "file": os.path.basename(replay_path), "date": h.get("Date", "")[:16],
        "map": h.get("MapName"), "mode": MODES.get((r.meta.get("game_type") or {}).get("playlist_id"),
                                                   f"{h.get('TeamSize')}v{h.get('TeamSize')}"),
        "score": [h.get("Team0Score", 0), h.get("Team1Score", 0)], "me": ME,
        "time": _r(r.time, 3), "clock": _r(r.seconds_remaining), "ot_frame": ot,
        "ball": {"pos": _rows(r.ball_pos, r.ball_valid), "quat": _rows(r.ball_quat, r.ball_valid, 3)},
        "players": [{
            "name": p.name, "team": p.team, "hitbox": p.hitbox, "me": p.name == ME,
            "pos": _rows(p.pos, p.valid), "quat": _rows(p.quat, p.valid, 3),
            "boost": [int(round(b / 2.55)) if ok and not np.isnan(b) else None for b, ok in zip(p.boost, p.valid)],
            "boosting": p.boosting.astype(int).tolist(),
        } for p in players],
        "goals": [{"t": round(g["time"], 3), "team": 0 if g["scoring_team_is_team_0"] else 1} for g in r.goals],
        "touches": touches,
        "saves": find_saves(replay_path, r, players, attributed),
        "pads": [{"x": pad["position"]["x"], "y": pad["position"]["y"], "z": pad["position"]["z"],
                  "big": pad.get("size") == "Big"} for pad in r.boost_pads],
        "pad_events": [{"t": round(e["time"], 3), "pad": pad_index.get(e["pad_id"])}
                       for e in getattr(r, "pad_events", [])],
    }
    out = view_path(cache_dir, key)
    json.dump(view, open(out + ".tmp", "w"), separators=(",", ":"))
    os.replace(out + ".tmp", out)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "list":
        build_list(sys.argv[2], sys.argv[3])
    elif cmd == "parse":
        parse(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
