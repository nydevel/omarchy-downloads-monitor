#!/usr/bin/env python3
"""Background download monitor for the Omarchy bar.

Watches known downloader processes (package managers, game launchers,
umu/Proton, curl, git, ...) and prints one JSON snapshot per line on stdout
whenever the picture changes. Everything is read locally from /proc and the
file system: no network access, no root, no third-party modules.

Usage:
  downloads.py [--interval SECONDS] [--watch name1,name2] [--once] [--demo]
"""

import argparse
import fcntl
import json
import os
import pwd
import re
import struct
import sys
import time

PROTOCOL_VERSION = 1
MAX_JOBS = 20
MAX_TEXT = 160

# comm name (as in /proc/<pid>/comm, max 15 chars) -> kind
WATCHED = {
    "pacman": "pacman",
    "yay": "aur",
    "paru": "aur",
    "pamac": "aur",
    "makepkg": "aur",
    "flatpak": "flatpak",
    "umu-run": "umu",
    "lutris": "lutris",
    "heroic": "heroic",
    "steam": "steam",
    "curl": "curl",
    "wget": "wget",
    "wget2": "wget",
    "aria2c": "aria2",
    "git": "git",
    "git-remote-http": "git",
    "yt-dlp": "ytdlp",
    "pip": "pip",
    "pip3": "pip",
    "uv": "pip",
    "npm": "npm",
    "pnpm": "npm",
    "bun": "npm",
    "cargo": "cargo",
    "rustup": "cargo",
    "mise": "mise",
}

KIND_TITLES = {
    "pacman": "pacman",
    "aur": "AUR",
    "flatpak": "Flatpak",
    "umu": "Proton (umu)",
    "lutris": "Lutris",
    "heroic": "Heroic",
    "steam": "Steam",
    "curl": "curl",
    "wget": "wget",
    "aria2": "aria2",
    "git": "git",
    "ytdlp": "yt-dlp",
    "pip": "Python",
    "npm": "Node.js",
    "cargo": "Rust",
    "mise": "mise",
}

# umu GAMEID (without the "umu-" prefix) -> display name
KNOWN_GAMEIDS = {
    "battlenet": "Battle.net",
    "eaapp": "EA app",
    "ubisoftconnect": "Ubisoft Connect",
    "epicgameslauncher": "Epic Games",
    "gog": "GOG Galaxy",
}

PARTIAL_SUFFIXES = (".part", ".parts", ".partial", ".download", ".crdownload",
                    ".aria2", ".tmp", ".!qb", ".incomplete")
PARTIAL_MARKERS = ("tmp_pack_", "/downloading/", "/download-")
IGNORED_PREFIXES = ("/dev/", "/proc/", "/sys/", "/run/", "/memfd:", "/tmp/.X")
IGNORED_SUFFIXES = (".log", ".lock", ".lck", "-journal", "-wal", "-shm", ".sqlite",
                    ".db", ".reg", ".pid")

# A job counts as downloading when its files grow this fast...
MIN_RATE = 96 * 1024
# ...or when it writes partial-download files at all.
MIN_PARTIAL_RATE = 1024
# Fallback for tools that rewrite preallocated files or open them only in
# bursts (Battle.net, some launchers): disk writes of the process tree plus
# network traffic, both at least this fast.
MIN_IO_RATE = 512 * 1024
MIN_IO_RX = 512 * 1024
# Linger after the last growth before hiding (seconds).
LINGER = 4.0
# How long after a download a pure disk-write phase (extracting) keeps showing.
WRITE_AFTER_DOWNLOAD = 30.0

EMA = 0.45


def clip(text, limit=MAX_TEXT):
    text = "".join(ch for ch in str(text) if ch.isprintable())
    return text if len(text) <= limit else text[: limit - 1] + "…"


def ema(old, new):
    return new if old is None else old + (new - old) * EMA


# ---------------------------------------------------------------- /proc


def read_text(path, limit=65536):
    try:
        with open(path, "rb") as fh:
            return fh.read(limit).decode("utf-8", "replace")
    except OSError:
        return None


