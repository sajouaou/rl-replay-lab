"""Films a presentation of Replay Lab: drives the plugin over RCON (scenes, captions, cameras) while Spectacle records
the screen, and writes when each scene starts and ends (scenes.json) for monter.py to cut the video.

    .venv/bin/python video/tourner.py            the whole video
    .venv/bin/python video/tourner.py --essai    checks first: captions, game HUD, a 3 s recording

The game must stay in front and nobody touches it: the script waits for the plugin to say the game is in front.
"""
import asyncio
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import aiohttp

BM = Path(os.environ.get("RL_PREFIX", "/mnt/data/SteamLibrary/steamapps/compatdata/252950/pfx")) / "drive_c/users/steamuser/AppData/Roaming/bakkesmod/bakkesmod"
LOG = BM / "bakkesmod.log"
REPLAY = os.environ.get("RL_REPLAY", "15B237DC11F1C73701F2B7B487D06070")   # a 2v2 Hoops replay (hoopsStreet_p): players 1-2 blue, 3-4 orange
OUT = Path(__file__).parent / "rushes"
# player numbers for rlab_role (1..4) and roles
BLUE1, BLUE2, ORANGE1, ORANGE2 = 1, 2, 3, 4
GHOST, NEXTO = 1, 5
CAM_PLAYER, CAM_AUTO = 1, 3


class Log:
    """New lines of BakkesMod's log since the last look."""

    def __init__(self):
        self.pos = LOG.stat().st_size

    def new(self):
        with open(LOG, "rb") as f:
            f.seek(self.pos)
            data = f.read()
            self.pos = f.tell()
        return data.decode("utf-8", "replace").splitlines()

    def last_focus(self):
        text = LOG.read_bytes().decode("utf-8", "replace")
        found = re.findall(r"jeu (au premier plan|en arriere-plan)", text)
        return found[-1] if found else ""


class Director:
    def __init__(self, ws):
        self.ws = ws
        self.log = Log()
        self.t0 = None
        self.scenes = []
        self.aborted = False

    async def cmd(self, *commands, pause=0.15):
        for c in commands:
            await self.ws.send_str(c)
            await asyncio.sleep(pause)

    async def wait_for(self, pattern, timeout=30.0):
        end = time.time() + timeout
        while time.time() < end:
            for line in self.log.new():
                if re.search(pattern, line):
                    return line
            await asyncio.sleep(0.2)
        raise TimeoutError(f"pas vu dans le journal: {pattern}")

    async def wait_front(self, timeout=900.0):
        """The game in front for 5 s in a row (not a quick Alt+Tab)."""
        end, since = time.time() + timeout, None
        while time.time() < end:
            if self.log.last_focus() == "au premier plan":
                since = since or time.time()
                if time.time() - since >= 5.0:
                    return
            else:
                since = None
            await asyncio.sleep(0.5)
        raise TimeoutError("le jeu n'est pas passe au premier plan")

    async def watchdog(self):
        """Only the game is filmed: in the background, the recording stops at once and is thrown away."""
        while True:
            await asyncio.sleep(0.3)
            if self.log.last_focus() != "au premier plan":
                spectacle_record()
                self.aborted = True
                print("jeu passe en arriere-plan: enregistrement arrete", flush=True)
                return

    def now(self):
        return time.time() - self.t0

    def end(self):
        """The scene being filmed ends here (what follows, loading the next one, is cut)."""
        if self.scenes and "fin" not in self.scenes[-1]:
            self.scenes[-1]["fin"] = self.now()

    def scene(self, name):
        self.end()
        self.scenes.append({"nom": name, "debut": self.now()})
        print(f"{self.now():6.1f} s  {name}", flush=True)

    async def open(self, frame):
        await self.cmd(f"rlab_open {REPLAY} {frame}")
        await self.wait_for(r"Replay charge")
        await asyncio.sleep(2.0)   # cars set up (looks: 3 s more after a reload)


def spectacle_record():
    """Spectacle's screen recording: the first call starts it, the next one stops it."""
    subprocess.run(["qdbus6", "org.kde.Spectacle", "/", "org.kde.Spectacle.RecordScreen", "0"], check=True)


