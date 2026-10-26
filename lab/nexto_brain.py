"""Nexto driving cars inside RocketSim.

Nexto is the RLGym bot by Rolv-Arild and Soren (RLBotPack/Necto/Nexto, CC BY-NC-SA 4.0,
personal non-commercial use). Its original files live unmodified in vendor/nexto; this module
only rebuilds the RLGym-style GameState that its observation builder expects, straight from
the RocketSim arena instead of an RLBot game packet.
"""
import os
import sys
import types

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(ROOT, "vendor", "nexto")
TICK_SKIP = 8          # Nexto picks a new action every 8 physics ticks (15 per second)
INV = np.array([-1.0, -1.0, 1.0])


class PhysicsObject:
    def __init__(self, position, linear_velocity, angular_velocity, rot=None):
        self.position = position
        self.linear_velocity = linear_velocity
        self.angular_velocity = angular_velocity
        self._rot = rot          # columns: forward, right, up

    def rotation_mtx(self):
        return self._rot

    def forward(self):
        return self._rot[:, 0]

    def up(self):
        return self._rot[:, 2]


class PlayerData:
    pass


class GameState:
    def __init__(self):
        self.game_type = 0
        self.blue_score = 0
        self.orange_score = 0
        self.last_touch = -1
        self.players = []
        self.ball = None
        self.inverted_ball = None
        self.boost_pads = np.zeros(34)
        self.inverted_boost_pads = np.zeros(34)


def _install_rlgym_compat_shim():
    """nexto_obs.py imports rlgym_compat only for team constants and type names."""
    if "rlgym_compat" in sys.modules:
        return
    pkg = types.ModuleType("rlgym_compat")
    common = types.ModuleType("rlgym_compat.common_values")
    common.BLUE_TEAM, common.ORANGE_TEAM = 0, 1
    gs = types.ModuleType("rlgym_compat.game_state")
    gs.GameState, gs.PlayerData = GameState, PlayerData
    pkg.GameState, pkg.common_values, pkg.game_state = GameState, common, gs
    sys.modules.update({"rlgym_compat": pkg, "rlgym_compat.common_values": common, "rlgym_compat.game_state": gs})


def _v(vec):
    return np.array([vec.x, vec.y, vec.z], dtype=np.float64)


