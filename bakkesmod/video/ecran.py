"""Records the screen under Wayland: the desktop portal's screen cast (PipeWire), encoded by the graphics card (NVENC).

    /usr/bin/python3 video/ecran.py sortie.mkv

Nothing is kept until it is told so: frames go through a closed valve. On its standard input, "go" opens it (the
caller makes sure only the game is on screen), "stop" (or the end of the input, SIGINT, SIGTERM) ends the file.
Prints PRET when frames come (valve still closed), ENREGISTRE when the first frame is kept. The first time, KDE asks
which screen to share; the answer is kept (video/.jeton).
(System Python: it needs GStreamer's bindings, gi.)
"""
import signal
import sys
import time
from pathlib import Path

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gio, GLib, Gst  # noqa: E402

TOKEN = Path(__file__).parent / ".jeton"
PORTAL = "org.freedesktop.portal.Desktop"
DESKTOP = "/org/freedesktop/portal/desktop"


class Portal:
    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        self.sender = self.bus.get_unique_name()[1:].replace(".", "_")
        self.count = 0

    def request(self, method, args, options):
        """A portal call answered later by a Response signal on its request object."""
        self.count += 1
        token = f"rlab{self.count}"
        path = f"{DESKTOP}/request/{self.sender}/{token}"
        loop = GLib.MainLoop()
        answer = {}

        def on_response(_bus, _sender, _path, _iface, _signal, params):
            answer["code"], answer["results"] = params.unpack()
            loop.quit()

        sub = self.bus.signal_subscribe(PORTAL, "org.freedesktop.portal.Request", "Response", path, None,
                                        Gio.DBusSignalFlags.NO_MATCH_RULE, on_response)
        options["handle_token"] = GLib.Variant("s", token)
        self.bus.call_sync(PORTAL, DESKTOP, "org.freedesktop.portal.ScreenCast", method,
                           GLib.Variant(f"({args[0]}a{{sv}})", (*args[1:], options)), None, 0, -1, None)
        loop.run()
        self.bus.signal_unsubscribe(sub)
        if answer["code"] != 0:
            sys.exit(f"partage d'ecran refuse ({method}, code {answer['code']})")
        return answer["results"]

    def start(self):
        session = self.request("CreateSession", ("",), {"session_handle_token": GLib.Variant("s", "rlabsession")})["session_handle"]
        options = {"types": GLib.Variant("u", 1), "multiple": GLib.Variant("b", False),
                   "cursor_mode": GLib.Variant("u", 1), "persist_mode": GLib.Variant("u", 2)}   # hidden pointer, kept
        if TOKEN.exists():
            options["restore_token"] = GLib.Variant("s", TOKEN.read_text().strip())
        self.request("SelectSources", ("o", session), options)
        asked = time.time()
        started = self.request("Start", ("os", session, ""), {})
        # a quick answer: the screen chosen last time, without asking (a slow one: KDE asked)
        print(f"partage d'ecran: {time.time() - asked:.1f} s, jeton {'reutilise' if 'restore_token' in options else 'absent'}, "
              f"flux {started['streams']}", file=sys.stderr, flush=True)
        if "restore_token" in started:
            TOKEN.write_text(started["restore_token"])
        node, props = started["streams"][0]
        result, fds = self.bus.call_with_unix_fd_list_sync(
            PORTAL, DESKTOP, "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote",
            GLib.Variant("(oa{sv})", (session, {})), GLib.VariantType("(h)"), 0, -1, None, None)
        return fds.get(result.unpack()[0]), props.get("pipewire-serial", node)


def main():
    out = Path(sys.argv[1])
    Gst.init(None)
    # PipeWire 1.x finds a stream by its serial (the portal gives it); KWin's is an output stream, not taken by default
    fd, serial = Portal().start()
    pipeline = Gst.parse_launch(
        f"pipewiresrc fd={fd} target-object={serial} do-timestamp=true keepalive-time=1000 always-copy=true "
        "! videorate ! video/x-raw,framerate=60/1 ! videoconvert ! video/x-raw,format=NV12 ! valve name=valve drop=true "
        "! nvh264enc name=enc preset=hq rc-mode=vbr bitrate=24000 max-bitrate=40000 gop-size=120 "
        f"! h264parse ! matroskamux ! filesink location=\"{out}\"")
    loop = GLib.MainLoop()
    said = set()

    def say_once(word):
        def probe(_pad, _info):
            if word not in said:
                said.add(word)
                print(word, flush=True)
            return Gst.PadProbeReturn.OK
        return probe

    valve = pipeline.get_by_name("valve")
    valve.get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, say_once("PRET"))
    pipeline.get_by_name("enc").get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, say_once("ENREGISTRE"))

    def on_message(_bus, message):
        if message.type in (Gst.MessageType.EOS, Gst.MessageType.ERROR):
            if message.type == Gst.MessageType.ERROR:
                print("erreur:", message.parse_error()[0].message, file=sys.stderr, flush=True)
            loop.quit()

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message", on_message)

    def stop(*_):
        pipeline.send_event(Gst.Event.new_eos())   # the file gets its end: playable
        return GLib.SOURCE_REMOVE

    def on_input(source, _condition):
        line = source.readline()
        if line.strip() == "go":
            valve.set_property("drop", False)
            return True
        stop()
        return False

    GLib.io_add_watch(GLib.IOChannel.unix_new(sys.stdin.fileno()), GLib.PRIORITY_DEFAULT, GLib.IOCondition.IN | GLib.IOCondition.HUP, on_input)
    signal.signal(signal.SIGINT, lambda *_: GLib.idle_add(stop))
    signal.signal(signal.SIGTERM, lambda *_: GLib.idle_add(stop))
    pipeline.set_state(Gst.State.PLAYING)
    loop.run()
    pipeline.set_state(Gst.State.NULL)
    print("TERMINE", out, flush=True)


main()