def screenshot(path):
    subprocess.run(["spectacle", "-b", "-n", "-f", "-o", str(path)], check=False)


async def essai(d):
    """Captions with accents, the game's HUD hidden, a 3 s recording (thrown away once checked)."""
    OUT.mkdir(exist_ok=True)
    await d.cmd(f"rlab_open {REPLAY} 1180")
    await d.wait_for(r"Replay charge")
    await asyncio.sleep(3.0)
    await d.cmd("rlab_cinema 1", "rlab_legende Scénario alternatif | à 0 seconde, été, où, ç", "rlab_titre REPLAY LAB | Prêt à tourner ?")
    await asyncio.sleep(1.0)
    screenshot(OUT / "essai_legendes.png")
    await d.cmd("rlab_titre", "rlab_legende", "rlab_hud")
    await asyncio.sleep(1.0)
    screenshot(OUT / "essai_sans_hud.png")
    await d.cmd("rlab_hud")
    monitor = subprocess.Popen(["dbus-monitor", "type='signal',interface='org.kde.Spectacle'"], stdout=subprocess.PIPE, text=True)
    if d.log.last_focus() != "au premier plan":
        sys.exit("le jeu n'est plus au premier plan: pas d'essai d'enregistrement")
    spectacle_record()
    await asyncio.sleep(3.0)
    spectacle_record()
    await asyncio.sleep(4.0)
    monitor.terminate()
    out = monitor.stdout.read()
    print(out)
    for f in re.findall(r'string "(/[^"]+)"', out):
        probe = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration:stream=codec_name,width,height,avg_frame_rate",
                                "-of", "compact", f], capture_output=True, text=True).stdout
        print("essai d'enregistrement:", f, probe)
        Path(f).unlink(missing_ok=True)


async def film(d):
    # ---- set up, before the recording
    await d.cmd("rlab_cinema 1", "rlab_tableau 1", "rlab_noms 1", "rlab_cam_joueur 1", "rlab_speed 1", "rlab_vitesse 1",
                "rlab_titre", "rlab_legende")
    await d.open(1180)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_capture")   # director camera, playing
    await asyncio.sleep(2.0)
    if d.log.last_focus() != "au premier plan":
        sys.exit("le jeu n'est plus au premier plan: rien n'est filme")
    monitor = subprocess.Popen(["dbus-monitor", "type='signal',interface='org.kde.Spectacle'"], stdout=subprocess.PIPE, text=True)
    spectacle_record()
    d.t0 = time.time()
    guard = asyncio.create_task(d.watchdog())
    try:
        await scenes(d)
    finally:
        await asyncio.sleep(3.0)
        monitor.terminate()
        saved = re.findall(r'string "(/[^"]+)"', monitor.stdout.read())
        if d.aborted:
            for f in saved:
                Path(f).unlink(missing_ok=True)
            sys.exit("tournage interrompu (le jeu est passe en arriere-plan), enregistrement supprime")
    OUT.mkdir(exist_ok=True)
    (OUT / "scenes.json").write_text(json.dumps({"video": saved[-1] if saved else "", "scenes": d.scenes}, indent=2, ensure_ascii=False))
    print("video:", saved, "scenes ecrites dans", OUT / "scenes.json")


