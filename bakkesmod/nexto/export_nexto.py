"""Export Nexto's weights for the Replay Lab plugin, and a parity test set.

Nexto (Rolv-Arild & Soren, RLBotPack, CC BY-NC-SA 4.0) runs inside the plugin with no Python:
nexto.cpp reimplements its observation builder and its network. This script writes:
  nexto.bin       the network weights (float32, little endian), in the order nexto.cpp reads them
  nexto_test.json game states built from a replay, with the observations and action scores of the original model
Usage: python export_nexto.py <replay blob from examples/dump> <out dir>
"""
import json
import os
import struct
import sys

import numpy as np
import torch

LAB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "lab")
sys.path.insert(0, os.path.dirname(LAB))
from lab.nexto_brain import GameState, PhysicsObject, PlayerData, INV, _install_rlgym_compat_shim, VENDOR  # noqa: E402

_install_rlgym_compat_shim()
sys.path.insert(0, VENDOR)
from agent import Agent  # noqa: E402
from nexto_obs import NextoObsBuilder  # noqa: E402

TENSORS = [
    "net.earl.query_preprocess.0.weight", "net.earl.query_preprocess.0.bias",
    "net.earl.query_preprocess.2.weight", "net.earl.query_preprocess.2.bias",
    "net.earl.key_value_preprocess.0.weight", "net.earl.key_value_preprocess.0.bias",
    "net.earl.key_value_preprocess.2.weight", "net.earl.key_value_preprocess.2.bias",
]
for b in range(2):
    TENSORS += [f"net.earl.blocks.{b}.{n}" for n in (
        "attention.in_proj_weight", "attention.in_proj_bias", "attention.out_proj.weight", "attention.out_proj.bias",
        "linear1.weight", "linear1.bias", "linear2.weight", "linear2.bias",
        "norm1.weight", "norm1.bias", "norm2.weight", "norm2.bias", "norm3.weight", "norm3.bias")]
TENSORS += ["net.output.net.0.weight", "net.output.net.0.bias", "net.output.net.2.weight", "net.output.net.2.bias",
            "net.output.net.4.weight", "net.output.net.4.bias",
            "net.output.emb_convertor.weight", "net.output.emb_convertor.bias"]


def export_weights(model, path):
    sd = model.state_dict()
    with open(path, "wb") as f:
        f.write(b"NXTO")
        f.write(struct.pack("<I", len(TENSORS)))
        for name in TENSORS:
            t = sd[name].detach().float().contiguous().numpy()
            f.write(struct.pack("<I", t.size))
            f.write(t.astype("<f4").tobytes())
    print("poids:", path, os.path.getsize(path), "octets")


def read_blob(path):
    d = open(path, "rb").read()
    o = 8

    def get(fmt):
        nonlocal o
        v = struct.unpack_from("<" + fmt, d, o)
        o += struct.calcsize("<" + fmt)
        return v

    def s():
        nonlocal o
        n, = get("H")
        r = d[o:o + n]
        o += n
        return r

    s(); s()
    n, P = get("II")
    teams = []
    for _ in range(P):
        s()
        teams.append(get("B")[0])
    frames = []
    for _ in range(n):
        get("fi")
        ball = get("B13f")
        cars = []
        for _ in range(P):
            cars.append(get("B13f") + get("BB"))
        frames.append((ball, cars))
    return teams, frames


def axes(q):
    x, y, z, w = q
    f = np.array([1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)])
    r = np.array([2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)])
    u = np.array([2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)])
    return f, r, u


def main():
    blob, out = sys.argv[1], sys.argv[2]
    agent = Agent()
    export_weights(agent.actor, os.path.join(out, "nexto.bin"))
    table = agent.make_lookup_table()
    teams, frames = read_blob(blob)
    rng = np.random.default_rng(7)
    samples = []
    picks = [i for i in range(0, len(frames), 97) if frames[i][0][0] and all(c[0] for c in frames[i][1])][:40]
    for n_i, i in enumerate(picks):
        ball, cars = frames[i]
        st = GameState()
        st.ball = PhysicsObject(np.array(ball[1:4]), np.array(ball[8:11]), np.array(ball[11:14]))
        st.inverted_ball = PhysicsObject(st.ball.position * INV, st.ball.linear_velocity * INV, st.ball.angular_velocity * INV)
        st.boost_pads = (rng.random(34) > 0.3).astype(float)
        st.inverted_boost_pads = st.boost_pads[::-1].copy()
        raw_players = []
        for k, c in enumerate(cars):
            f, r, u = axes(c[4:8])
            rot = np.column_stack([f, r, u])
            p = PlayerData()
            p.car_id, p.team_num = k, teams[k] if n_i % 2 == 0 else 1 - teams[k]
            p.is_demoed = False
            p.on_ground = bool(c[3] < 20)
            p.ball_touched = False
            p.has_jump = p.on_ground
            p.has_flip = bool(rng.random() > 0.3)
            p.boost_amount = c[14] / 255.0
            pos, vel, ang = np.array(c[1:4]), np.array(c[8:11]), np.array(c[11:14])
            p.car_data = PhysicsObject(pos, vel, ang, rot)
            p.inverted_car_data = PhysicsObject(pos * INV, vel * INV, ang * INV, rot * INV[:, None])
            st.players.append(p)
            raw_players.append({"team": p.team_num, "pos": pos.tolist(), "vel": vel.tolist(), "ang": ang.tolist(),
                                "fwd": f.tolist(), "right": r.tolist(), "up": u.tolist(), "boost": p.boost_amount,
                                "demoed": 0, "ground": int(p.on_ground), "flip": int(p.has_flip)})
        for me_k in range(len(cars)):
            me = st.players[me_k]
            mates = [p for p in st.players if p.team_num == me.team_num and p is not me]
            opps = [p for p in st.players if p.team_num != me.team_num]
            order = [me] + mates + opps
            st2 = GameState()
            st2.__dict__.update(st.__dict__)
            st2.players = order
            prev = table[rng.integers(len(table))].astype(float)
            builder = NextoObsBuilder()
            builder.reset(st2)
            q, kv, m = builder.build_obs(me, st2, prev)
            with torch.no_grad():
                scores, _ = agent.actor(tuple(torch.from_numpy(a).float() for a in (q, kv, m)))
            logits = scores.reshape(-1).numpy()
            samples.append({
                "players": [raw_players[p.car_id] for p in order], "ball": {"pos": list(ball[1:4]), "vel": list(ball[8:11]),
                                                                           "ang": list(ball[11:14])},
                "pads": st.boost_pads.tolist(), "prev": prev.tolist(),
                "q": q.reshape(-1).tolist(), "kv": kv.reshape(-1).tolist(), "mask": m.reshape(-1).tolist(),
                "logits": logits.tolist(), "action": int(np.argmax(logits)),
            })
    json.dump({"table": table.tolist(), "samples": samples}, open(os.path.join(out, "nexto_test.json"), "w"))
    print("tests:", len(samples), "observations")


if __name__ == "__main__":
    main()