def process_table():
    """pid -> (comm, ppid, uid)"""
    table = {}
    for name in os.listdir("/proc"):
        if not name.isdigit():
            continue
        pid = int(name)
        stat = read_text(f"/proc/{pid}/stat", 1024)
        if not stat:
            continue
        lpar, rpar = stat.find("("), stat.rfind(")")
        if lpar < 0 or rpar < 0:
            continue
        comm = stat[lpar + 1 : rpar]
        fields = stat[rpar + 2 :].split()
        try:
            ppid = int(fields[1])
            uid = os.stat(f"/proc/{pid}").st_uid
        except (IndexError, ValueError, OSError):
            continue
        table[pid] = (comm, ppid, uid)
    return table


def read_environ_value(pid, key):
    raw = read_text(f"/proc/{pid}/environ", 262144)
    if not raw:
        return None
    prefix = key + "="
    for item in raw.split("\0"):
        if item.startswith(prefix):
            return item[len(prefix) :]
    return None


def net_rx_bytes():
    raw = read_text("/proc/net/dev")
    total = 0
    if not raw:
        return 0
    for line in raw.splitlines()[2:]:
        if ":" not in line:
            continue
        iface, data = line.split(":", 1)
        iface = iface.strip()
        if iface == "lo" or iface.startswith(("veth", "docker", "virbr", "br-")):
            continue
        try:
            total += int(data.split()[0])
        except (IndexError, ValueError):
            pass
    return total


def write_bytes(pid):
    raw = read_text(f"/proc/{pid}/io", 1024)
    if not raw:
        return None
    match = re.search(r"^write_bytes:\s*(\d+)", raw, re.M)
    return int(match.group(1)) if match else None


def is_partial(path):
    return path.endswith(PARTIAL_SUFFIXES) or any(m in path for m in PARTIAL_MARKERS)


def interesting_path(path):
    if not path.startswith("/") or path.endswith(" (deleted)"):
        return False
    if path.startswith(IGNORED_PREFIXES):
        return False
    return not path.endswith(IGNORED_SUFFIXES)


def fd_writable(pid, fd):
    raw = read_text(f"/proc/{pid}/fdinfo/{fd}", 512)
    if not raw:
        return False
    match = re.search(r"^flags:\s*([0-7]+)", raw, re.M)
    return bool(match) and (int(match.group(1), 8) & 3) in (1, 2)


# ---------------------------------------------------------------- totals


def aria2_total(path):
    """Total length from an aria2 control file next to the download."""
    control = path if path.endswith(".aria2") else path + ".aria2"
    try:
        with open(control, "rb") as fh:
            head = fh.read(4096)
    except OSError:
        return None
    if len(head) < 10:
        return None
    version = struct.unpack(">H", head[:2])[0]
    order = ">" if version == 1 else "<"
    try:
        (hash_len,) = struct.unpack(order + "I", head[6:10])
        offset = 10 + hash_len + 4
        (total,) = struct.unpack(order + "Q", head[offset : offset + 8])
    except struct.error:
        return None
    return total if 0 < total < 1 << 50 else None


def strip_partial(name):
    for suffix in PARTIAL_SUFFIXES:
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


# ---------------------------------------------------------------- Steam


def parse_vdf_pairs(text):
    return re.findall(r'"([^"\\]*)"\s+"([^"\\]*)"', text or "")