class NextoBrain:
    def __init__(self, beta=1.0):
        _install_rlgym_compat_shim()
        if VENDOR not in sys.path:
            sys.path.insert(0, VENDOR)
        from agent import Agent                       # vendor/nexto/agent.py (loads torch + the model)
        from nexto_obs import NextoObsBuilder, BOOST_LOCATIONS
        self.agent = Agent()
        self._builder_cls = NextoObsBuilder
        self._boost_locations = np.array(BOOST_LOCATIONS)
        self.beta = beta                              # 1 = Nexto's best action (deterministic)

    def new_game(self, arena, teams):
        """teams: RocketSim car id -> 0 (blue) / 1 (orange)."""
        pads = arena.get_boost_pads()
        xy = np.array([[p.get_pos().x, p.get_pos().y] for p in pads])
        # RocketSim's pad order -> the RLBot order Nexto was trained with
        self._pad_index = [int(np.argmin(np.linalg.norm(xy - loc[:2], axis=1))) for loc in self._boost_locations]
        self.obs_builder = self._builder_cls()
        self.prev_actions = {}
        self.obs_builder.reset(self.game_state(arena, teams))

    def game_state(self, arena, teams):
        st = GameState()
        bs = arena.ball.get_state()
        st.ball = PhysicsObject(_v(bs.pos), _v(bs.vel), _v(bs.ang_vel))
        st.inverted_ball = PhysicsObject(st.ball.position * INV, st.ball.linear_velocity * INV,
                                         st.ball.angular_velocity * INV)
        pads = arena.get_boost_pads()
        st.boost_pads = np.array([1.0 if pads[i].get_state().is_active else 0.0 for i in self._pad_index])
        st.inverted_boost_pads = st.boost_pads[::-1].copy()
        for car in arena.get_cars():
            cs = car.get_state()
            rot = np.column_stack([_v(cs.rot_mat.forward), _v(cs.rot_mat.right), _v(cs.rot_mat.up)])
            p = PlayerData()
            p.car_id = car.id
            p.team_num = teams.get(car.id, 0)
            p.is_demoed = bool(cs.is_demoed)
            p.on_ground = bool(cs.is_on_ground)
            p.ball_touched = False
            p.has_jump = not cs.has_jumped
            p.has_flip = bool(cs.is_on_ground or (not cs.has_double_jumped and not cs.has_flipped
                                                  and cs.air_time_since_jump < 1.25))
            p.boost_amount = cs.boost / 100.0
            p.car_data = PhysicsObject(_v(cs.pos), _v(cs.vel), _v(cs.ang_vel), rot)
            p.inverted_car_data = PhysicsObject(_v(cs.pos) * INV, _v(cs.vel) * INV, _v(cs.ang_vel) * INV,
                                                rot * INV[:, None])
            st.players.append(p)
        return st

    def act(self, arena, teams, car_ids):
        """New 8-value actions (throttle, steer, pitch, yaw, roll, jump, boost, handbrake) for each car id."""
        return self._act(self.obs_builder, self.game_state(arena, teams), car_ids, self.prev_actions)

    def act_json(self, data):
        """Same, for a game state sent by the BakkesMod plugin (real game, game units, rad/s):
        {"session": str, "ball": {pos, vel, angvel}, "cars": [{id, team, pos, vel, angvel, fwd, right, up,
         boost 0..100, ground, flip}], "nexto": [ids]}"""
        if not hasattr(self, "_json_builder"):
            self._json_builder = self._builder_cls()
            self._json_prev = {}
        session = data.get("session", "")
        if session != getattr(self, "_json_session", None):
            self._json_session, self._json_prev = session, {}
        st = GameState()
        b = data["ball"]
        st.ball = PhysicsObject(np.array(b["pos"], float), np.array(b["vel"], float), np.array(b["angvel"], float))
        st.inverted_ball = PhysicsObject(st.ball.position * INV, st.ball.linear_velocity * INV,
                                         st.ball.angular_velocity * INV)
        st.boost_pads = np.ones(34)
        st.inverted_boost_pads = np.ones(34)
        for c in data["cars"]:
            rot = np.column_stack([np.array(c["fwd"], float), np.array(c["right"], float), np.array(c["up"], float)])
            pos, vel, ang = (np.array(c[k], float) for k in ("pos", "vel", "angvel"))
            p = PlayerData()
            p.car_id, p.team_num = int(c["id"]), int(c["team"])
            p.is_demoed, p.ball_touched = False, False
            p.on_ground, p.has_flip = bool(c["ground"]), bool(c["flip"])
            p.has_jump = p.on_ground
            p.boost_amount = float(c["boost"]) / 100.0
            p.car_data = PhysicsObject(pos, vel, ang, rot)
            p.inverted_car_data = PhysicsObject(pos * INV, vel * INV, ang * INV, rot * INV[:, None])
            st.players.append(p)
        if not hasattr(self._json_builder, "demo_timers") or self._json_builder.demo_timers is None:
            self._json_builder.reset(st)
        return self._act(self._json_builder, st, [int(i) for i in data.get("nexto", [])], self._json_prev)

    def _act(self, builder, state, car_ids, prev):
        everyone = list(state.players)
        out = {}
        for cid in car_ids:
            me = next((p for p in everyone if p.car_id == cid), None)
            if me is None or me.is_demoed:
                continue
            mates = [p for p in everyone if p.team_num == me.team_num and p is not me]
            opps = [p for p in everyone if p.team_num != me.team_num]
            state.players = [me] + mates + opps
            obs = builder.build_obs(me, state, prev.get(cid, np.zeros(8)))
            action, _ = self.agent.act(obs, self.beta)
            prev[cid] = action
            out[cid] = action
        return out
