"""Films a presentation of Replay Lab: drives the plugin over RCON (scenes, captions, cameras) while ecran.py records
the screen, and writes when each scene starts and ends (rushes/scenes.json) for monter.py to cut the video.

    .venv/bin/python video/tourner.py            the whole video, recorded by ecran.py
    .venv/bin/python video/tourner.py --manuel   the whole video, recorded by you (Spectacle: Meta+Alt+R); monter.py
                                                 finds where it starts by a magenta flash (rlab_flash)
    .venv/bin/python video/tourner.py --essai    checks first: captions, game HUD, a 4 s recording

Only the game is filmed. The plugin writes twice a second whether the game is in front (data/replaylab_focus.txt);
nothing is kept before the game has been in front 5 s in a row, and as soon as it is not (another window, the game
minimized: the file is no longer written), the recording stops and is thrown away.
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
FOCUS = BM / "data/replaylab_focus.txt"
# a 2v2 soccar replay (Stadium_P, 14 goals): players 1-2 blue, 3 you (orange), 4 orange; its frames for the scenes
REPLAY = os.environ.get("RL_REPLAY", "420342A14DBD4E0C653318A7EA3202E8")
START, GOAL_FORK, TAKEOVER, ZERO_FORK = 3300, 2548, 11795, 12330   # a goal at 3477 / 3.5 s before one / 2.2 s before a save of your shot / 0:02
HERE = Path(__file__).parent
OUT = HERE / "rushes"
# player numbers for rlab_role (1..4) and roles
BLUE1, BLUE2, ME, ORANGE2 = 1, 2, 3, 4
TOI, GHOST, NEXTO = 0, 1, 5
CAM_PLAYER, CAM_AUTO = 1, 3


def focus():
    """What the plugin said last: (game in front, seconds ago); (False, inf) if nothing."""
    try:
        written, front = FOCUS.read_text().split()
        return front == "1", abs(time.time() * 1000 - int(written)) / 1000.0
    except (OSError, ValueError):
        return False, float("inf")


def game_in_front():
    """The game in front, said by the plugin less than 1.5 s ago."""
    front, age = focus()
    return front and age < 1.5


class Away:
    """The game no longer in front: said so for 0.5 s, or no news for 5 s (the game is busy a moment when it sets up
    a scene: a short silence is not leaving it)."""

    def __init__(self):
        self.since = None

    def check(self):
        front, age = focus()
        if age > 5.0:
            return True
        if front or age > 1.5:
            self.since = None
            return False
        self.since = self.since or time.time()
        return time.time() - self.since > 0.5


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


class Recorder:
    """ecran.py: the screen cast, kept only once told "go"."""

    def __init__(self, path):
        self.path = path
        self.proc = None

    async def start(self, timeout=90.0):
        """Up to the frames flowing (KDE may ask which screen to share: not kept)."""
        self.path.unlink(missing_ok=True)
        self.proc = await asyncio.create_subprocess_exec(
            "/usr/bin/python3", str(HERE / "ecran.py"), str(self.path),
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE)
        await self.until("PRET", timeout)

    async def until(self, word, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                line = await asyncio.wait_for(self.proc.stdout.readline(), timeout=max(0.1, end - time.time()))
            except asyncio.TimeoutError:
                break
            if not line:
                raise RuntimeError("l'enregistreur s'est arrete")
            if line.decode().strip() == word:
                return
        raise TimeoutError(f"l'enregistreur n'a pas dit {word}")

    async def go(self):
        self.proc.stdin.write(b"go\n")
        await self.proc.stdin.drain()
        await self.until("ENREGISTRE", 5.0)

    async def stop(self):
        if self.proc and self.proc.returncode is None:
            try:
                self.proc.stdin.write(b"stop\n")
                await self.proc.stdin.drain()
                await asyncio.wait_for(self.proc.wait(), timeout=15.0)
            except (asyncio.TimeoutError, ConnectionResetError, BrokenPipeError):
                self.proc.kill()


class Director:
    def __init__(self, ws):
        self.ws = ws
        self.log = Log()
        self.t0 = None
        self.scenes = []
        self.aborted = False

    async def cmd(self, *commands, pause=0.15):
        for c in commands:
            if c.startswith(("rlab_titre", "rlab_legende")):
                c = c.replace("'", "\u2019")   # BakkesMod's console takes ' for a quote: the typographic one (made plain by the plugin)
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

    @staticmethod
    async def wait_front(timeout=900.0, steady=5.0):
        """The game in front for some seconds in a row (not a quick Alt+Tab)."""
        end, since = time.time() + timeout, None
        while time.time() < end:
            if game_in_front():
                since = since or time.time()
                if time.time() - since >= steady:
                    return
            else:
                since = None
            await asyncio.sleep(0.2)
        raise TimeoutError("le jeu n'est pas passe au premier plan")

    async def watchdog(self, recorder):
        """Only the game is filmed: anything else in front, the recording stops at once (and is thrown away)."""
        away = Away()
        while True:
            await asyncio.sleep(0.1)
            if away.check():
                self.aborted = True
                await recorder.stop()
                print("le jeu n'est plus au premier plan: enregistrement arrete", flush=True)
                return

    def now(self):
        return time.time() - self.t0

    def end(self):
        """The scene being filmed ends here (what follows, loading the next one, is cut)."""
        if self.scenes and "fin" not in self.scenes[-1]:
            self.scenes[-1]["fin"] = round(self.now(), 2)

    def scene(self, name):
        if self.aborted:
            raise RuntimeError("tournage interrompu")
        self.end()
        self.scenes.append({"nom": name, "debut": round(self.now(), 2)})
        print(f"{self.now():6.1f} s  {name}", flush=True)

    async def close_window(self):
        """Replay Lab's window (F3) closed: it would be filmed."""
        await self.cmd("rlab_etat")
        line = await self.wait_for(r"Replay Lab etat: .*fenetre=", timeout=20.0)
        if "fenetre=1" in line:
            await self.cmd("togglemenu replaylab")

    async def open(self, frame):
        await self.cmd(f"rlab_open {REPLAY} {frame}")
        await self.wait_for(r"Replay charge")
        await self.until_playing()

    async def until_playing(self, timeout=60.0):
        """The replay on the pitch (its stadium loaded, its cars set up): the plugin in playback."""
        end = time.time() + timeout
        while time.time() < end:
            await self.cmd("rlab_etat")
            try:
                line = await self.wait_for(r"Replay Lab etat: [a-z ]+, t=", timeout=15.0)
            except TimeoutError:
                continue
            if "etat: lecture" in line or "etat: simulation" in line:
                await asyncio.sleep(1.0)
                return
            await asyncio.sleep(1.0)
        raise TimeoutError("le replay n'est pas en lecture")