class SteamCollector:
    ROOTS = ("~/.local/share/Steam", "~/.steam/steam",
             "~/.var/app/com.valvesoftware.Steam/.local/share/Steam")

    def __init__(self):
        self.libraries = []
        self.libraries_at = 0
        self.apps = {}  # appid -> dict(state)

    def library_paths(self, now):
        if now - self.libraries_at < 30 and self.libraries:
            return self.libraries
        paths = []
        for root in self.ROOTS:
            root = os.path.expanduser(root)
            vdf = os.path.join(root, "steamapps", "libraryfolders.vdf")
            text = read_text(vdf, 1 << 20)
            if text is None:
                continue
            found = [v for k, v in parse_vdf_pairs(text) if k == "path"] or [root]
            for path in found:
                apps = os.path.join(path, "steamapps")
                if os.path.isdir(apps) and apps not in paths:
                    paths.append(apps)
        self.libraries, self.libraries_at = paths, now
        return paths

    def collect(self, now):
        jobs = []
        seen = set()
        for apps in self.library_paths(now):
            try:
                names = os.listdir(apps)
            except OSError:
                continue
            for name in names:
                if not (name.startswith("appmanifest_") and name.endswith(".acf")):
                    continue
                data = dict(parse_vdf_pairs(read_text(os.path.join(apps, name), 1 << 16)))
                try:
                    todo = int(data.get("BytesToDownload", 0))
                    done = int(data.get("BytesDownloaded", 0))
                except ValueError:
                    continue
                appid = data.get("appid") or name[12:-4]
                if todo <= 0 or done >= todo:
                    self.apps.pop(appid, None)
                    continue
                seen.add(appid)
                state = self.apps.setdefault(appid, {"done": done, "t": now, "rate": None,
                                                     "grew": 0.0, "first": now})
                dt = now - state["t"]
                if done != state["done"] and dt > 0:
                    state["rate"] = ema(state["rate"], max(0, done - state["done"]) / dt)
                    state["grew"] = now
                    state["done"], state["t"] = done, now
                elif dt > 15:
                    state["rate"] = 0.0
                if now - state["grew"] > 20:
                    continue  # paused or queued
                jobs.append({
                    "id": f"steam:{appid}",
                    "kind": "steam",
                    "title": "Steam",
                    "detail": clip(data.get("name") or appid),
                    "state": "downloading",
                    "done": done,
                    "total": todo,
                    "rate": round(state["rate"] or 0),
                    "approx": False,
                    "files": 0,
                })
        for appid in list(self.apps):
            if appid not in seen:
                del self.apps[appid]
        return jobs


# ---------------------------------------------------------------- monitor


