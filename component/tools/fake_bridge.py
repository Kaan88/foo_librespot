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
STATE = {"revision": "1", "tracks": 3, "saved": set(), "playlist_adds": 0}
STATE_LOCK = threading.Lock()


def log(message):
    if not ARGS.quiet:
        print(f"[fake_bridge] {message}", flush=True)


def make_track(uri, index=0, bitrate=320):
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
        "album_uri": "spotify:album:fakealbum000000000001",
        "artist_uris": (["spotify:artist:fakeartist00000000001", "spotify:artist:secondartist0000000001"]
                        if index == 0 else ["spotify:artist:fakeartist00000000001"]),
        "album_artist": "Fake Artist",
        "track_number": index + 1,
        "disc_number": 1,
        "date": "2026",
        "duration_ms": ARGS.duration_ms,
        "codec": "vorbis",
        "bitrate": bitrate,
        "cover_url": "",
        "track_gain_db": -7.25,
        "track_peak": 0.9,
        "album_gain_db": None,
        "album_peak": None,
        "playable": True,
    }


def make_item(uri, kind, name, subtitle="", revision=None, track_count=None, editable=None):
    item = {
        "uri": uri,
        "kind": kind,
        "name": name,
        "subtitle": subtitle,
        "image_url": None,
        "track_count": track_count,
        "revision": revision,
    }
    if editable is not None:
        item["editable"] = editable
    return item


def context_name(uri):
    if uri.startswith("spotify:album:"):
        return "Fake Album"
    if uri.startswith("spotify:user:"):
        return "Liked Songs"
    if uri.startswith("spotify:artist:"):
        return "Fake Artist"
    return "Fake Playlist"


def make_library():
    with STATE_LOCK:
        revision = STATE["revision"]
        count = STATE["tracks"]
    return {
        "ok": True,
        "sections": [
            {"id": "liked", "title": "Liked Songs", "items": [
                make_item("spotify:user:fake_user:collection", "collection", "Liked Songs",
                          "", revision, count)]},
            {"id": "made_for_you", "title": "Made for you", "items": [
                make_item("spotify:playlist:made000000000000000001", "playlist",
                          "Daily Mix 1", "Spotify", revision, count),
                make_item("spotify:playlist:made000000000000000002", "playlist",
                          "Discover Weekly", "Spotify", revision, count)]},
            {"id": "playlists", "title": "Your playlists", "items": [
                make_item("spotify:playlist:user000000000000000001", "playlist",
                          "Road Trip", "fake_user", revision, count, True),
                make_item("spotify:playlist:shared00000000000001", "playlist",
                          "Shared Mix", "someone_else", revision, count, False)]},
            {"id": "albums", "title": "Albums", "items": [
                make_item("spotify:album:alb0000000000000000001", "album",
                          "Saved Album One", "Fake Artist", revision, count),
                make_item("spotify:album:alb0000000000000000002", "album",
                          "Saved Album Two", "Fake Artist", revision, count)]},
            {"id": "artists", "title": "Artists", "items": [
                make_item("spotify:artist:art0000000000000000001", "artist",
                          "Fake Artist", "", None, count)]},
        ],
    }


def make_search(query, limit):
    limit = max(1, min(limit, 50))
    with STATE_LOCK:
        revision = STATE["revision"]
        count = STATE["tracks"]
    return {
        "ok": True,
        "tracks": [make_track("", i) for i in range(min(2, limit))],
        "albums": [make_item("spotify:album:sea0000000000000000001", "album",
                             f"{query} Album", "Fake Artist", revision, count)],
        "artists": [make_item("spotify:artist:sea0000000000000000001", "artist",
                              f"{query} Artist", "", None, count)],
        "playlists": [make_item("spotify:playlist:sea0000000000000000001", "playlist",
                                f"{query} Playlist", "fake_user", revision, count)],
    }


def make_artist(uri):
    with STATE_LOCK:
        revision = STATE["revision"]
        count = STATE["tracks"]
    return {
        "ok": True,
        "artist": make_item(uri, "artist", "Fake Artist", "", None, count),
        "top_tracks": [make_track("", 0), make_track("", 1)],
        "albums": [
            make_item("spotify:album:art0000000000000000001", "album",
                      "Artist Album One", "Fake Artist", revision, count),
            make_item("spotify:album:art0000000000000000002", "album",
                      "Artist Album Two", "Fake Artist", revision, count),
        ],
        "singles": [
            make_item("spotify:album:art0000000000000000003", "album",
                      "Artist Single", "Fake Artist", revision, count),
        ],
    }


