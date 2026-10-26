"""What-if engine: rewind a replay to time t0, change something, simulate forward.

Other players are "ghosts": every tick their car is placed where it really was
(interpolated from the replay), so they can still hit the ball, but they do not react
to the new situation.
"""
from dataclasses import dataclass

import numpy as np

from .sim import Sim, TICK

BLUE, ORANGE = 0, 1


@dataclass
class Outcome:
    times: np.ndarray        # seconds after t0
    ball: np.ndarray         # (T, 3) simulated ball path
    goal_team: int | None    # team that scored (0 blue / 1 orange), None if no goal
    goal_after: float | None
    touches: list            # [(seconds after t0, player name)]

    def describe(self):
        if self.goal_team is None:
            return f"no goal within {self.times[-1]:.1f}s"
        return f"GOAL for {'blue' if self.goal_team == BLUE else 'orange'} after {self.goal_after:.2f}s"


def frame_at(r, t):
    return int(np.clip(np.searchsorted(r.time, t), 0, len(r.time) - 1))


def _interp_player(p, r, t):
    i = np.searchsorted(r.time, t)
    if i <= 0 or i >= len(r.time) or not (p.valid[i - 1] and p.valid[i]):
        j = min(max(i, 0), len(r.time) - 1)
        return (p.pos[j], p.vel[j], p.angvel[j], p.quat[j], p.boost[j]) if p.valid[j] else None
    a = (t - r.time[i - 1]) / max(r.time[i] - r.time[i - 1], 1e-6)
    lerp = lambda x: x[i - 1] * (1 - a) + x[i] * a
    q0, q1 = p.quat[i - 1], p.quat[i]
    if np.dot(q0, q1) < 0:
        q1 = -q1
    q = q0 * (1 - a) + q1 * a
    return lerp(p.pos), lerp(p.vel), lerp(p.angvel), q / np.linalg.norm(q), lerp(p.boost)


def simulate(r, t0, remove=(), horizon=4.0, ghosts=True, ball_override=None, ghost_shift=0.0):
    """Roll the game forward from replay time t0 without the players in `remove`.

    ghost_shift moves the ghost cars in time relative to the ball (used for robustness checks).
    """
    sim = Sim()
    i0 = frame_at(r, t0)
    t0 = r.time[i0]
    pos, vel, ang = ball_override or (r.ball_pos[i0], r.ball_vel[i0], r.ball_angvel[i0])
    sim.set_ball(pos, vel, ang)

    removed = {n.lower() for n in remove}
    active = []
    if ghosts:
        for k, p in enumerate(r.players):
            if p.name.lower() in removed or p.team not in (BLUE, ORANGE):
                continue
            sim.add_car(k, p.team, p.hitbox)
            active.append((k, p))
    car_owner = {sim.cars[k].id: p.name for k, p in active}

    n_ticks = int(round(horizon / TICK))
    path = np.zeros((n_ticks + 1, 3))
    path[0] = sim.ball_pos()
    goal_team = goal_after = None
    for tick in range(1, n_ticks + 1):
        t = t0 + (tick - 1) * TICK + ghost_shift
        for k, p in active:
            st = _interp_player(p, r, t)
            car = sim.cars[k]
            if st is None or np.isnan(st[0]).any():
                if not car.get_state().is_demoed:
                    car.demolish()
                continue
            sim.set_car(k, st[0], st[1], st[2], st[3], (st[4] if not np.isnan(st[4]) else 85) / 2.55)
        sim.arena.step(1)
        path[tick] = sim.ball_pos()
        if sim.goals:
            goal_team, goal_after = sim.goals[0][1], tick * TICK
            path = path[:tick + 1]
            break
    start_tick = sim.arena.tick_count - (len(path) - 1)
    touches = [((tk - start_tick) * TICK, car_owner.get(cid, "?")) for tk, cid in sim.touches]
    return Outcome(np.arange(len(path)) * TICK, path, goal_team, goal_after, touches)


def ensemble(r, t0s, remove=(), horizon=4.0, shifts=(-1 / 60, 0.0, 1 / 60)):
    """Run several slightly perturbed versions; returns (runs, P(blue goal), P(orange goal))."""
    runs = [simulate(r, t0, remove, horizon, ghost_shift=s) for t0 in t0s for s in shifts]
    p_blue = np.mean([o.goal_team == BLUE for o in runs])
    p_orange = np.mean([o.goal_team == ORANGE for o in runs])
    return runs, p_blue, p_orange


def real_outcome(r, t0, horizon=4.0):
    """What actually happened in the replay over the same window."""
    i0 = frame_at(r, t0)
    i1 = frame_at(r, r.time[i0] + horizon)
    goal = next((g for g in r.goals if i0 <= g["frame"] <= i1 + 1), None)
    if goal:
        i1 = min(i1, goal["frame"])
    times = r.time[i0:i1 + 1] - r.time[i0]
    team = None if goal is None else (BLUE if goal["scoring_team_is_team_0"] else ORANGE)
    return Outcome(times, r.ball_pos[i0:i1 + 1], team, None if goal is None else goal["time"] - r.time[i0], [])


def attribute_touches(r, max_dist=400.0):
    """Replay touch events only carry the team; pick the closest car of that team."""
    out = []
    for ev in r.touches:
        f = ev["frame"]
        team = BLUE if ev["team_is_team_0"] else ORANGE
        best, best_d = None, max_dist
        for p in r.players:
            if p.team != team:
                continue
            for g in (f - 1, f, f + 1):
                if 0 <= g < len(r.time) and p.valid[g] and r.ball_valid[g]:
                    d = np.linalg.norm(p.pos[g] - r.ball_pos[g])
                    if d < best_d:
                        best, best_d = p.name, d
        out.append({"frame": f, "time": ev["time"], "team": team, "player": best, "dist": best_d})
    return out


def overtime_start(r):
    """Frame where overtime starts (clock hit 0 and then counts up), or None."""
    zero = np.where(r.seconds_remaining == 0)[0]
    if len(zero) and (r.seconds_remaining[zero[0]:] > 0).any():
        return int(zero[0])
    return None


def clock(r, t):
    i = frame_at(r, t)
    s = int(r.seconds_remaining[i])
    ot = overtime_start(r)
    prefix = "OT +" if ot is not None and i > ot else ""
    return f"{prefix}{s // 60}:{s % 60:02d}"
