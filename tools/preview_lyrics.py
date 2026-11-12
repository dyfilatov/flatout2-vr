# Plays FlatOut 2's soundtrack from the installed data.bfs and shows the local .lrc
# files in time, so lyrics can be checked without starting the game.
#
#   python tools\preview_lyrics.py [--game "C:\Path\To\FlatOut 2"] [--lyrics PATH] [--port 8765]
#   python tools\preview_lyrics.py --check
#
# The Steam and GOG installs keep the songs inside data.bfs (there is no loose
# data\songs1 folder). Stunt copies (data/songs2) are labelled "Henri" in the game
# and start part-way through; the player uses the songs1 title for those and can
# seek to the stunt StartPos. Notes are saved to lyrics-review.json next to the .lrc files.

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import threading
import webbrowser
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse

# songs2/NN is the stunt edit of a songs1 track. The stunt playlist names every one "Henri".
STUNT_OF = {
    "25": "03",
    "26": "05",
    "27": "10",
    "28": "13",
    "29": "14",
    "30": "15",
    "31": "16",
    "32": "17",
    "33": "18",
    "34": "21",
}

PLAYLIST_WHERE = {
    "playlist_title.bed": "меню",
    "playlist_ingame.bed": "гонки",
    "playlist_stunt.bed": "трюки",
}

HERE = Path(__file__).resolve().parent
HTML_PATH = HERE / "lyrics_preview.html"


def norm_name(value: str) -> str:
    return re.sub(r"\s+", " ", value or "").strip().casefold()


def read_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith(b"\xff\xfe"):
        return data[2:].decode("utf-16-le", errors="replace")
    if data.startswith(b"\xfe\xff"):
        return data[2:].decode("utf-16-be", errors="replace")
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8", errors="replace")
    return data.decode("utf-8", errors="replace")


def lrc_summary(text: str) -> dict:
    artist = ""
    title = ""
    first = ""
    for line in text.splitlines():
        stripped = line.strip()
        meta = re.match(r"^\[([A-Za-z]+):(.*)\]\s*$", stripped)
        if meta:
            key = meta.group(1).lower()
            if key == "ar" and not artist:
                artist = meta.group(2).strip()
            elif key == "ti" and not title:
                title = meta.group(2).strip()
            continue
        timed = re.match(r"^(?:\[\d+:\d+(?:\.\d+)?\])+(.*)$", stripped)
        if not timed or first:
            continue
        body = re.sub(r"<\d+:\d+(?:\.\d+)?>", "", timed.group(1)).strip()
        if body:
            first = body[:160]
    return {"artist": artist, "title": title, "first": first}


def parse_playlist(text: str, where: str) -> list:
    entries = []
    for block in re.split(r"\[\d+\]\s*=\s*\{", text)[1:]:
        def field(name: str) -> str:
            match = re.search(rf"{name}\s*=\s*\"([^\"]*)\"", block)
            return match.group(1) if match else ""

        start = re.search(r"StartPos\s*=\s*(-?\d+)", block)
        path = field("File").replace("\\", "/")
        if not path:
            continue
        entries.append({
            "where": where,
            "file": path,
            "artist": field("Artist").strip(),
            "title": field("Song").strip(),
            "startMs": int(start.group(1)) if start else 0,
        })
    return entries


class BfsFile:
    def __init__(self, archive: Path, flags: int, offset: int, unpacked: int, packed: int):
        self.archive = archive
        self.flags = flags
        self.offset = offset
        self.unpacked = unpacked
        self.packed = packed

    def read(self) -> bytes:
        with self.archive.open("rb") as handle:
            handle.seek(self.offset)
            blob = handle.read(self.packed)
        if self.flags & 0x08:
            raise RuntimeError(f"{self.archive.name}: zstd compression is not used by the base game")
        if self.flags & 0x01:
            blob = zlib.decompress(blob)
        if len(blob) != self.unpacked:
            raise RuntimeError(
                f"{self.archive.name}: unpacked {len(blob)} bytes, header says {self.unpacked}"
            )
        return blob


