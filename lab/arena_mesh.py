"""Procedurally build an approximate Soccar (standard) arena for RocketSim.

RocketSim already provides floor, ceiling and side-wall planes. This file supplies
everything else from public RLBot dimensions: back walls with goal openings,
45 degree corners, curved floor/ceiling ramps, and the goal boxes.
Output is RocketSim's .cmf format: int32 nTris, int32 nVerts, nTris*3 int32, nVerts*3 float32.
"""
import math
import os
import struct
import sys

X_EXT, Y_EXT, H = 4096.0, 5120.0, 2044.0
CORNER = 8064.0                     # |x| + |y| = 8064 on the diagonal walls
GOAL_HW, GOAL_H, GOAL_BACK = 892.755, 642.775, 6000.0
R_FLOOR, R_CEIL = 256.0, 512.0      # ramp radii, tuned against real replays
ARC_STEPS = 10
UU_TO_FILE = 1 / 50                 # rocketsim 2.2.1 reads .cmf vertices in Bullet units (found by probing)


class Mesh:
    def __init__(self):
        self.verts, self.tris = [], []

    def tri(self, a, b, c, inward):
        n = _cross(_sub(b, a), _sub(c, a))
        cen = [(a[i] + b[i] + c[i]) / 3 for i in range(3)]
        if _dot(n, _sub(inward, cen)) < 0:
            b, c = c, b
        i = len(self.verts)
        self.verts += [a, b, c]
        self.tris.append((i, i + 1, i + 2))

    def quad(self, a, b, c, d, inward):
        self.tri(a, b, c, inward)
        self.tri(a, c, d, inward)

    def write(self, path):
        with open(path, "wb") as f:
            f.write(struct.pack("<ii", len(self.tris), len(self.verts)))
            for t in self.tris:
                f.write(struct.pack("<3i", *t))
            for v in self.verts:
                f.write(struct.pack("<3f", *(c * UU_TO_FILE for c in v)))


def _sub(a, b): return [a[i] - b[i] for i in range(3)]
def _dot(a, b): return sum(a[i] * b[i] for i in range(3))
def _cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def _wall_lines():
    """The 8 perimeter walls as (outward unit normal, offset) with n.p = offset, counter-clockwise."""
    s = 1 / math.sqrt(2)
    return [
        ((1, 0), X_EXT), ((s, s), CORNER * s), ((0, 1), Y_EXT), ((-s, s), CORNER * s),
        ((-1, 0), X_EXT), ((-s, -s), CORNER * s), ((0, -1), Y_EXT), ((s, -s), CORNER * s),
    ]


def _ring(inset):
    """Octagon vertices for the perimeter moved inward by `inset`; vertex i joins wall i and i+1."""
    lines = _wall_lines()
    pts = []
    for i in range(8):
        (n1, d1), (n2, d2) = lines[i], lines[(i + 1) % 8]
        d1, d2 = d1 - inset, d2 - inset
        det = n1[0] * n2[1] - n1[1] * n2[0]
        pts.append(((d1 * n2[1] - d2 * n1[1]) / det, (n1[0] * d2 - n2[0] * d1) / det))
    return pts


def _profile():
    """(inset, z) samples from floor, up the wall, to the ceiling."""
    prof = []
    for k in range(ARC_STEPS + 1):          # floor ramp: (R,0) -> (0,R)
        a = math.pi / 2 * k / ARC_STEPS
        prof.append((R_FLOOR - R_FLOOR * math.sin(a), R_FLOOR - R_FLOOR * math.cos(a)))
    prof.append((0.0, GOAL_H))
    prof.append((0.0, H - R_CEIL))
    for k in range(1, ARC_STEPS + 1):       # ceiling ramp: (0,H-R) -> (R,H)
        a = math.pi / 2 * k / ARC_STEPS
        prof.append((R_CEIL - R_CEIL * math.cos(a), H - R_CEIL + R_CEIL * math.sin(a)))
    return prof


def build_arena(side_walls=False):
    """side_walls=True adds the flat side walls too (for drawing; RocketSim has its own planes there)."""
    m = Mesh()
    center = [0.0, 0.0, H / 2]
    prof = _profile()
    rings = [_ring(d) for d, _ in prof]
    for k in range(len(prof) - 1):
        z0, z1 = prof[k][1], prof[k + 1][1]
        vertical = prof[k][0] == 0 and prof[k + 1][0] == 0
        for e in range(8):
            a0, b0 = rings[k][e - 1], rings[k][e]      # wall e spans vertex e-1 -> e
            a1, b1 = rings[k + 1][e - 1], rings[k + 1][e]
            if e in (0, 4) and vertical and not side_walls:
                continue                               # RocketSim's side-wall planes cover this
            if e in (2, 6):
                # back wall: split at the goal posts, skip the mouth below the crossbar
                post = GOAL_HW if a0[0] > 0 else -GOAL_HW
                x0 = [a0[0], post, -post, b0[0]]
                x1 = [a1[0], post, -post, b1[0]]
                y0, y1 = a0[1], a1[1]
                for j in range(3):
                    if j == 1 and z1 <= GOAL_H + 1e-3:
                        continue
                    m.quad([x0[j], y0, z0], [x0[j + 1], y0, z0], [x1[j + 1], y1, z1], [x1[j], y1, z1], center)
                continue
            m.quad([a0[0], a0[1], z0], [b0[0], b0[1], z0], [b1[0], b1[1], z1], [a1[0], a1[1], z1], center)
    return m


def build_goals():
    m = Mesh()
    for sy in (1, -1):
        y0, y1 = sy * Y_EXT, sy * GOAL_BACK
        inside = [0.0, sy * (Y_EXT + GOAL_BACK) / 2, GOAL_H / 2]
        for sx in (1, -1):  # side walls of the goal
            x = sx * GOAL_HW
            m.quad([x, y0, 0], [x, y1, 0], [x, y1, GOAL_H], [x, y0, GOAL_H], inside)
        m.quad([-GOAL_HW, y0, GOAL_H], [GOAL_HW, y0, GOAL_H], [GOAL_HW, y1, GOAL_H], [-GOAL_HW, y1, GOAL_H], inside)
        m.quad([-GOAL_HW, y1, 0], [GOAL_HW, y1, 0], [GOAL_HW, y1, GOAL_H], [-GOAL_HW, y1, GOAL_H], inside)
    return m


def write_meshes(root):
    os.makedirs(os.path.join(root, "soccar"), exist_ok=True)
    arena, goals = build_arena(), build_goals()
    arena.write(os.path.join(root, "soccar", "approx_arena.cmf"))
    goals.write(os.path.join(root, "soccar", "approx_goals.cmf"))
    return len(arena.tris), len(goals.tris)


def visual_mesh():
    """Arena and goal triangles in game units, flattened, for the 3D viewer."""
    flat = lambda m: [round(c, 1) for t in m.tris for i in t for c in m.verts[i]]
    return {"arena": flat(build_arena(side_walls=True)), "goals": flat(build_goals()),
            "dims": {"x": X_EXT, "y": Y_EXT, "h": H, "corner": CORNER, "goal_hw": GOAL_HW, "goal_h": GOAL_H,
                     "goal_back": GOAL_BACK}}


if __name__ == "__main__":
    print(write_meshes(sys.argv[1]))
