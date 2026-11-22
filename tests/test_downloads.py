"""Tests for the helper: python3 -m unittest discover -s tests"""

import importlib.util
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HELPER = os.path.join(HERE, "..", "downloads.py")

spec = importlib.util.spec_from_file_location("downloads", HELPER)
downloads = importlib.util.module_from_spec(spec)
spec.loader.exec_module(downloads)


class HelpersTest(unittest.TestCase):
    def test_clip_drops_control_chars_and_bounds_length(self):
        self.assertEqual(downloads.clip("a\x1b[31mb"), "a[31mb")
        self.assertEqual(len(downloads.clip("x" * 500)), downloads.MAX_TEXT)

    def test_partial_detection(self):
        self.assertTrue(downloads.is_partial("/home/u/.cache/x.tar.gz.part"))
        self.assertTrue(downloads.is_partial("/tmp/tmpab/GE-Proton.tar.gz.parts"))
        self.assertTrue(downloads.is_partial("/r/.git/objects/pack/tmp_pack_AbC"))
        self.assertFalse(downloads.is_partial("/home/u/game/data.bin"))

    def test_interesting_path(self):
        self.assertFalse(downloads.interesting_path("/dev/null"))
        self.assertFalse(downloads.interesting_path("socket:[123]"))
        self.assertFalse(downloads.interesting_path("/home/u/app.log"))
        self.assertFalse(downloads.interesting_path("/home/u/x (deleted)"))
        self.assertTrue(downloads.interesting_path("/home/u/x.part"))

    def test_strip_partial(self):
        self.assertEqual(downloads.strip_partial("a.tar.gz.part"), "a.tar.gz")
        self.assertEqual(downloads.strip_partial("a.tar.gz"), "a.tar.gz")

    def test_aria2_total(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, "file.iso")
            body = struct.pack(">H", 1) + b"\0\0\0\0" + struct.pack(">I", 0)
            body += struct.pack(">I", 1 << 20) + struct.pack(">Q", 123456789)
            with open(target + ".aria2", "wb") as fh:
                fh.write(body + b"\0" * 32)
            self.assertEqual(downloads.aria2_total(target), 123456789)
            self.assertIsNone(downloads.aria2_total(os.path.join(tmp, "missing")))

    def test_vdf_pairs(self):
        text = '"AppState"\n{\n\t"appid"\t\t"570"\n\t"name"\t\t"Dota 2"\n}'
        self.assertEqual(dict(downloads.parse_vdf_pairs(text)), {"appid": "570", "name": "Dota 2"})


class SteamTest(unittest.TestCase):
    def test_reports_progress_from_app_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            apps = os.path.join(tmp, "steamapps")
            os.makedirs(apps)
            manifest = os.path.join(apps, "appmanifest_570.acf")

            def write(done):
                with open(manifest, "w") as fh:
                    fh.write('"AppState"\n{\n"appid" "570"\n"name" "Dota 2"\n'
                             f'"BytesToDownload" "1000"\n"BytesDownloaded" "{done}"\n}}')

            steam = downloads.SteamCollector()
            steam.libraries, steam.libraries_at = [apps], 1e12
            write(100)
            steam.collect(100.0)
            write(300)
            jobs = steam.collect(102.0)
            self.assertEqual(len(jobs), 1)
            self.assertEqual(jobs[0]["detail"], "Dota 2")
            self.assertEqual(jobs[0]["done"], 300)
            self.assertEqual(jobs[0]["total"], 1000)
            self.assertGreater(jobs[0]["rate"], 0)
            write(1000)
            self.assertEqual(steam.collect(104.0), [])


class LiveTest(unittest.TestCase):
    """Runs a real watched process writing a growing .part file."""

    def test_detects_growing_partial_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = os.path.join(tmp, "big.bin.part")
            # A process named "curl" (as seen in /proc/<pid>/comm) that writes slowly.
            writer = subprocess.Popen([sys.executable, "-c",
                "import ctypes,sys,time\n"
                "ctypes.CDLL(None).prctl(15, b'curl', 0, 0, 0)\n"
                "f=open(sys.argv[1],'wb')\n"
                "for _ in range(40):\n f.write(b'x'*65536); f.flush(); time.sleep(0.1)",
                target])
            try:
                time.sleep(0.2)
                monitor = downloads.Monitor()
                snap = None
                for i in range(4):
                    snap = monitor.tick(time.monotonic())
                    time.sleep(0.5)
                jobs = [j for j in snap["jobs"] if j["id"] == f"pid:{writer.pid}"]
                self.assertEqual(len(jobs), 1, snap)
                self.assertEqual(jobs[0]["detail"], "big.bin")
                self.assertGreater(jobs[0]["rate"], 0)
            finally:
                writer.kill()
                writer.wait()


class CliTest(unittest.TestCase):
    def test_once_prints_valid_snapshot(self):
        import json
        out = subprocess.run([sys.executable, "-I", HELPER, "--once", "--interval", "0.5"],
                             capture_output=True, text=True, timeout=20, check=True)
        snap = json.loads(out.stdout.strip().splitlines()[-1])
        self.assertEqual(snap["v"], 1)
        self.assertIsInstance(snap["jobs"], list)


if __name__ == "__main__":
    unittest.main()