class Monitor:
    def __init__(self, extra_watch=()):
        self.watched = dict(WATCHED)
        for name in extra_watch:
            name = name.strip()[:15]
            if name:
                self.watched.setdefault(name, "other")
        self.uid = os.getuid()
        try:
            self.alpm_uid = pwd.getpwnam("alpm").pw_uid
        except KeyError:
            self.alpm_uid = None
        self.fd_cache = {}    # (pid, fd) -> (path, writable)
        self.files = {}       # path -> {"size", "t", "grew", "partial", "rate"}
        self.jobs = {}        # root pid -> job state
        self.steam = SteamCollector()
        self.last_rx = None
        self.rx_rate = None
        self.last_t = None
        self.finished = []

    # -- process grouping

    def group(self, table):
        children = {}
        for pid, (_comm, ppid, _uid) in table.items():
            children.setdefault(ppid, []).append(pid)

        def watched(pid):
            return table[pid][0] in self.watched

        roots = set()
        for pid in table:
            if not watched(pid):
                continue
            root, cur, hops = pid, table[pid][1], 0
            while cur in table and hops < 32:
                if watched(cur):
                    root = cur
                cur, hops = table[cur][1], hops + 1
            roots.add(root)

        groups = {}
        for root in roots:
            members, stack = [], [root]
            while stack and len(members) < 4096:
                pid = stack.pop()
                members.append(pid)
                stack.extend(children.get(pid, ()))
            groups[root] = members
        return groups

    # -- files

    def open_files(self, pid):
        out = []
        try:
            fds = os.listdir(f"/proc/{pid}/fd")
        except OSError:
            return None
        for fd in fds:
            key = (pid, fd)
            try:
                path = os.readlink(f"/proc/{pid}/fd/{fd}")
            except OSError:
                continue
            cached = self.fd_cache.get(key)
            if cached is None or cached[0] != path:
                writable = interesting_path(path) and fd_writable(pid, fd)
                cached = (path, writable)
                self.fd_cache[key] = cached
            if cached[1]:
                out.append(path)
        return out

    def sample_file(self, path, now):
        try:
            size = os.stat(path).st_size
        except OSError:
            return None
        info = self.files.get(path)
        if info is None:
            info = {"size": size, "t": now, "grew": 0.0, "rate": None,
                    "partial": is_partial(path), "seen": now}
            self.files[path] = info
            return info, 0
        growth = max(0, size - info["size"])
        dt = max(1e-3, now - info["t"])
        info["rate"] = ema(info["rate"], growth / dt)
        if growth:
            info["grew"] = now
        info["size"], info["t"], info["seen"] = size, now, now
        return info, growth

    # -- one tick

    def tick(self, now):
        dt = None if self.last_t is None else max(1e-3, now - self.last_t)
        rx = net_rx_bytes()
        if self.last_rx is not None and dt:
            self.rx_rate = ema(self.rx_rate, max(0, rx - self.last_rx) / dt)
        self.last_rx, self.last_t = rx, now

        table = process_table()
        groups = self.group(table)
        live_pids = set(table)
        self.fd_cache = {k: v for k, v in self.fd_cache.items() if k[0] in live_pids}

        out = []
        steam_running = False
        for root, members in groups.items():
            kind = self.watched.get(table[root][0], "other")
            if kind == "steam":
                steam_running = True
                continue  # Steam is read from its app manifests instead
            job = self.jobs.setdefault(root, {
                "kind": kind, "first": now, "grew": 0.0, "downloaded": 0.0,
                "rate": None, "visible_since": None, "title": self.title_for(root, kind, table),
                "io": {}, "io_rate": None, "detail": "", "done": None,
            })
            files, growth, unreadable, alpm = [], 0, False, False
            io_now, io_growth = {}, 0
            for pid in members:
                comm, _ppid, uid = table[pid]
                if comm == "steam":
                    steam_running = True
                if self.alpm_uid is not None and uid == self.alpm_uid:
                    alpm = True
                if uid != self.uid:
                    unreadable = True
                    continue
                written = write_bytes(pid)
                if written is not None:
                    io_now[pid] = written
                    if pid in job["io"]:
                        io_growth += max(0, written - job["io"][pid])
                paths = self.open_files(pid)
                if paths is None:
                    unreadable = True
                    continue
                for path in paths:
                    sampled = self.sample_file(path, now)
                    if sampled:
                        files.append((path, sampled[0]))
                        growth += sampled[1]
            job["rate"] = ema(job["rate"], growth / dt) if dt else job["rate"]
            job["io_rate"] = ema(job["io_rate"], io_growth / dt) if dt else job["io_rate"]
            job["io"] = io_now
            rate = job["rate"] or 0
            io_rate = job["io_rate"] or 0
            rx_rate = self.rx_rate or 0
            partial_growing = any(i["partial"] and now - i["grew"] < 2.5 and (i["rate"] or 0) >= MIN_PARTIAL_RATE
                                  for _p, i in files)
            networked = rx_rate >= 0.3 * rate

            state = None
            approx = False
            if alpm:
                state, approx, rate = "downloading", True, rx_rate
            elif rate >= MIN_RATE or partial_growing:
                if networked or partial_growing:
                    state = "downloading"
                elif now - job["downloaded"] < WRITE_AFTER_DOWNLOAD:
                    state = "writing"
            elif unreadable and rx_rate >= 256 * 1024 and kind in ("pacman", "aur", "flatpak"):
                state, approx, rate = "downloading", True, rx_rate
            elif io_rate >= MIN_IO_RATE and rx_rate >= MIN_IO_RX:
                state, approx, rate = "downloading", True, rx_rate

            if state:
                job["grew"] = now
                job["state"] = state
                job["approx"] = approx
                job["shown_rate"] = rate
                if state == "downloading":
                    job["downloaded"] = now
            elif now - job["grew"] > LINGER:
                self.finish(root, job)
                continue
            if not job.get("state"):
                continue

            if job["visible_since"] is None:
                job["visible_since"] = now
            growing = sorted(((p, i) for p, i in files if now - i["grew"] < LINGER),
                             key=lambda item: item[1]["rate"] or 0, reverse=True)
            total = None
            if growing:
                path, info = growing[0]
                job["detail"] = clip(strip_partial(os.path.basename(path)))
                job["done"] = sum(i["size"] for _p, i in growing)
                if len(growing) == 1:
                    total = aria2_total(path)
            # Keep the last file seen: some tools open it only in bursts.
            detail, done = job["detail"], job["done"]
            out.append({
                "id": f"pid:{root}",
                "kind": kind,
                "title": job["title"],
                "detail": detail,
                "state": job["state"],
                "done": done,
                "total": total,
                "rate": round(job.get("shown_rate") or 0),
                "approx": job["approx"],
                "files": len(growing),
            })

        for root in list(self.jobs):
            if root not in groups:
                self.finish(root, self.jobs[root])

        if steam_running:
            out.extend(self.steam.collect(now))

        # forget files nobody holds any more
        self.files = {p: i for p, i in self.files.items() if now - i["seen"] < 10}

        finished, self.finished = self.finished, []
        out.sort(key=lambda j: j["id"])
        return {
            "v": PROTOCOL_VERSION,
            "rx": round(self.rx_rate or 0),
            "jobs": out[:MAX_JOBS],
            "finished": finished[:MAX_JOBS],
        }

    def finish(self, root, job):
        self.jobs.pop(root, None)
        since = job.get("visible_since")
        if since is not None and job.get("downloaded") and job["downloaded"] - since >= 3:
            self.finished.append(job["title"])

    def title_for(self, root, kind, table):
        if kind == "umu":
            game = read_environ_value(root, "GAMEID") or ""
            game = game[4:] if game.startswith("umu-") else game
            if game and not game.isdigit() and game != "default":
                name = KNOWN_GAMEIDS.get(game.lower(), f"{game[:1].upper()}{game[1:]}")
                return clip(f"{name} (Proton)")
        if kind == "other":
            return clip(table[root][0])
        return KIND_TITLES.get(kind, kind)