def resolve_reply(uri):
    if uri.startswith("spotify:track:") or "/track/" in uri:
        if uri.startswith("spotify:track:"):
            return {"ok": True, "tracks": [make_track(uri)]}
        track_id = uri.split("/track/")[1].split("?")[0]
        return {"ok": True, "tracks": [make_track(f"spotify:track:{track_id}")]}

    with STATE_LOCK:
        revision = STATE["revision"]
        count = STATE["tracks"]
    base = abs(hash(uri)) % 1000000
    tracks = [make_track(f"spotify:track:{base + i:022d}", i) for i in range(count)]
    context = {
        "uri": uri,
        "name": context_name(uri),
        "owner": "Fake Owner",
        "image_url": None,
        "revision": revision,
        "track_count": count,
    }
    return {"ok": True, "tracks": tracks, "context": context}


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

        if ARGS.logged_out and op in ("info", "resolve", "open", "library", "search", "artist",
                                       "saved", "save", "unsave", "playlist_add", "radio"):
            write_all(handle, (json.dumps(error_reply("not_logged_in", "not logged in")) + "\n").encode("utf-8"))
            return

        if op == "info":
            uris = request.get("uris", [])
            bitrate = int(request.get("bitrate", 320))
            tracks = [make_track(uri, i, bitrate) for i, uri in enumerate(uris)]
            write_all(handle, (json.dumps({"ok": True, "tracks": tracks}) + "\n").encode("utf-8"))
            return

        if op == "resolve":
            reply = resolve_reply(request.get("uri", ""))
            log(f"resolved {len(reply['tracks'])} track(s)")
            write_all(handle, (json.dumps(reply) + "\n").encode("utf-8"))
            return

        if op == "library":
            write_all(handle, (json.dumps(make_library()) + "\n").encode("utf-8"))
            return

        if op == "search":
            write_all(handle, (json.dumps(make_search(request.get("query", ""),
                                                      int(request.get("limit", 20)))) + "\n").encode("utf-8"))
            return

        if op == "artist":
            write_all(handle, (json.dumps(make_artist(request.get("uri", ""))) + "\n").encode("utf-8"))
            return

        if op in ("saved", "save", "unsave"):
            uris = request.get("uris")
            if not isinstance(uris, list):
                write_all(handle, (json.dumps(error_reply("bad_request", "uris required")) + "\n").encode("utf-8"))
                return
            with STATE_LOCK:
                if op == "saved":
                    reply = {"ok": True, "saved": [u in STATE["saved"] for u in uris]}
                else:
                    for u in uris:
                        if op == "save":
                            STATE["saved"].add(u)
                        else:
                            STATE["saved"].discard(u)
                    STATE["revision"] = str(int(STATE["revision"]) + 1)
                    reply = {"ok": True}
            write_all(handle, (json.dumps(reply) + "\n").encode("utf-8"))
            return

        if op == "playlist_add":
            if request.get("playlist") != "spotify:playlist:user000000000000000001":
                write_all(handle, (json.dumps(error_reply("bad_request", "you cannot edit this playlist")) + "\n").encode("utf-8"))
                return
            with STATE_LOCK:
                STATE["playlist_adds"] += 1
                revision = "pa%d" % STATE["playlist_adds"]
            write_all(handle, (json.dumps({"ok": True, "revision": revision}) + "\n").encode("utf-8"))
            return

        if op == "radio":
            uri = request.get("uri", "")
            if uri == "spotify:track:unsupported":
                write_all(handle, (json.dumps(error_reply("unsupported", "Spotify refused radio for this login")) + "\n").encode("utf-8"))
                return
            tracks = [make_track("", i) for i in range(4)]
            context = {"uri": "spotify:station:" + uri, "name": "Fake Track 1 Radio", "owner": "Spotify",
                       "image_url": None, "revision": "r1", "track_count": 4}
            write_all(handle, (json.dumps({"ok": True, "tracks": tracks, "context": context}) + "\n").encode("utf-8"))
            return

        if op == "test_control":
            with STATE_LOCK:
                if "revision" in request:
                    STATE["revision"] = str(request["revision"])
                if "tracks" in request:
                    STATE["tracks"] = int(request["tracks"])
                reply = {"ok": True, "revision": STATE["revision"], "tracks": STATE["tracks"]}
            write_all(handle, (json.dumps(reply) + "\n").encode("utf-8"))
            return

        if op == "open":
            uri = request.get("uri", "")
            bitrate = int(request.get("bitrate", 320))
            start_ms = int(request.get("start_ms", 0))
            start_frame = round(start_ms * ARGS.rate / 1000.0)
            total_frames = round(ARGS.duration_ms * ARGS.rate / 1000.0)
            header = {
                "ok": True,
                "track": make_track(uri, 0, bitrate),
                "sample_rate": ARGS.rate,
                "channels": ARGS.channels,
                "start_frame": start_frame,
            }
            log(f"open start_ms={start_ms} start_frame={start_frame}")
            write_all(handle, (json.dumps(header) + "\n").encode("utf-8"))
            stream_pcm(handle, start_frame, total_frames)
            # Closing a pipe with unread data discards the tail; wait for the
            # client to drain it first.
            win32file.FlushFileBuffers(handle)
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
    parser.add_argument("--revision", default="1")
    parser.add_argument("--tracks", type=int, default=3)
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--parent-pid", type=int, default=0)
    ARGS = parser.parse_args()
    STATE["revision"] = ARGS.revision
    STATE["tracks"] = ARGS.tracks

    log(f"listening on \\\\.\\pipe\\{ARGS.pipe} (duration {ARGS.duration_ms} ms, "
        f"{ARGS.rate} Hz, {ARGS.channels} ch{', logged out' if ARGS.logged_out else ''})")
    try:
        serve()
    except KeyboardInterrupt:
        return 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
