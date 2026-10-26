"""A live "what-if" simulation forked from a replay moment.

Each player can be:
  ghost    - replays exactly what they really did (can still hit the ball)
  removed  - not in the simulation
  free     - real physics, nobody at the controls (coasts)
  bot      - simple bot that drives at the ball
  nexto    - played by Nexto, the RLGym bot (loaded on first use, ~4 s and ~250 MB)
  control  - driven by the viewer (keyboard / gamepad)
"""
import math
import os
from types import SimpleNamespace

import numpy as np
import RocketSim as rs

from .sim import Sim, TICK, quat_to_rotmat
from .whatif import _interp_player

TICK_SKIP = 8          # Nexto acts every 8 ticks, like in the real game

GOAL_Y = 5120.0
# The ball follows the replay until a changed car gets this close to it (center to center, game units).
# Contact happens around 170-180; the margin covers ~3 ticks at full closing speed.
RELEASE_DIST = 320.0


_brain = None


def nexto_brain():
    """One Nexto model per server process, loaded the first time a car is given to Nexto."""
    global _brain
    if _brain is None:
        from .nexto_brain import NextoBrain
        _brain = NextoBrain()
    return _brain


def _controls_from_action(a):
    c = rs.CarControls()
    c.throttle, c.steer, c.pitch, c.yaw, c.roll = (float(x) for x in a[:5])
    c.jump, c.boost, c.handbrake = bool(a[5] > 0), bool(a[6] > 0), bool(a[7] > 0)
    return c


def _interp_ball(r, t):
    i = int(np.clip(np.searchsorted(r.time, t), 1, len(r.time) - 1))
    if not (r.ball_valid[i - 1] and r.ball_valid[i]):
        return None
    a = (t - r.time[i - 1]) / max(r.time[i] - r.time[i - 1], 1e-6)
    lerp = lambda x: x[i - 1] * (1 - a) + x[i] * a
    return lerp(r.ball_pos), lerp(r.ball_vel), lerp(r.ball_angvel)


def load_npz(path):
    z = np.load(path, allow_pickle=False)
    players = [SimpleNamespace(name=str(z["p_names"][k]), team=int(z["p_team"][k]), hitbox=str(z["p_hitbox"][k]),
                               pos=z["p_pos"][k], vel=z["p_vel"][k], angvel=z["p_angvel"][k], quat=z["p_quat"][k],
                               boost=z["p_boost"][k], valid=z["p_valid"][k])
               for k in range(len(z["p_names"]))]
    return SimpleNamespace(time=z["time"], ball_pos=z["ball_pos"], ball_vel=z["ball_vel"],
                           ball_angvel=z["ball_angvel"], ball_valid=z["ball_valid"], players=players)


def _v(vec, nd=0):
    return [round(vec.x, nd), round(vec.y, nd), round(vec.z, nd)] if nd else [round(vec.x), round(vec.y), round(vec.z)]


def bot_controls(car, ball, team):
    """Drive at a point just behind the ball (relative to the goal being attacked)."""
    cs, bs = car.get_state(), ball.get_state()
    cp, bp = np.array([cs.pos.x, cs.pos.y, cs.pos.z]), np.array([bs.pos.x, bs.pos.y, bs.pos.z])
    goal = np.array([0.0, GOAL_Y if team == 0 else -GOAL_Y, 0.0])
    to_goal = goal - bp
    to_goal[2] = 0
    to_goal /= max(np.linalg.norm(to_goal), 1.0)
    target = bp - to_goal * 110
    fwd, right = cs.rot_mat.forward, cs.rot_mat.right
    d = target - cp
    lx = d[0] * fwd.x + d[1] * fwd.y + d[2] * fwd.z
    ly = d[0] * right.x + d[1] * right.y + d[2] * right.z
    ang = math.atan2(ly, lx)
    dist = float(np.hypot(*(bp - cp)[:2]))
    c = rs.CarControls()
    c.throttle = 1.0
    c.steer = float(np.clip(ang * 2.5, -1, 1))
    c.boost = bool(abs(ang) < 0.35 and dist > 900 and cs.boost > 0)
    c.handbrake = bool(abs(ang) > 1.9 and cs.is_on_ground)
    c.jump = bool(cs.is_on_ground and dist < 260 and bp[2] > 220)
    return c


