"""Load a Rocket League .replay into numpy arrays (ball + per-player rigid bodies over time)."""
import json
from dataclasses import dataclass, field

import numpy as np
import subtr_actor as sa

ANGVEL_SCALE = 1 / 100  # replays store angular velocity in rad/s * 100


def _rb(entry):
    """Rigid body dict from a frame entry, or None when the actor has no data that frame."""
    if not isinstance(entry, dict) or "Data" not in entry:
        return None
    data = entry["Data"]
    return data.get("rigid_body", data) if isinstance(data, dict) else None


def _vec(d):
    return (d["x"], d["y"], d["z"]) if d else (np.nan, np.nan, np.nan)


@dataclass
class Player:
    name: str
    team: int            # 0 = blue, 1 = orange
    hitbox: str
    pos: np.ndarray
    vel: np.ndarray
    angvel: np.ndarray
    quat: np.ndarray     # (w, x, y, z)
    boost: np.ndarray    # raw replay units 0..255
    valid: np.ndarray
    boosting: np.ndarray = None
    remote: str = ""     # platform id as canonical JSON, matches stats events


@dataclass
class Replay:
    path: str
    meta: dict
    time: np.ndarray
    seconds_remaining: np.ndarray
    ball_pos: np.ndarray
    ball_vel: np.ndarray
    ball_angvel: np.ndarray
    ball_valid: np.ndarray
    ball_quat: np.ndarray
    players: list
    touches: list = field(default_factory=list)
    goals: list = field(default_factory=list)
    demos: list = field(default_factory=list)

    def player(self, name):
        return next(p for p in self.players if p.name.lower() == name.lower())


def load(path):
    d = sa.get_replay_frames_data(path)
    fd = d["frame_data"]
    meta_frames = fd["metadata_frames"]
    n = len(meta_frames)
    time = np.array([m["time"] for m in meta_frames], dtype=np.float64)
    secs = np.array([m["seconds_remaining"] for m in meta_frames], dtype=np.float64)

    ball_pos = np.full((n, 3), np.nan)
    ball_vel = np.full((n, 3), np.nan)
    ball_ang = np.full((n, 3), np.nan)
    ball_valid = np.zeros(n, bool)
    ball_quat = np.tile([1.0, 0.0, 0.0, 0.0], (n, 1))
    for i, fr in enumerate(fd["ball_data"]["frames"]):
        rb = _rb(fr)
        if rb is None or rb.get("location") is None:
            continue
        ball_pos[i] = _vec(rb["location"])
        if rb.get("rotation"):
            q = rb["rotation"]
            ball_quat[i] = (q["w"], q["x"], q["y"], q["z"])
        ball_vel[i] = _vec(rb.get("linear_velocity")) if rb.get("linear_velocity") else (0, 0, 0)
        av = rb.get("angular_velocity")
        ball_ang[i] = np.array(_vec(av)) * ANGVEL_SCALE if av else (0, 0, 0)
        ball_valid[i] = True

    hitboxes = {}
    for team_key in ("team_zero", "team_one"):
        for p in d["meta"].get(team_key) or []:
            hitboxes[p["name"]] = p.get("car_hitbox_family") or "Octane"

    players = []
    for pid, pdata in fd["players"]:
        frames = pdata["frames"]
        pos = np.full((n, 3), np.nan); vel = np.full((n, 3), np.nan); ang = np.full((n, 3), np.nan)
        quat = np.full((n, 4), np.nan); boost = np.full(n, np.nan); valid = np.zeros(n, bool)
        boosting = np.zeros(n, bool)
        name, team = None, None
        for i, fr in enumerate(frames):
            if not isinstance(fr, dict) or "Data" not in fr:
                continue
            data = fr["Data"]
            rb = data.get("rigid_body")
            name = name or data.get("player_name")
            if team is None and data.get("is_team_0") is not None:
                team = 0 if data["is_team_0"] else 1
            if not rb or rb.get("location") is None:
                continue
            pos[i] = _vec(rb["location"])
            vel[i] = _vec(rb.get("linear_velocity")) if rb.get("linear_velocity") else (0, 0, 0)
            av = rb.get("angular_velocity")
            ang[i] = np.array(_vec(av)) * ANGVEL_SCALE if av else (0, 0, 0)
            q = rb["rotation"]
            quat[i] = (q["w"], q["x"], q["y"], q["z"])
            boost[i] = data.get("boost_amount", np.nan)
            boosting[i] = bool(data.get("boost_active"))
            valid[i] = True
        if name is None:
            name = next(iter(pid.values())).get("name", "?") if isinstance(next(iter(pid.values())), dict) else "?"
        players.append(Player(name, team if team is not None else -1, hitboxes.get(name, "Octane"),
                              pos, vel, ang, quat, boost, valid, boosting, json.dumps(pid, sort_keys=True)))

    r = Replay(path, d["meta"], time, secs, ball_pos, ball_vel, ball_ang, ball_valid, ball_quat, players,
               d.get("touch_events", []), d.get("goal_events", []), d.get("demolish_infos", []))
    r.boost_pads = d.get("boost_pads", [])
    r.pad_events = [e for e in d.get("boost_pad_events", []) if "PickedUp" in (e.get("kind") or {})]
    return r
