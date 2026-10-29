"""Cuts the presentation out of the recording: the scenes tourner.py wrote (rushes/scenes.json), without the loading in
between, joined by short cross fades, faded in and out, as an MP4 in the Videos folder.

    .venv/bin/python video/monter.py [recording]

Without a recording given: the one scenes.json names, else the newest video of Spectacle's folder. A recording you
made yourself (tourner.py --manuel) starts with the magenta flash of rlab_flash: the scenes are counted from it
(only the flash is looked for, on tiny frames).
"""
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SCENES = Path(os.environ.get("RLAB_SCENES", HERE / "rushes" / "scenes.json"))
VIDEOS = Path(subprocess.run(["xdg-user-dir", "VIDEOS"], capture_output=True, text=True).stdout.strip() or Path.home() / "Videos")
OUT = Path(os.environ.get("RLAB_OUT", VIDEOS / "ReplayLab_presentation.mp4"))
FADE = 0.4         # seconds of cross fade between two scenes
FPS = 30         # Spectacle records 30 to 60 frames a second, unevenly: 30 even ones look smooth


def newest_recording():
    found = [p for p in VIDEOS.rglob("*") if p.suffix.lower() in (".webm", ".mp4", ".mkv") and p != OUT]
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


def flash_time(video):
    """When the magenta flash starts, in the recording's time (to 1/30 s)."""
    w, h = 16, 9
    proc = subprocess.Popen(["ffmpeg", "-v", "error", "-i", str(video), "-vf", f"fps=30,scale={w}:{h}", "-f", "rawvideo",
                             "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
    size, index = w * h * 3, 0
    try:
        while True:
            frame = proc.stdout.read(size)
            if len(frame) < size:
                return None
            pixels = [frame[i:i + 3] for i in range(0, size, 3)]
            magenta = sum(1 for r, g, b in pixels if r > 200 and g < 70 and b > 200)
            if magenta > 0.7 * len(pixels):
                return index / 30.0
            index += 1
    finally:
        proc.kill()


def main():
    plan = json.loads(SCENES.read_text())
    video = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(plan["video"]) if plan.get("video") else newest_recording()
    if not video or not video.exists():
        sys.exit("pas d'enregistrement trouve")
    start = 0.0
    if plan.get("repere") == "flash":
        start = flash_time(video)
        if start is None:
            sys.exit(f"flash magenta introuvable dans {video}")
    # a scene is one piece, or several ("morceaux": the take-over scene keeps its frozen start and the try that went in)
    pieces = []
    for s in plan["scenes"]:
        for a, b in s.get("morceaux") or [[s["debut"], s.get("fin", s["debut"])]]:
            if b - a > 2 * FADE:
                pieces.append((s["nom"], start + a, start + b))
    print(f"{video.name}: debut a {start:.2f} s, {len(pieces)} morceaux")
    parts, chains = [], []
    for i, (name, a, b) in enumerate(pieces):
        parts.append(b - a)
        chains.append(f"[0:v]trim=start={a:.3f}:end={b:.3f},setpts=PTS-STARTPTS,fps={FPS},format=yuv420p,settb=AVTB[s{i}]")
        print(f"  {name:14s} {a:7.2f} -> {b:7.2f} s ({b - a:4.1f} s)")
    last, offset = "s0", 0.0
    for i in range(1, len(pieces)):
        offset += parts[i - 1] - FADE
        chains.append(f"[{last}][s{i}]xfade=transition=fade:duration={FADE}:offset={offset:.3f}[x{i}]")
        last = f"x{i}"
    total = sum(parts) - FADE * (len(pieces) - 1)
    chains.append(f"[{last}]fade=t=in:st=0:d=0.6,fade=t=out:st={total - 0.8:.3f}:d=0.8[v]")
    subprocess.run(["ffmpeg", "-v", "error", "-stats", "-y", "-i", str(video), "-filter_complex", ";".join(chains), "-map", "[v]",
                    "-an", "-c:v", "h264_nvenc", "-preset", "p7", "-rc", "vbr", "-cq", "19", "-b:v", "0", "-movflags", "+faststart",
                    str(OUT)], check=True)
    print(f"video: {OUT} ({total:.0f} s)")


main()
