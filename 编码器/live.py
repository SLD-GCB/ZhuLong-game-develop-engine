"""Run a script with the editor as its host.

    python -u live.py <script> [args...]

This is what the editor launches instead of the script, and it does two things before the script
starts.

**stderr becomes the picture channel.** The editor's half of the machine writes each presented frame
down it in the format `frames.py` describes. The script's own stderr is pointed at stdout first, so
tracebacks and warnings still reach the console; only the frames go the other way.

Frames do not block the script: a 1280x720 frame is 3.7 MB, and sixty of those a second is 220 MB/s,
so a synchronous write would drag the game down to the editor's reading speed. Sending happens on
another thread with a **one-frame slot**, and a frame that arrives while the slot is full **replaces**
what is in it. Someone watching wants to know what it looks like now, not what every frame looked
like; the ones thrown away are counted, and the script can say so at the end.

**`zlong.System` is shadowed by one that always has a host.** A machine with no host still renders,
but it presents to nobody, so nothing would reach the editor. The shadow installs the editor's host
on construction. A script that names a host of its own does not lose it: the editor's host shows the
frame and then passes it on, so `scene_demo.py`'s PNG is still written *and* the editor sees the
picture.

Nothing here imports Qt: this runs inside the child process.
"""

from __future__ import annotations

import io
import os
import queue
import runpy
import sys
import threading
import time
from pathlib import Path

import frames

# The exit code to use when a script ends by raising SystemExit with something that is not a number.
_FAILED = 1


def open_channel():
    """A private handle on the real stderr, with fd 2 sent back to stdout."""
    handle = io.open(os.dup(2), "wb")
    os.dup2(1, 2)
    return handle


class Sender:
    """One slot, filled by the frame loop and emptied by a thread of its own.

    The frame loop never waits: if the slot still holds a frame the editor has not taken, the new one
    takes its place and the old one is counted as dropped.
    """

    def __init__(self, channel) -> None:
        self._channel = channel
        self._slot: queue.Queue[bytes] = queue.Queue(maxsize=1)
        self.sent = 0
        self.dropped = 0
        self._thread = threading.Thread(target=self._write_forever, daemon=True)
        self._thread.start()

    def offer(self, packet: bytes) -> None:
        try:
            self._slot.put_nowait(packet)
        except queue.Full:
            try:
                self._slot.get_nowait()      # whatever was waiting is now the older frame
                self.dropped += 1
            except queue.Empty:
                pass
            try:
                self._slot.put_nowait(packet)
            except queue.Full:
                pass

    def _write_forever(self) -> None:
        while True:
            packet = self._slot.get()
            try:
                self._channel.write(packet)
                self._channel.flush()
            except OSError:
                return                        # the editor has gone
            self.sent += 1

    def finish(self, timeout: float = 2.0) -> None:
        """Wait for the frame in hand to go out, so the last thing drawn is the last thing shown."""
        deadline = time.monotonic() + timeout
        while not self._slot.empty() and time.monotonic() < deadline:
            time.sleep(0.01)


def install(zlong, channel) -> "Sender":
    """Make the editor a host of every machine the script builds."""

    sender = Sender(channel)

    def show(rgba: bytes, pitch: int, width: int, height: int) -> None:
        sender.offer(frames.pack(frames.Frame(rgba=rgba, width=width, height=height,
                                              pitch=pitch)))

    class EditorHost(zlong.Platform):
        """The editor's half of a host: show the frame, then hand it to whoever else wants it."""

        def __init__(self, inner=None) -> None:
            super().__init__()
            self._inner = inner

        def pump(self) -> bool:
            return self._inner.pump() if self._inner is not None else True

        def on_frame(self, scene, seconds: float) -> None:
            if self._inner is not None:
                self._inner.on_frame(scene, seconds)

        def on_mix(self, scene, seconds: float) -> None:
            if self._inner is not None:
                self._inner.on_mix(scene, seconds)

        def present(self, rgba, pitch: int, width: int, height: int) -> None:
            show(rgba, pitch, width, height)
            if self._inner is not None:
                self._inner.present(rgba, pitch, width, height)

    base = zlong.System

    class EditorSystem(base):
        def __init__(self, config) -> None:
            super().__init__(config)
            # Straight to the C++ one: going through the override below would put a second editor
            # host in the chain and write every frame out twice.
            base.set_platform(self, EditorHost())

        def set_platform(self, platform) -> None:
            base.set_platform(self, EditorHost(platform))

    zlong.System = EditorSystem


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv if argv is None else argv)
    if len(argv) < 2:
        sys.stderr.write("live.py: no script given\n")
        return _FAILED

    script = Path(argv[1]).resolve()
    if not script.is_file():
        sys.stderr.write(f"live.py: no such script: {script}\n")
        return _FAILED

    # The interpreter put this file's directory first. The script's own directory belongs there
    # instead, so that a script can import the modules sitting next to it.
    sys.path[0] = str(script.parent)
    sys.argv = [str(script), *argv[2:]]

    channel = open_channel()
    try:
        import zlong
    except ImportError:
        # Not every script wants the machine; one that does will say so itself.
        zlong = None
    sender = install(zlong, channel) if zlong is not None else None

    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as stop:
        if stop.code is None:
            return 0
        if isinstance(stop.code, int):
            return stop.code
        sys.stderr.write(f"{stop.code}\n")
        return _FAILED
    finally:
        # Let the frame in hand go out, then say what the sending managed: those numbers are how a
        # program finds out it was drawing faster than the editor could show it.
        if sender is not None and sender.sent + sender.dropped:
            sender.finish()
            print(f"[画面 显示 {sender.sent} 帧，丢掉 {sender.dropped} 帧]")
        channel.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