def _decode_huffman_names(name_offsets, name_lengths, nodes, huff) -> list:
    table = {}
    queue = []

    def visit(key: int, position: int) -> None:
        if position < 0 or position >= len(nodes):
            return
        value, node_type = nodes[position]
        if node_type == 0x00:
            queue.append(((key << 1) | 1, position + 1))
            queue.append((key << 1, value))
        elif node_type == 0x80:
            table[key] = value

    visit(1, 0)
    while queue:
        item_key, item_pos = queue.pop()
        visit(item_key, item_pos)

    names = []
    for index, (offset, length) in enumerate(zip(name_offsets, name_lengths)):
        end = name_offsets[index + 1] if index + 1 < len(name_offsets) else len(huff)
        encoded = huff[offset:end]
        pattern = 1
        out = bytearray()
        for byte in encoded:
            for bit in range(8):
                pattern = (pattern << 1) | ((byte >> bit) & 1)
                decoded = table.get(pattern)
                if decoded is None:
                    continue
                out.append(decoded)
                pattern = 1
                if len(out) >= length:
                    break
            if len(out) >= length:
                break
        names.append(out.decode("latin1", errors="replace"))
    return names


def read_bfs_index(path: Path) -> dict:
    """Map lowercase archive path -> BfsFile. Filenames are Huffman-coded (BFS 2004.05.05b)."""
    files = {}
    with path.open("rb") as handle:
        magic, _version, header_end, file_count = struct.unpack("<4sIII", handle.read(16))
        if magic != b"bfs1":
            raise RuntimeError(f"{path.name} is not a bfs1 archive")
        hash_size = struct.unpack("<I", handle.read(4))[0]
        if hash_size <= 0 or hash_size > 100000:
            raise RuntimeError(f"{path.name}: unexpected hash table size {hash_size}")
        handle.seek(hash_size * 8, 1)
        meta_start = handle.tell()
        fh_off, name_off, name_len, dict_off, data_off = struct.unpack("<5I", handle.read(20))

        def absolute(offset: int) -> int:
            return meta_start + offset

        sections = sorted({
            header_end,
            absolute(fh_off),
            absolute(name_off),
            absolute(name_len),
            absolute(dict_off),
            absolute(data_off),
        })

        def span(start: int) -> int:
            for left, right in zip(sections, sections[1:]):
                if left == start:
                    return right - left
            return 0

        def read_at(start: int) -> bytes:
            handle.seek(start)
            return handle.read(span(start))

        name_off_bytes = read_at(absolute(name_off))
        name_len_bytes = read_at(absolute(name_len))
        raw_dict = read_at(absolute(dict_off))
        huff = read_at(absolute(data_off))
        name_offsets = list(struct.unpack("<" + "I" * (len(name_off_bytes) // 4), name_off_bytes))
        name_lengths = list(struct.unpack("<" + "H" * (len(name_len_bytes) // 2), name_len_bytes))
        nodes = [(raw_dict[i], raw_dict[i + 1]) for i in range(0, len(raw_dict) - 1, 2)]
        names = _decode_huffman_names(name_offsets, name_lengths, nodes, huff)

        handle.seek(absolute(fh_off))
        for _ in range(file_count):
            head = handle.read(24)
            if len(head) < 24:
                break
            flags = head[0]
            copies = head[1]
            data_offset, unpacked, packed, _crc = struct.unpack_from("<IIII", head, 4)
            folder_id, file_id = struct.unpack_from("<HH", head, 20)
            if copies:
                handle.read(copies * 4)
            if folder_id >= len(names) or file_id >= len(names):
                continue
            key = (names[folder_id] + "/" + names[file_id]).replace("\\", "/").lower()
            files[key] = BfsFile(path, flags, data_offset, unpacked, packed)
    return files


def find_game(explicit: str | None) -> Path:
    if explicit:
        game = Path(explicit).expanduser()
        if not (game / "FlatOut2.exe").is_file():
            raise SystemExit(f"FlatOut2.exe not found in {game}")
        return game.resolve()

    candidates = []
    parent = HERE.parent
    candidates.append(parent)
    candidates.append(parent.parent)
    extra = parent / "extra-game-dirs.txt"
    if extra.is_file():
        for line in extra.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                candidates.append(Path(line))
    candidates.extend([
        Path(r"C:\Program Files (x86)\Steam\steamapps\common\FlatOut 2"),
        Path(r"C:\Program Files\Steam\steamapps\common\FlatOut 2"),
        Path(r"C:\GOG Games\FlatOut 2"),
    ])
    seen = set()
    for candidate in candidates:
        try:
            resolved = candidate.resolve()
        except OSError:
            continue
        if resolved in seen:
            continue
        seen.add(resolved)
        if (resolved / "FlatOut2.exe").is_file():
            return resolved
    raise SystemExit(
        "Could not find FlatOut2.exe. Pass --game \"C:\\Path\\To\\FlatOut 2\"."
    )


def archive_paths(game: Path) -> list:
    names = []
    listing = game / "filesystem"
    if listing.is_file():
        for line in listing.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and not line.startswith(";"):
                names.append(line)
    if not names:
        names = ["data.bfs"]
    paths = []
    for name in names:
        candidate = game / name
        if candidate.is_file():
            paths.append(candidate)
    if not paths:
        raise SystemExit(f"No BFS archives listed in {game}")
    return paths


def find_lyrics(game: Path, explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).expanduser().resolve()
    options = [
        game / "scripts" / "lyrics",
        HERE / "lyrics",
        HERE.parent / "build" / "lyrics",
        HERE.parent / "scripts" / "lyrics",
    ]
    with_files = [path for path in options if path.is_dir() and any(path.glob("*.lrc"))]
    if with_files:
        return with_files[0].resolve()
    for path in options:
        if path.is_dir():
            return path.resolve()
    return (game / "scripts" / "lyrics").resolve()


def load_archives(game: Path) -> dict:
    merged = {}
    for archive in archive_paths(game):
        try:
            indexed = read_bfs_index(archive)
        except Exception as error:
            print(f"Skipping {archive.name}: {error}", file=sys.stderr)
            continue
        merged.update(indexed)
        print(f"{archive.name}: {len(indexed)} files")
    return merged


def build_tracks(game: Path, lyrics_dir: Path, review_path: Path) -> list:
    archives = load_archives(game)
    places = {}
    for bed_name, where in PLAYLIST_WHERE.items():
        key = f"data/music/{bed_name}"
        record = archives.get(key)
        if record is None:
            print(f"Missing {key}", file=sys.stderr)
            continue
        for entry in parse_playlist(record.read().decode("latin1", errors="replace"), where):
            places.setdefault(entry["file"].lower(), []).append(entry)

    review = load_review(review_path)
    tracks = []
    for key, record in archives.items():
        match = re.match(r"data/songs([12])/(\d+)\.ogg$", key)
        if not match:
            continue
        stem = match.group(2)
        lrc_path = lyrics_dir / f"{stem}.lrc"
        summary = lrc_summary(read_text(lrc_path)) if lrc_path.is_file() else None
        note = review.get(stem, {})
        tracks.append({
            "file": stem,
            "path": key,
            "size": record.unpacked,
            "places": [
                {
                    "where": place["where"],
                    "artist": place["artist"],
                    "title": place["title"],
                    "startMs": place["startMs"],
                }
                for place in places.get(key, [])
            ],
            "hasLrc": summary is not None,
            "lrcArtist": summary["artist"] if summary else "",
            "lrcTitle": summary["title"] if summary else "",
            "firstLine": summary["first"] if summary else "",
            "wrong": bool(note.get("wrong")),
            "note": note.get("note") or "",
            "_record": record,
        })
    tracks.sort(key=lambda item: int(item["file"]))
    by_file = {item["file"]: item for item in tracks}
    for item in tracks:
        real = [place for place in item["places"] if norm_name(place["artist"]) != "henri"]
        sibling = STUNT_OF.get(item["file"])
        if real:
            item["artist"] = real[0]["artist"]
            item["title"] = real[0]["title"]
            item["sameAs"] = None
        elif sibling and sibling in by_file and by_file[sibling].get("artist"):
            item["artist"] = by_file[sibling]["artist"]
            item["title"] = by_file[sibling]["title"]
            item["sameAs"] = sibling
        elif item["places"]:
            item["artist"] = item["places"][0]["artist"]
            item["title"] = item["places"][0]["title"]
            item["sameAs"] = None
        else:
            item["artist"] = ""
            item["title"] = ""
            item["sameAs"] = None
        item["tagMismatch"] = bool(item["hasLrc"]) and (
            norm_name(item["lrcArtist"]) != norm_name(item["artist"])
            or norm_name(item["lrcTitle"]) != norm_name(item["title"])
        )
    return tracks


def public_track(item: dict) -> dict:
    return {key: value for key, value in item.items() if key != "_record"}


def load_review(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def save_review(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


class App:
    def __init__(self, game: Path, lyrics_dir: Path, review_path: Path, tracks: list):
        self.game = game
        self.lyrics_dir = lyrics_dir
        self.review_path = review_path
        self.tracks = tracks
        self.by_file = {item["file"]: item for item in tracks}
        self.audio_cache = {}
        self.cache_lock = threading.Lock()
        self.review_lock = threading.Lock()

    def audio(self, stem: str) -> bytes:
        item = self.by_file.get(stem)
        if item is None:
            raise KeyError(stem)
        with self.cache_lock:
            cached = self.audio_cache.get(stem)
            if cached is not None:
                return cached
            data = item["_record"].read()
            self.audio_cache[stem] = data
            return data

    def lyrics_text(self, stem: str) -> str | None:
        path = self.lyrics_dir / f"{stem}.lrc"
        if not path.is_file():
            return None
        return read_text(path)

    def set_review(self, stem: str, wrong: bool, note: str) -> dict:
        if stem not in self.by_file:
            raise KeyError(stem)
        with self.review_lock:
            review = load_review(self.review_path)
            if wrong or note.strip():
                review[stem] = {"wrong": bool(wrong), "note": note}
            else:
                review.pop(stem, None)
            save_review(self.review_path, review)
        item = self.by_file[stem]
        item["wrong"] = bool(wrong) if (wrong or note.strip()) else False
        item["note"] = note if (wrong or note.strip()) else ""
        return {"file": stem, "wrong": item["wrong"], "note": item["note"]}


APP: App | None = None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args) -> None:
        target = args[0] if args else ""
        if isinstance(target, str) and (" /audio/" in target or target.startswith("GET /audio/")):
            return
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, status: int, body: bytes, content_type: str, extra: dict | None = None) -> None:
        try:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Accept-Ranges", "bytes")
            for key, value in (extra or {}).items():
                self.send_header(key, value)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            pass

    def _send_range(self, data: bytes, content_type: str) -> None:
        total = len(data)
        start, end = 0, total - 1
        status = 200
        header = self.headers.get("Range")
        extra = {}
        if header and header.startswith("bytes="):
            spec = header[6:].split(",", 1)[0].strip()
            left, sep, right = spec.partition("-")
            try:
                if sep != "-":
                    raise ValueError
                if left == "":
                    suffix = int(right)
                    if suffix <= 0:
                        raise ValueError
                    start = max(0, total - suffix)
                    end = total - 1
                else:
                    start = int(left)
                    end = int(right) if right else total - 1
            except ValueError:
                start = total
            if start >= total or start < 0 or end < start:
                self._send(416, b"", "text/plain", {"Content-Range": f"bytes */{total}"})
                return
            end = min(end, total - 1)
            status = 206
            extra["Content-Range"] = f"bytes {start}-{end}/{total}"
        self._send(status, data[start:end + 1], content_type, extra)

    def _json(self, payload, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self._send(status, body, "application/json; charset=utf-8")

    def do_HEAD(self) -> None:
        self.do_GET()

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        app = APP
        if app is None:
            self._send(500, b"not ready", "text/plain")
            return
        if path in ("/", "/index.html"):
            if not HTML_PATH.is_file():
                self._send(500, b"lyrics_preview.html is missing", "text/plain")
                return
            self._send(200, HTML_PATH.read_bytes(), "text/html; charset=utf-8")
            return
        if path == "/favicon.ico":
            self._send(204, b"", "image/x-icon")
            return
        if path == "/api/tracks":
            self._json({
                "game": str(app.game),
                "lyrics": str(app.lyrics_dir),
                "review": str(app.review_path),
                "tracks": [public_track(item) for item in app.tracks],
            })
            return
        audio = re.fullmatch(r"/audio/(\d{2})", path)
        if audio:
            stem = audio.group(1)
            if stem not in app.by_file:
                self._send(404, b"no such track", "text/plain")
                return
            try:
                self._send_range(app.audio(stem), "audio/ogg")
            except Exception as error:
                self._send(500, str(error).encode("utf-8", errors="replace"), "text/plain; charset=utf-8")
            return
        lyrics = re.fullmatch(r"/lyrics/(\d{2})", path)
        if lyrics:
            text = app.lyrics_text(lyrics.group(1))
            if text is None:
                self._send(404, b"no lyrics", "text/plain")
                return
            self._send(200, text.encode("utf-8"), "text/plain; charset=utf-8")
            return
        self._send(404, b"not found", "text/plain")

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path != "/api/review":
            self._send(404, b"not found", "text/plain")
            return
        app = APP
        length = int(self.headers.get("Content-Length") or "0")
        if length <= 0 or length > 65536:
            self._json({"error": "bad length"}, 400)
            return
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            stem = str(payload.get("file", ""))
            if not re.fullmatch(r"\d{2}", stem):
                raise KeyError(stem)
            saved = app.set_review(stem, bool(payload.get("wrong")), str(payload.get("note") or ""))
        except KeyError:
            self._json({"error": "unknown track"}, 404)
            return
        except (json.JSONDecodeError, TypeError, ValueError) as error:
            self._json({"error": str(error)}, 400)
            return
        except OSError as error:
            self._json({"error": str(error)}, 500)
            return
        self._json(saved)


class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def run_check(game: Path) -> int:
    archives = load_archives(game)
    songs = sorted(key for key in archives if re.match(r"data/songs[12]/\d+\.ogg$", key))
    problems = []
    if len(songs) != 34:
        problems.append(f"expected 34 ogg files, found {len(songs)}")
    for required in ("data/songs1/01.ogg", "data/songs2/25.ogg", "data/music/playlist_ingame.bed"):
        if required not in archives:
            problems.append(f"missing {required}")
    sample = archives.get("data/songs1/01.ogg")
    if sample is not None:
        head = sample.read()[:4]
        if head != b"OggS":
            problems.append(f"01.ogg does not start with OggS ({head!r})")
    ingame = archives.get("data/music/playlist_ingame.bed")
    if ingame is not None:
        entries = parse_playlist(ingame.read().decode("latin1", errors="replace"), "гонки")
        titles = {(entry["file"].lower(), entry["title"]) for entry in entries}
        if ("data/songs1/01.ogg", "Nowhere Ride") not in titles:
            problems.append("playlist_ingame.bed did not contain Nowhere Ride")
        if len(entries) < 19:
            problems.append(f"playlist_ingame.bed has {len(entries)} entries")
    stunt = archives.get("data/music/playlist_stunt.bed")
    if stunt is not None:
        entries = parse_playlist(stunt.read().decode("latin1", errors="replace"), "трюки")
        if not any(entry["file"].lower() == "data/songs2/25.ogg" and entry["startMs"] > 0 for entry in entries):
            problems.append("playlist_stunt.bed has no StartPos for 25.ogg")
    if problems:
        print("CHECK FAILED")
        for problem in problems:
            print(" -", problem)
        return 1
    print(f"CHECK OK: {len(songs)} soundtrack files, playlists readable, 01.ogg is Ogg Vorbis")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check FlatOut 2 lyrics against the game's own music.")
    parser.add_argument("--game", help="Folder that contains FlatOut2.exe")
    parser.add_argument("--lyrics", help="Folder of .lrc files (default: the game's scripts\\lyrics)")
    parser.add_argument("--review", help="Where to save wrong-song notes (default: lyrics-review.json beside the .lrc files)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--check", action="store_true", help="Read the archives and exit")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    global APP
    args = parse_args(argv)
    game = find_game(args.game)
    print(f"Game: {game}")
    if args.check:
        return run_check(game)
    lyrics_dir = find_lyrics(game, args.lyrics)
    review_path = Path(args.review).expanduser().resolve() if args.review else lyrics_dir / "lyrics-review.json"
    print(f"Lyrics: {lyrics_dir}")
    print(f"Notes:  {review_path}")
    tracks = build_tracks(game, lyrics_dir, review_path)
    if not tracks:
        raise SystemExit("No soundtrack .ogg files found in the game's BFS archives.")
    lrc_count = sum(1 for item in tracks if item["hasLrc"])
    print(f"Tracks: {len(tracks)}, with lyrics: {lrc_count}")
    APP = App(game, lyrics_dir, review_path, tracks)

    port = args.port
    httpd = None
    last_error = None
    for candidate in range(port, port + 20):
        try:
            httpd = Server(("127.0.0.1", candidate), Handler)
            port = candidate
            break
        except OSError as error:
            last_error = error
    if httpd is None:
        raise SystemExit(f"Could not listen near port {args.port}: {last_error}")

    url = f"http://127.0.0.1:{port}/"
    print(f"Open {url}")
    print("Close this window to stop.")
    if not args.no_browser:
        threading.Timer(0.3, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