def spectacle_recording():
    """Spectacle (or another screen recorder of KDE) reading the screen: its video input stream in PipeWire."""
    out = subprocess.run(["pw-dump"], capture_output=True, text=True).stdout or "[]"
    for o in json.loads(out):
        props = (o.get("info") or {}).get("props", {})
        if o.get("type", "").endswith("Node") and props.get("media.class") == "Stream/Input/Video" and \
                "spectacle" in " ".join(str(props.get(k, "")) for k in ("node.name", "application.name", "application.process.binary")).lower():
            return True
    return False


def screenshot(path):
    if game_in_front():
        subprocess.run(["spectacle", "-b", "-n", "-f", "-o", str(path)], check=False)


async def essai(d):
    """Captions, the game's HUD hidden, a 4 s recording (one frame kept to look at, the video thrown away)."""
    OUT.mkdir(exist_ok=True)
    await d.open(START)
    await d.close_window()
    await d.cmd("rlab_cinema 1", "rlab_legende Scénario alternatif | à 0 seconde, été, où, ça", "rlab_titre REPLAY LAB | Prêt à tourner ?", "rlab_hud")
    await asyncio.sleep(1.0)
    screenshot(OUT / "essai_legendes.png")
    recorder = Recorder(OUT / "essai.mkv")
    await recorder.start()
    await d.wait_front(steady=2.0)
    await recorder.go()
    guard = asyncio.create_task(d.watchdog(recorder))
    await asyncio.sleep(4.0)
    guard.cancel()
    await recorder.stop()
    await d.cmd("rlab_hud", "rlab_titre", "rlab_legende")
    if d.aborted:
        recorder.path.unlink(missing_ok=True)
        sys.exit("essai interrompu (le jeu n'etait plus au premier plan), enregistrement supprime")
    print(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration:stream=codec_name,width,height,avg_frame_rate",
                          "-of", "compact", str(recorder.path)], capture_output=True, text=True).stdout)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", "2", "-i", str(recorder.path), "-frames:v", "1", str(OUT / "essai_video.png")])
    recorder.path.unlink(missing_ok=True)


