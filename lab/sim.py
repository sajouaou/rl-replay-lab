"""RocketSim helpers: load replay states into an arena and roll them forward."""
import os

import numpy as np
import RocketSim as rs

from . import arena_mesh

HERE = os.path.dirname(os.path.abspath(__file__))
MESH_DIR = os.path.join(HERE, "meshes")
TICK = 1 / 120
HITBOXES = {"Octane": "OCTANE", "Dominus": "DOMINUS", "Plank": "PLANK", "Breakout": "BREAKOUT",
            "Hybrid": "HYBRID", "Merc": "MERC"}
_ready = False


def init():
    global _ready
    if not _ready:
        if not os.path.isdir(os.path.join(MESH_DIR, "soccar")):
            arena_mesh.write_meshes(MESH_DIR)
        rs.init(MESH_DIR)
        _ready = True


def quat_to_rotmat(q):
    """Replay quaternion (w, x, y, z) -> RocketSim RotMat (forward, right, up columns)."""
    w, x, y, z = q
    fwd = (1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y))
    right = (2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x))
    up = (2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y))
    return rs.RotMat(rs.Vec(*fwd), rs.Vec(*right), rs.Vec(*up))


class Sim:
    def __init__(self):
        init()
        self.arena = rs.Arena(rs.GameMode.SOCCAR)
        self.goals = []          # (tick, scoring team)
        self.touches = []        # (tick, car id)
        self.arena.set_goal_score_callback(self._on_goal)
        self.arena.set_ball_touch_callback(self._on_touch)
        self.cars = {}           # replay player index -> rs.Car

    def _on_goal(self, **kw):
        self.goals.append((self.arena.tick_count, kw.get("team")))

    def _on_touch(self, **kw):
        car = kw.get("car")
        self.touches.append((self.arena.tick_count, car.id if car is not None else None))

    def set_ball(self, pos, vel, angvel):
        s = rs.BallState()
        s.pos, s.vel, s.ang_vel = rs.Vec(*pos), rs.Vec(*vel), rs.Vec(*angvel)
        self.arena.ball.set_state(s)

    def add_car(self, key, team, hitbox):
        car = self.arena.add_car(team, getattr(rs.CarConfig, HITBOXES.get(hitbox, "OCTANE")))
        self.cars[key] = car
        return car

    def set_car(self, key, pos, vel, angvel, quat, boost=33.0):
        car = self.cars[key]
        s = car.get_state()
        s.pos, s.vel, s.ang_vel = rs.Vec(*pos), rs.Vec(*vel), rs.Vec(*angvel)
        s.rot_mat = quat_to_rotmat(quat)
        s.boost = float(boost)
        car.set_state(s)

    def ball_pos(self):
        p = self.arena.ball.get_state().pos
        return np.array([p.x, p.y, p.z])