# ---------------------------------------------------------------- demo


def demo_snapshot(now):
    phase = now % 40
    jobs = [{
        "id": "pid:1", "kind": "umu", "title": "Battle.net (Proton)",
        "detail": "GE-Proton11-12-x86_64.tar.gz", "state": "downloading",
        "done": int(phase * 2.1e6), "total": None, "rate": 2100000, "approx": False, "files": 1,
    }, {
        "id": "steam:570", "kind": "steam", "title": "Steam", "detail": "Dota 2",
        "state": "downloading", "done": int(9e9 + phase * 2.5e7), "total": int(4.1e10),
        "rate": 25000000, "approx": False, "files": 0,
    }]
    if phase > 20:
        jobs.append({"id": "pid:2", "kind": "pacman", "title": "pacman", "detail": "",
                     "state": "downloading", "done": None, "total": None,
                     "rate": 6400000, "approx": True, "files": 0})
    return {"v": PROTOCOL_VERSION, "rx": 33500000, "jobs": jobs, "finished": []}


# ---------------------------------------------------------------- main


class NotifyLock:
    """Only one helper announces finished downloads.

    The shell runs one widget per monitor, so several helpers may be alive at
    once; the one holding this lock owns the "finished" events.
    """

    def __init__(self):
        runtime = os.environ.get("XDG_RUNTIME_DIR") or f"/tmp/downloads-monitor-{os.getuid()}"
        self.path = os.path.join(runtime, "omarchy-downloads-monitor.lock")
        self.fh = None

    def held(self):
        if self.fh is not None:
            return True
        try:
            fh = open(self.path, "a")
        except OSError:
            return False
        try:
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            fh.close()
            return False
        self.fh = fh
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--watch", default="", help="extra process names, comma separated")
    parser.add_argument("--once", action="store_true", help="print two samples and exit")
    parser.add_argument("--demo", action="store_true", help="emit fake downloads for UI work")
    args = parser.parse_args()
    interval = min(10.0, max(0.5, args.interval))

    monitor = Monitor(args.watch.split(","))
    lock = NotifyLock()
    parent = os.getppid()
    last = None
    samples = 0
    while True:
        now = time.monotonic()
        snap = demo_snapshot(now) if args.demo else monitor.tick(now)
        if snap["finished"] and not lock.held():
            snap["finished"] = []
        line = json.dumps(snap, ensure_ascii=False, separators=(",", ":"))
        samples += 1
        if args.once and samples < 2:
            time.sleep(interval)
            continue
        if line != last or snap["jobs"]:
            try:
                sys.stdout.write(line + "\n")
                sys.stdout.flush()
            except BrokenPipeError:
                return 0
            last = line
        if args.once:
            return 0
        if os.getppid() != parent:
            return 0  # the shell went away
        time.sleep(interval)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(0)