async def film(d):
    OUT.mkdir(exist_ok=True)
    # ---- set up, before the recording
    await d.cmd("rlab_cinema 1", "rlab_tableau 1", "rlab_noms 1", "rlab_cam_joueur 1", "rlab_speed 1", "rlab_vitesse 1",
                "rlab_titre", "rlab_legende")
    await d.open(START)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_capture")   # director camera, playing
    await d.close_window()
    recorder = Recorder(OUT / "prise.mkv")
    await recorder.start()
    await d.wait_front(steady=2.0)
    await d.cmd("rlab_hud")   # the game's own HUD away (boost gauge, freeplay hints)
    await asyncio.sleep(0.5)
    await recorder.go()
    d.t0 = time.time()
    guard = asyncio.create_task(d.watchdog(recorder))
    try:
        await scenes(d)
    except RuntimeError:
        pass
    finally:
        guard.cancel()
        await recorder.stop()
        await d.cmd("rlab_hud", "rlab_titre", "rlab_legende", "rlab_cinema 0", "rlab_vitesse 1")
    if d.aborted:
        recorder.path.unlink(missing_ok=True)
        sys.exit("tournage interrompu (le jeu n'etait plus au premier plan), enregistrement supprime")
    (OUT / "scenes.json").write_text(json.dumps({"video": str(recorder.path), "scenes": d.scenes}, indent=2, ensure_ascii=False))
    print("video:", recorder.path, "scenes:", OUT / "scenes.json")


async def film_manual(d):
    """You record (Spectacle); the scenes start once your recording runs and the game is in front, after a flash."""
    OUT.mkdir(exist_ok=True)
    await d.cmd("rlab_cinema 1", "rlab_tableau 1", "rlab_noms 1", "rlab_cam_joueur 1", "rlab_speed 1", "rlab_vitesse 1",
                "rlab_titre", "rlab_legende")
    await d.open(START)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_capture")
    await d.close_window()
    print("Lance l'enregistrement de l'ecran (Meta+Alt+R), puis reviens sur le jeu et ne touche plus a rien.", flush=True)
    await d.cmd("rlab_legende Pret a tourner | Lance l'enregistrement de l'ecran : Meta+Alt+R, puis ne touche plus a rien")
    while not spectacle_recording():
        await asyncio.sleep(0.5)
    print("enregistrement vu, j'attends le jeu au premier plan...", flush=True)
    await d.cmd("rlab_legende C'est parti dans 3 secondes | Ne touche plus a rien")
    await d.wait_front(steady=3.0)
    await d.cmd("rlab_legende")
    await asyncio.sleep(0.5)
    await d.cmd("rlab_hud")
    await asyncio.sleep(0.5)
    await d.cmd("rlab_flash")
    d.t0 = time.time()

    async def guard():
        away = Away()
        while True:
            await asyncio.sleep(0.1)
            if away.check():
                d.aborted = True
                return
    watch = asyncio.create_task(guard())
    try:
        await scenes(d)
    except RuntimeError:
        pass
    finally:
        watch.cancel()
        await d.cmd("rlab_hud", "rlab_titre", "rlab_legende Tournage fini | Arrete l'enregistrement : Meta+Alt+R", "rlab_vitesse 1")
    d.end()
    (OUT / "scenes.json").write_text(json.dumps({"video": "", "repere": "flash", "interrompu": d.aborted, "scenes": d.scenes},
                                                indent=2, ensure_ascii=False))
    if d.aborted:
        await d.cmd("rlab_legende Tournage interrompu | Arrete l'enregistrement (Meta+Alt+R), on recommencera")
        sys.exit("tournage interrompu (le jeu n'etait plus au premier plan) : arrete ton enregistrement ; scenes deja tournees dans scenes.json")
    print("C'est fini : arrete l'enregistrement (Meta+Alt+R). Scenes:", OUT / "scenes.json", flush=True)


