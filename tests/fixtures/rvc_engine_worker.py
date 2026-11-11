"""Engine regression protocol fixture; never loads neural assets or singer files."""
import msvcrt
import os
from pathlib import Path
import struct
import sys
import threading
import time

msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
slots = [("", ""), ("", "")]
hang = os.environ.get("ENGINE_FIX_HANG", "")
reject_load = os.environ.get("ENGINE_FIX_REJECT_LOAD") == "1"
reply_mode = os.environ.get("ENGINE_FIX_REPLY", "")


def wait_forever():
    threading.Event().wait()


def send(kind, payload=b""):
    sys.stdout.buffer.write(struct.pack("<4sII", b"RVCR", kind, len(payload)) + payload)
    sys.stdout.buffer.flush()


def info(slot):
    path, index = slots[slot]
    name = Path(path).stem if path else "UNLOADED"
    return (struct.pack("<8I", slot, bool(path), bool(path), bool(index), 40000, 2, 1 if index else 0, 768)
            + name.encode()[:63].ljust(64, b"\0")
            + path.encode()[:511].ljust(512, b"\0")
            + b"FIXTURE".ljust(64, b"\0") + bytes(128 * 4)
            + index.encode()[:511].ljust(512, b"\0"))


while True:
    header = sys.stdin.buffer.read(12)
    if len(header) != 12:
        break
    magic, command, size = struct.unpack("<4sII", header)
    if command == 5 and hang == "synth-write":
        wait_forever()  # Fill the pipe while the engine is writing a large request.
    payload = sys.stdin.buffer.read(size)
    if command == 1:
        if hang in ("init", "timeout"):
            wait_forever()
        send(20, info(0) + info(1))
    elif command == 2:
        if hang == "load":
            wait_forever()
        slot, path_len = struct.unpack_from("<II", payload)
        path = payload[8:8 + path_len].decode()
        index = ""
        if len(payload) > 8 + path_len:
            index_len, = struct.unpack_from("<I", payload, 8 + path_len)
            index = payload[12 + path_len:12 + path_len + index_len].decode()
        if Path(path).stem == "bad" or reject_load:
            message = b"invalid checkpoint fixture"
            send(90, struct.pack("<I", len(message)) + message)
        else:
            if Path(path).stem == "slow_good":
                time.sleep(0.3)
            slots[slot] = (path, index)
            send(21, info(slot))
    elif command == 4:
        slot, = struct.unpack("<I", payload)
        slots[slot] = ("", "")
        send(21, info(slot))
    elif command == 5:
        if hang == "synth-read":
            wait_forever()
        marker = 0.125 if Path(slots[0][0]).stem == "good" else 0.25
        target_sr, input_samples = struct.unpack_from("<II", payload, 36)
        output_samples = round(input_samples * target_sr / 16000)
        if reply_mode == "one-short":
            output_samples -= 1
        elif reply_mode == "one-long":
            output_samples += 1
        elif reply_mode == "short":
            output_samples = 2
        elif reply_mode == "long":
            output_samples += 100
        elif reply_mode == "empty":
            output_samples = 0
        audio_a = struct.pack("<f", marker) * output_samples
        audio_b = bytes(output_samples * 4)
        send(22, struct.pack("<ffI", 220, 1, output_samples) + bytes(4 * 64 * 4) + audio_a + audio_b)