async def scenes(d):
    await asyncio.sleep(1.0)

    # ---- 1. title over the match playing
    d.scene("titre")
    await d.cmd("rlab_titre REPLAY LAB | Rejoue tes replays dans Rocket League... et change l'histoire")
    await asyncio.sleep(7.0)
    await d.cmd("rlab_titre")

    # ---- 2. its stadium, its mode
    d.scene("stade")
    await d.cmd("rlab_legende Le vrai stade, le vrai mode | Le replay se lance dans son stade et ses règles : ici Hoops, avec son ballon et ses paniers")
    await asyncio.sleep(9.0)

    # ---- 3. each player in his car, his view
    d.scene("joueurs")
    await d.cmd(f"rlab_cam_mode {CAM_PLAYER}",
                "rlab_legende Chaque joueur dans sa voiture | Son nom, ses couleurs, et sa vue : ses réglages caméra et sa caméra ballon")
    for _ in range(3):
        await asyncio.sleep(3.5)
        await d.cmd("rlab_camera")
    await asyncio.sleep(3.0)
    d.end()

    # ---- 4. the real goal, then: what if the defenders were Nexto?
    await d.cmd("rlab_capture", "rlab_legende")
    await d.open(1486)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_capture")
    d.scene("vrai but")
    await d.cmd("rlab_legende Ce qui s'est passé | Les oranges ouvrent le score")
    await asyncio.sleep(5.5)
    d.end()
    await d.cmd("rlab_capture", "rlab_legende")
    await d.open(1486)
    await d.cmd(f"rlab_role {BLUE1} {NEXTO}", f"rlab_role {BLUE2} {NEXTO}")
    await asyncio.sleep(1.5)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_play")
    await d.wait_for(r"Simulation: tu regardes")
    d.scene("nexto")
    await d.cmd("rlab_legende Et si les défenseurs étaient Nexto ? | Simuler d'ici : les oranges refont leur vrai trajet, l'IA Nexto défend pour les bleus")
    await asyncio.sleep(9.0)
    d.end()

    # ---- 5. watch the simulation again like a replay, slowed down, from a player's view
    await d.cmd("rlab_legende", "rlab_revoir")
    await asyncio.sleep(2.5)
    await d.cmd(f"rlab_cam_mode {CAM_PLAYER}", "rlab_vitesse 0.5")
    d.scene("revoir")
    await d.cmd("rlab_legende Chaque simulation s'enregistre | On la revoit comme un replay : ralenti, caméras du jeu... de quoi la filmer")
    for _ in range(2):
        await asyncio.sleep(4.0)
        await d.cmd("rlab_camera")
    await asyncio.sleep(3.0)
    d.end()
    await d.cmd("rlab_vitesse 1", "rlab_legende")

    # ---- 6. the last seconds: zero second rule
    await d.cmd("rlab_capture")
    await d.cmd(f"rlab_role {BLUE1} {GHOST}", f"rlab_role {BLUE2} {GHOST}")
    await d.open(7740)
    await d.cmd(f"rlab_role {BLUE1} {NEXTO}", f"rlab_role {BLUE2} {NEXTO}")
    await asyncio.sleep(1.5)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_play")
    await d.wait_for(r"Simulation: tu regardes")
    d.scene("zero seconde")
    await d.cmd("rlab_legende Et si... à 2 secondes de la fin ? | L'horloge du match et la règle du 0 seconde : tout se joue avant que le ballon touche le sol")
    try:
        await d.wait_for(r"FIN DU MATCH", timeout=25.0)
    except TimeoutError:
        pass
    await asyncio.sleep(1.0)
    await d.cmd("rlab_legende")
    await asyncio.sleep(4.0)

    # ---- 7. end title
    d.scene("fin")
    await d.cmd("rlab_titre REPLAY LAB | Plugin BakkesMod : tes replays, tes scénarios, Nexto")
    await asyncio.sleep(6.0)
    d.scenes[-1]["fin"] = d.now()
    if not d.aborted:
        spectacle_record()
    await d.cmd("rlab_titre", "rlab_legende", "rlab_cinema 0")


async def main():
    password = re.search(r'^rcon_password "([^"]*)"', (BM / "cfg/config.cfg").read_text(errors="ignore"), re.M).group(1)
    async with aiohttp.ClientSession() as s, s.ws_connect("ws://127.0.0.1:9002") as ws:
        await ws.send_str("rcon_password " + password)
        if "authyes" not in str((await ws.receive(timeout=5)).data):
            sys.exit("RCON refuse")
        d = Director(ws)
        await d.cmd("rcon_refresh_allowed", "plugin unload replaylab")
        await asyncio.sleep(1.0)
        await d.cmd("plugin load replaylab")
        await asyncio.sleep(2.0)
        print("j'attends que le jeu soit au premier plan...", flush=True)
        await d.wait_front()
        if "--essai" in sys.argv:
            await essai(d)
        else:
            await film(d)


asyncio.run(main())
