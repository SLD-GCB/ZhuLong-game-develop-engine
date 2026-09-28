"""The wire the running program sends its picture down.

A frame is a twenty-byte header and then the pixels:

    magic   'ZLFR'      4 bytes
    width   uint32 LE
    height  uint32 LE
    pitch   uint32 LE   bytes per row
    length  uint32 LE   bytes of pixel data following

That is deliberately the shape `Platform::Present` already has -- RGBA8, a pitch, a width and a
height -- so the sending side does no translation, and so a C++ host (or anything else) could write
the same bytes without a second specification.

Frames travel on the child's **stderr**; its stdout stays what the program says. One channel for
words, one for pictures, which keeps the console from having to tell a 3.7 MB frame apart from a line
of `print`.

Nothing here imports Qt: this half runs inside the child.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

MAGIC = b"ZLFR"
HEADER = struct.Struct("<4sIIII")
HEADER_SIZE = HEADER.size       # 20


@dataclass(frozen=True)
class Frame:
    rgba: bytes
    width: int
    height: int
    pitch: int


@dataclass
class Read:
    """What one helping of the channel turned out to be.

    `text` is everything that was not a frame. On the picture channel it should always be empty --
    a program that takes the editor's host points its own stderr somewhere else first -- but a
    program that never looks at `ZL_EDITOR_HOST` writes its complaints down the same pipe, and
    swallowing them silently would be worse than the occasional stray word in the console.
    """

    frames: list[Frame]
    text: bytes = b""


def pack(frame: Frame) -> bytes:
    return HEADER.pack(MAGIC, frame.width, frame.height, frame.pitch, len(frame.rgba)) + frame.rgba


class Reader:
    """Turns a byte stream into frames, however the stream happens to be cut up.

    A pipe gives you whatever it feels like: half a frame, three frames, a frame with the tail of the
    previous one in front of it. So this keeps what is left over and only hands back what is whole.
    """

    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> Read:
        self._buffer += data
        frames: list[Frame] = []
        text = bytearray()

        while True:
            start = self._buffer.find(MAGIC)
            if start < 0:
                # Nothing that looks like a frame. Hold back only what could be the beginning of
                # one: a pipe can cut a stream between the Z and the L, so a tail of `Z`, `ZL` or
                # `ZLF` waits for the rest. Anything else is text, and holding it would mean a
                # program's last line never reached the console.
                keep = 0
                for length in range(min(len(MAGIC) - 1, len(self._buffer)), 0, -1):
                    if MAGIC.startswith(bytes(self._buffer[-length:])):
                        keep = length
                        break
                if len(self._buffer) > keep:
                    text += self._buffer[: len(self._buffer) - keep]
                    del self._buffer[: len(self._buffer) - keep]
                break
            if start > 0:
                text += self._buffer[:start]
                del self._buffer[:start]
            if len(self._buffer) < HEADER_SIZE:
                break

            _, width, height, pitch, length = HEADER.unpack_from(self._buffer)
            if length == 0 or width == 0 or height == 0 or pitch < width * 4:
                # Not a frame after all: drop the magic and look again.
                text += self._buffer[: len(MAGIC)]
                del self._buffer[: len(MAGIC)]
                continue
            if len(self._buffer) < HEADER_SIZE + length:
                break

            payload = bytes(self._buffer[HEADER_SIZE:HEADER_SIZE + length])
            del self._buffer[: HEADER_SIZE + length]
            frames.append(Frame(rgba=payload, width=width, height=height, pitch=pitch))

        return Read(frames=frames, text=bytes(text))

    def pending(self) -> int:
        return len(self._buffer)

    def flush(self) -> Read:
        """Give up on whatever is left -- the child has stopped, so it is not a frame."""
        text = bytes(self._buffer)
        self._buffer.clear()
        return Read(frames=[], text=text)