async def scenes(d):
    await asyncio.sleep(1.0)

    # ---- 1. title over the match playing
    d.scene("titre")
    await d.cmd("rlab_titre REPLAY LAB | Rejoue tes replays dans Rocket League... et change l'histoire")
    await asyncio.sleep(7.0)
    await d.cmd("rlab_titre")

    # ---- 2. the replay in the game: its stadium, each player in his car, his view
    d.scene("replay")
    await d.cmd("rlab_legende Le replay, dans le jeu | Son stade et son mode, chaque joueur dans sa voiture : son nom, ses couleurs")
    await asyncio.sleep(6.0)
    await d.cmd(f"rlab_cam_mode {CAM_PLAYER}",
                "rlab_legende La vue de chaque joueur | Ses réglages caméra et sa caméra ballon, comme dans le vrai replay")
    for _ in range(3):
        await asyncio.sleep(3.0)
        await d.cmd("rlab_camera")
    await asyncio.sleep(2.0)
    d.end()

    # ---- 3. a real goal, then: what if the defenders were Nexto?
    await d.cmd("rlab_capture", "rlab_legende")
    await d.open(GOAL_FORK)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_capture")
    d.scene("vrai but")
    await d.cmd("rlab_legende Ce qui s'est passé | Les oranges marquent")
    await asyncio.sleep(5.5)
    d.end()
    await d.cmd("rlab_capture", "rlab_legende")
    await d.open(GOAL_FORK)
    await d.cmd(f"rlab_role {BLUE1} {NEXTO}", f"rlab_role {BLUE2} {NEXTO}")
    await asyncio.sleep(1.5)
    await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_play")
    await d.wait_for(r"Simulation: tu regardes")
    d.scene("nexto")
    await d.cmd("rlab_legende Et si les défenseurs étaient Nexto ? | Simuler d'ici : les oranges refont leur vrai trajet, l'IA Nexto défend pour les bleus")
    await asyncio.sleep(9.0)
    d.end()

    # ---- 4. your turn: take over just before a save, as many tries as it takes
    await d.cmd("rlab_legende")
    await d.open(TAKEOVER)
    await d.cmd(f"rlab_role {ME} {TOI}")
    await asyncio.sleep(2.5)
    await d.cmd("rlab_play")
    await d.wait_for(r"Simulation prete")
    d.scene("controle")
    tries, intro = [d.now()], d.now()

    async def caption():
        await d.cmd("rlab_legende A toi : ton tir a été arrêté, fais mieux | Figée comme en entraînement : la scène part dès que tu touches la manette."
                    f" Raté ? Bouton reset (essai {len(tries)})")
    await caption()
    scored, end = False, time.time() + 600.0
    while time.time() < end and not scored:
        try:
            line = await d.wait_for(r"Simulation prete|BUT pour les oranges", timeout=end - time.time())
        except TimeoutError:
            break
        if "Simulation prete" in line:
            tries.append(d.now())
            await caption()
        else:
            scored = True
    await d.cmd(f"rlab_legende But ! | Réussi en {len(tries)} essai{'s' if len(tries) > 1 else ''}" if scored else "rlab_legende")
    await asyncio.sleep(3.5)
    d.end()
    last = d.scenes[-1]
    last["essais"] = len(tries)
    # in the video: the frozen scene and its caption, then the try that went in
    last["morceaux"] = [[intro, intro + 5.0], [tries[-1], last["fin"]]] if len(tries) > 1 else [[intro, last["fin"]]]

    # ---- 5. watch it again like a replay, slowed down
    if scored:
        await d.cmd("rlab_legende", "rlab_revoir")
        await asyncio.sleep(2.5)
        await d.cmd(f"rlab_cam_mode {CAM_AUTO}", "rlab_vitesse 0.5")
        d.scene("revoir")
        await d.cmd("rlab_legende Chaque simulation s'enregistre | On la revoit comme un replay : ralenti, caméras du jeu... de quoi la filmer")
        await asyncio.sleep(9.0)
        d.end()
        await d.cmd("rlab_vitesse 1", "rlab_legende")

    # ---- 6. the last seconds: zero second rule
    await d.cmd("rlab_capture")
    await d.open(ZERO_FORK)
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
    d.end()


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
        if "--manuel" not in sys.argv:
            print("j'attends que le jeu soit au premier plan...", flush=True)
            await d.wait_front()
        if "--essai" in sys.argv:
            await essai(d)
        elif "--manuel" in sys.argv:
            await film_manual(d)
        else:
            await film(d)


asyncio.run(main())
