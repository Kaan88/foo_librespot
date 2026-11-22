#!/usr/bin/env python3
"""Fake librespot-bridge for testing foo_librespot without Spotify.

Implements proto/PROTOCOL.md v1 over a named pipe using pywin32:
status / info / resolve / open (sine wave PCM, sample-accurate start_frame).

Usage:
    python fake_bridge.py --pipe foo_librespot --duration-ms 30000

Options:
    --pipe NAME          pipe name without \\.\pipe\ prefix (default foo_librespot)
    --duration-ms N      track length reported and streamed (default 30000)
    --rate N             sample rate (default 44100)
    --channels N         channels (default 2)
    --realtime           pace PCM chunks to real time (default: as fast as possible)
    --logged-out         answer not_logged_in to info/resolve/open
    --quiet              do not log each connection
"""

import argparse
import json
import math
import os
import struct
import sys
import threading
import time

import pywintypes
import win32file
import win32pipe

PIPE_UNLIMITED_INSTANCES = 255

ARGS = None


def log(message):
    if not ARGS.quiet:
        print(f"[fake_bridge] {message}", flush=True)


def make_track(uri, index=0):
    """Deterministic metadata for a uri."""
    if uri.startswith("spotify:track:"):
        track_id = uri.split(":")[-1]
    else:
        track_id = f"fake{index:018d}"[:22]
    return {
        "uri": uri if uri.startswith("spotify:track:") else f"spotify:track:{track_id}",
        "title": f"Fake Track {index + 1}",
        "artists": ["Fake Artist", "Second Artist"] if index == 0 else ["Fake Artist"],
        "album": "Fake Album",
        "album_artist": "Fake Artist",
        "track_number": index + 1,
        "disc_number": 1,
        "date": "2026",
        "duration_ms": ARGS.duration_ms,
        "codec": "vorbis",
        "bitrate": 320,
        "cover_url": "",
        "track_gain_db": -7.25,
        "track_peak": 0.9,
        "album_gain_db": None,
        "album_peak": None,
        "playable": True,
    }


def resolve_tracks(uri):
    if uri.startswith("spotify:track:") or "/track/" in uri:
        if uri.startswith("spotify:track:"):
            return [make_track(uri)]
        track_id = uri.split("/track/")[1].split("?")[0]
        return [make_track(f"spotify:track:{track_id}")]
    # album / playlist / anything else: a few tracks
    base = abs(hash(uri)) % 1000000
    return [make_track(f"spotify:track:{base + i:022d}", i) for i in range(3)]


def error_reply(code, message):
    return {"ok": False, "error": message, "code": code}


def read_request_line(handle):
    data = b""
    while b"\n" not in data:
        _, chunk = win32file.ReadFile(handle, 65536)
        if not chunk:
            return None
        data += chunk
    return json.loads(data.split(b"\n", 1)[0].decode("utf-8"))


def write_all(handle, data):
    offset = 0
    while offset < len(data):
        _, written = win32file.WriteFile(handle, data[offset:])
        if written == 0:
            raise ConnectionError("write returned 0")
        offset += written


def stream_pcm(handle, start_frame, total_frames):
    rate = ARGS.rate
    channels = ARGS.channels
    frames_per_chunk = 4096
    frame = start_frame
    while frame < total_frames:
        count = min(frames_per_chunk, total_frames - frame)
        values = []
        for i in range(count):
            t = (frame + i) / rate
            sample = 0.25 * math.sin(2.0 * math.pi * 440.0 * t)
            values.extend([sample] * channels)
        write_all(handle, struct.pack(f"<{len(values)}f", *values))
        frame += count
        if ARGS.realtime:
            time.sleep(count / rate)


def handle_connection(handle):
    try:
        request = read_request_line(handle)
        if request is None:
            return
        op = request.get("op")
        log(f"op={op} uri={request.get('uri', '')}")

        if op == "status":
            reply = {
                "ok": True,
                "logged_in": not ARGS.logged_out,
                "username": "" if ARGS.logged_out else "fake_user",
                "country": "" if ARGS.logged_out else "TR",
                "version": "0.1.0-fake",
            }
            write_all(handle, (json.dumps(reply) + "\n").encode("utf-8"))
            return

        if ARGS.logged_out and op in ("info", "resolve", "open"):
            write_all(handle, (json.dumps(error_reply("not_logged_in", "not logged in")) + "\n").encode("utf-8"))
            return

        if op == "info":
            uris = request.get("uris", [])
            tracks = [make_track(uri, i) for i, uri in enumerate(uris)]
            write_all(handle, (json.dumps({"ok": True, "tracks": tracks}) + "\n").encode("utf-8"))
            return

        if op == "resolve":
            tracks = resolve_tracks(request.get("uri", ""))
            log(f"resolved {len(tracks)} track(s)")
            write_all(handle, (json.dumps({"ok": True, "tracks": tracks}) + "\n").encode("utf-8"))
            return

        if op == "open":
            uri = request.get("uri", "")
            start_ms = int(request.get("start_ms", 0))
            start_frame = round(start_ms * ARGS.rate / 1000.0)
            total_frames = round(ARGS.duration_ms * ARGS.rate / 1000.0)
            header = {
                "ok": True,
                "track": make_track(uri),
                "sample_rate": ARGS.rate,
                "channels": ARGS.channels,
                "start_frame": start_frame,
            }
            log(f"open start_ms={start_ms} start_frame={start_frame}")
            write_all(handle, (json.dumps(header) + "\n").encode("utf-8"))
            stream_pcm(handle, start_frame, total_frames)
            return

        write_all(handle, (json.dumps(error_reply("bad_request", f"unknown op {op}")) + "\n").encode("utf-8"))
    except (BrokenPipeError, ConnectionError, pywintypes.error) as exc:
        log(f"client disconnected: {exc}")
    finally:
        try:
            win32pipe.DisconnectNamedPipe(handle)
        except pywintypes.error:
            pass
        try:
            handle.Close()
        except Exception:
            pass


def serve():
    pipe_name = "\\\\.\\pipe\\" + ARGS.pipe
    while True:
        handle = win32pipe.CreateNamedPipe(
            pipe_name,
            win32pipe.PIPE_ACCESS_DUPLEX,
            win32pipe.PIPE_TYPE_BYTE | win32pipe.PIPE_READMODE_BYTE | win32pipe.PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            64 * 1024,
            64 * 1024,
            0,
            None,
        )
        try:
            win32pipe.ConnectNamedPipe(handle, None)
        except pywintypes.error as exc:
            if exc.winerror != 535:  # ERROR_PIPE_CONNECTED
                log(f"ConnectNamedPipe failed: {exc}")
                handle.Close()
                continue
        threading.Thread(target=handle_connection, args=(handle,), daemon=True).start()


def main():
    global ARGS
    parser = argparse.ArgumentParser()
    parser.add_argument("--pipe", default="foo_librespot")
    parser.add_argument("--duration-ms", type=int, default=30000)
    parser.add_argument("--rate", type=int, default=44100)
    parser.add_argument("--channels", type=int, default=2)
    parser.add_argument("--realtime", action="store_true")
    parser.add_argument("--logged-out", action="store_true")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--parent-pid", type=int, default=0)
    ARGS = parser.parse_args()

    log(f"listening on \\\\.\\pipe\\{ARGS.pipe} (duration {ARGS.duration_ms} ms, "
        f"{ARGS.rate} Hz, {ARGS.channels} ch{', logged out' if ARGS.logged_out else ''})")
    try:
        serve()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