class Session:
    def __init__(self, cache_dir):
        self.cache_dir = cache_dir
        self.replay = None
        self.replay_key = None
        self.sim = None
        self.paused = True
        self.speed = 0.5
        self.controls = rs.CarControls()
        self.params = None
        self.events = []
        self.tick = 0
        self.score = [0, 0]

    # ---------- setup ----------
    def fork(self, key, t, modes, ball=None, speed=None):
        if key != self.replay_key:
            self.replay = load_npz(os.path.join(self.cache_dir, key + ".npz"))
            self.replay_key = key
        self.params = dict(key=key, t=t, modes=modes, ball=ball)
        r = self.replay
        i0 = int(np.clip(np.searchsorted(r.time, t), 0, len(r.time) - 1))
        self.t0 = float(r.time[i0])
        self.sim = Sim()
        self.sim.arena.set_goal_score_callback(self._on_goal)
        self.sim.arena.set_ball_touch_callback(self._on_touch)
        if ball:
            pos, vel = ball["pos"], ball["vel"]
            ang = ball.get("angvel", [0, 0, 0])
        else:
            pos, vel, ang = r.ball_pos[i0], r.ball_vel[i0], r.ball_angvel[i0]
        self.sim.set_ball(pos, vel, ang)
        self.modes = {}
        for k, p in enumerate(r.players):
            mode = modes.get(str(k), "ghost")
            if mode == "removed" or not p.valid[max(0, i0 - 3):i0 + 4].any():
                continue
            car = self.sim.add_car(k, p.team, p.hitbox)
            self.modes[k] = mode
            st = _interp_player(p, r, self.t0)
            if st is not None and not np.isnan(st[0]).any():
                self.sim.set_car(k, st[0], st[1], st[2], st[3], (st[4] if not np.isnan(st[4]) else 85) / 2.55)
            else:
                car.demolish()
        self.car_owner = {car.id: k for k, car in self.sim.cars.items()}
        # players whose real behaviour no longer happens; while none of them is near the ball,
        # the ball is pinned to the replay so an unchanged game stays identical to it
        self.changed = [k for k in range(len(r.players)) if modes.get(str(k), "ghost") != "ghost"]
        self.ball_locked = ball is None
        self.release_t = None if self.ball_locked else 0.0
        self.nexto_cars = [car.id for k, car in self.sim.cars.items() if self.modes[k] == "nexto"]
        self.nexto_controls = {}
        if self.nexto_cars:
            self.teams = {car.id: r.players[k].team for k, car in self.sim.cars.items()}
            nexto_brain().new_game(self.sim.arena, self.teams)
        self.tick = 0
        self.events = []
        self.goal_scored = False
        self.score = [0, 0]
        if speed is not None:
            self.speed = speed
        self.paused = False

    def reset(self):
        if self.params:
            self.fork(**self.params)

    def _on_goal(self, **kw):
        if not self.goal_scored:
            self.goal_scored = True
            team = kw.get("team")
            team = int(team) if team is not None else None
            if team in (0, 1):
                self.score[team] += 1
            self.events.append({"type": "goal", "team": team, "t": self.tick * TICK})
            self.paused = True

    def _on_touch(self, **kw):
        car = kw.get("car")
        if car is not None:
            self.events.append({"type": "touch", "player": self.car_owner.get(car.id), "t": self.tick * TICK})

    # ---------- live changes ----------
    def set_ball(self, pos=None, vel=None):
        bs = self.sim.arena.ball.get_state()
        if pos is not None:
            bs.pos = rs.Vec(*pos)
        if vel is not None:
            bs.vel = rs.Vec(*vel)
        self.sim.arena.ball.set_state(bs)
        self.goal_scored = False
        self._release()

    def _release(self):
        if self.ball_locked:
            self.ball_locked = False
            self.release_t = self.tick * TICK

    def _changed_car_near_ball(self, t, ball_pos):
        r = self.replay
        for k in self.changed:
            st = _interp_player(r.players[k], r, t)          # where the real car was
            if st is not None and not np.isnan(st[0]).any() and np.linalg.norm(st[0] - ball_pos) < RELEASE_DIST:
                return True
            car = self.sim.cars.get(k)                       # where the simulated car is
            if car is not None and not car.get_state().is_demoed:
                p = car.get_state().pos
                if np.linalg.norm(np.array([p.x, p.y, p.z]) - ball_pos) < RELEASE_DIST:
                    return True
        return False

    def set_controls(self, c):
        ctl = rs.CarControls()
        for k in ("throttle", "steer", "pitch", "yaw", "roll"):
            setattr(ctl, k, float(np.clip(c.get(k, 0.0), -1, 1)))
        for k in ("jump", "boost", "handbrake"):
            setattr(ctl, k, bool(c.get(k, False)))
        self.controls = ctl

    # ---------- stepping ----------
    def replay_time(self):
        return self.t0 + self.tick * TICK

    def step(self, ticks):
        if not self.sim or self.paused:
            return
        r = self.replay
        for _ in range(ticks):
            t = self.replay_time()
            if self.nexto_cars and self.tick % TICK_SKIP == 0:
                for cid, action in nexto_brain().act(self.sim.arena, self.teams, self.nexto_cars).items():
                    self.nexto_controls[cid] = _controls_from_action(action)
            for k, mode in self.modes.items():
                car = self.sim.cars[k]
                if mode == "ghost":
                    st = _interp_player(r.players[k], r, t)
                    if st is None or np.isnan(st[0]).any():
                        if not car.get_state().is_demoed:
                            car.demolish()
                        continue
                    self.sim.set_car(k, st[0], st[1], st[2], st[3], (st[4] if not np.isnan(st[4]) else 85) / 2.55)
                elif mode == "control":
                    car.set_controls(self.controls)
                elif mode == "bot":
                    car.set_controls(bot_controls(car, self.sim.arena.ball, r.players[k].team))
                elif mode == "nexto":
                    car.set_controls(self.nexto_controls.get(car.id, rs.CarControls()))
                else:
                    car.set_controls(rs.CarControls())
            if self.ball_locked:
                b = _interp_ball(r, t)
                if b is None or self._changed_car_near_ball(t, b[0]):
                    self._release()
                else:
                    self.sim.set_ball(*b)
            self.sim.arena.step(1)
            self.tick += 1
            if self.paused:
                break

    # ---------- output ----------
    def state(self, with_prediction=False):
        a = self.sim.arena
        bs = a.ball.get_state()
        cars = []
        for k, car in self.sim.cars.items():
            cs = car.get_state()
            cars.append({"i": k, "p": _v(cs.pos), "f": _v(cs.rot_mat.forward, 3), "u": _v(cs.rot_mat.up, 3),
                         "b": round(cs.boost), "on": bool(car.get_controls().boost and cs.boost > 0 and self.modes[k] != "ghost"),
                         "d": bool(cs.is_demoed), "m": self.modes[k]})
        out = {"type": "state", "t": round(self.tick * TICK, 3), "rt": round(self.replay_time(), 3),
               "paused": self.paused, "locked": self.ball_locked, "rel": self.release_t, "ball": {"p": _v(bs.pos), "f": _v(bs.rot_mat.forward, 3), "u": _v(bs.rot_mat.up, 3),
                                               "v": _v(bs.vel)},
               "cars": cars, "score": self.score,
               "pads": "".join("1" if pad.get_state().is_active else "0" for pad in a.get_boost_pads())}
        if with_prediction:
            out["pred"] = [_v(s.pos) for s in a.get_ball_prediction(60, 8)]   # 60 states, every 8 ticks = 4 s
        if self.events:
            out["events"], self.events = self.events, []
        return out

    def pad_positions(self):
        return [_v(p.get_pos()) + [bool(p.is_big)] for p in self.sim.arena.get_boost_pads()]
