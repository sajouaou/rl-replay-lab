"""Records the screen under Wayland: the desktop portal's screen cast (PipeWire), encoded by the graphics card (NVENC).

    /usr/bin/python3 video/ecran.py sortie.mkv

Prints ENREGISTRE when the first frames come, stops cleanly on SIGINT or SIGTERM. The first time, KDE asks which
screen to share; the answer is kept (video/.jeton) and not asked again.
(System Python: it needs GStreamer's bindings, gi.)
"""
import json
import signal
import subprocess
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
        started = self.request("Start", ("os", session, ""), {})
        if "restore_token" in started:
            TOKEN.write_text(started["restore_token"])
        node = started["streams"][0][0]
        result, fds = self.bus.call_with_unix_fd_list_sync(
            PORTAL, DESKTOP, "org.freedesktop.portal.ScreenCast", "OpenPipeWireRemote",
            GLib.Variant("(oa{sv})", (session, {})), GLib.VariantType("(h)"), 0, -1, None, None)
        return fds.get(result.unpack()[0]), node


def main():
    out = Path(sys.argv[1])
    Gst.init(None)
    fd, node = Portal().start()
    # PipeWire 1.x finds a stream by its serial, not its id (and KWin's is an output stream: not taken by default)
    serial = None
    for _ in range(30):   # the stream shows up a moment after the portal answers
        found = [o for o in json.loads(subprocess.run(["pw-dump"], capture_output=True, text=True).stdout or "[]") if o.get("id") == node]
        if found and "object.serial" in found[0].get("info", {}).get("props", {}):
            serial = found[0]["info"]["props"]["object.serial"]
            break
        time.sleep(0.1)
    if serial is None:
        sys.exit(f"flux d'ecran {node} introuvable dans PipeWire")
    pipeline = Gst.parse_launch(
        f"pipewiresrc fd={fd} target-object={serial} do-timestamp=true keepalive-time=1000 always-copy=true "
        "! videorate ! video/x-raw,framerate=60/1 ! videoconvert ! video/x-raw,format=NV12 "
        "! nvh264enc name=enc preset=hq rc-mode=vbr bitrate=24000 max-bitrate=40000 gop-size=120 "
        f"! h264parse ! matroskamux ! filesink location=\"{out}\"")
    loop = GLib.MainLoop()
    first = {"seen": False}

    def on_frame(_pad, _info):
        if not first["seen"]:
            first["seen"] = True
            print("ENREGISTRE", flush=True)
        return Gst.PadProbeReturn.OK

    pipeline.get_by_name("enc").get_static_pad("sink").add_probe(Gst.PadProbeType.BUFFER, on_frame)

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

    signal.signal(signal.SIGINT, lambda *_: GLib.idle_add(stop))
    signal.signal(signal.SIGTERM, lambda *_: GLib.idle_add(stop))
    pipeline.set_state(Gst.State.PLAYING)
    loop.run()
    pipeline.set_state(Gst.State.NULL)
    print("TERMINE", out, flush=True)


main()
