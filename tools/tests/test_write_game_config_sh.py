"""Tests for tools/write-game-config.sh against fake directories in a temporary directory."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS_DIR / "write-game-config.sh"
SH = shutil.which("sh")


@unittest.skipUnless(os.name == "posix" and SH, "needs a POSIX sh")
class WriteGameConfigShTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.world = Path(self._tmp.name).resolve()
        self.bin = self.world / "bin"
        self.bin.mkdir()
        self.game = self.world / "game"
        (self.game / "PS3_GAME").mkdir(parents=True)
        (self.game / "PS3_GAME" / "PARAM.SFO").write_bytes(b"\0PSF")
        self.elf = self.world / "gen" / "EBOOT.elf"
        self.config = self.bin / "config.json"

    def run_script(self, *args: str, cwd: Path | None = None):
        return subprocess.run(
            [SH, str(SCRIPT), *args],
            cwd=cwd or self.world,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def write(self, *extra: str, game: Path | None = None):
        return self.run_script(
            "--bin-dir", str(self.bin), "--game-dir", str(game or self.game), *extra
        )

    def assertOk(self, proc):
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    # Writing

    def test_missing_param_sfo_writes_nothing(self):
        (self.game / "PS3_GAME" / "PARAM.SFO").unlink()
        proc = self.write()
        self.assertOk(proc)
        self.assertEqual(
            proc.stdout.strip(),
            f"config.json not written: {self.game} has no PS3_GAME/PARAM.SFO",
        )
        self.assertFalse(self.config.exists())

    def test_writes_game_dir(self):
        proc = self.write()
        self.assertOk(proc)
        self.assertEqual(proc.stdout.strip(), f"wrote {self.config}")
        raw = self.config.read_bytes()
        self.assertFalse(raw.startswith(b"\xef\xbb\xbf"), "byte-order mark")
        self.assertNotIn(b"\r", raw)
        self.assertTrue(raw.endswith(b"\n"))
        self.assertEqual(json.loads(raw), {"game_dir": str(self.game)})

    def test_writes_elf_after_game_dir(self):
        proc = self.write("--eboot-elf", str(self.elf))
        self.assertOk(proc)
        raw = self.config.read_bytes()
        self.assertEqual(json.loads(raw), {"game_dir": str(self.game), "elf": str(self.elf)})
        self.assertLess(raw.index(b'"game_dir"'), raw.index(b'"elf"'))

    def test_relative_paths_are_made_absolute(self):
        proc = self.run_script(
            "--bin-dir", "bin", "--game-dir", "game/", "--eboot-elf", "gen/EBOOT.elf"
        )
        self.assertOk(proc)
        self.assertEqual(
            json.loads(self.config.read_text(encoding="utf-8")),
            {"game_dir": str(self.game), "elf": str(self.elf)},
        )

    def test_path_with_spaces_and_quotes_survives(self):
        game = self.world / 'my "game" dir'
        (game / "PS3_GAME").mkdir(parents=True)
        (game / "PS3_GAME" / "PARAM.SFO").write_bytes(b"\0PSF")
        self.assertOk(self.write(game=game))
        self.assertEqual(json.loads(self.config.read_bytes()), {"game_dir": str(game)})

    def test_missing_bin_dir_is_an_error(self):
        proc = self.run_script("--bin-dir", str(self.world / "nope"), "--game-dir", str(self.game))
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertFalse((self.world / "nope").exists())

    # Existing config.json is never changed

    def assertUnchanged(self, proc, text: str, content: bytes):
        self.assertOk(proc)
        self.assertEqual(proc.stdout.strip(), text)
        self.assertEqual(self.config.read_bytes(), content)

    def test_existing_matching_config_is_left_alone_silently(self):
        content = json.dumps({"game_dir": str(self.game), "elf": "x"}).encode()
        self.config.write_bytes(content)
        proc = self.write("--eboot-elf", str(self.elf))
        self.assertUnchanged(proc, "", content)

    def test_existing_game_dir_matches_through_link_and_trailing_slash(self):
        link = self.world / "link"
        try:
            link.symlink_to(self.game, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"cannot create symbolic links here: {exc}")
        for spelling in (f"{link}", f"{self.game}/", f"{self.world}/game/../game"):
            with self.subTest(spelling=spelling):
                content = json.dumps({"game_dir": spelling}).encode()
                self.config.write_bytes(content)
                self.assertUnchanged(self.write(), "", content)

    def test_existing_config_for_another_game_dir(self):
        other = self.world / "other"
        other.mkdir()
        content = json.dumps({"game_dir": str(other)}).encode()
        self.config.write_bytes(content)
        proc = self.write()
        self.assertUnchanged(
            proc,
            f"config.json left unchanged: game_dir in {self.config} differs from {self.game}",
            content,
        )

    def test_existing_config_without_a_string_game_dir(self):
        for label, content in (
            ("missing", b"{}"),
            ("number", b'{"game_dir": 3}'),
            ("list", b"[1, 2]"),
        ):
            with self.subTest(label):
                self.config.write_bytes(content)
                proc = self.write()
                self.assertUnchanged(
                    proc,
                    f"config.json left unchanged: game_dir in {self.config} differs from {self.game}",
                    content,
                )

    def test_existing_config_without_elf(self):
        content = json.dumps({"game_dir": str(self.game)}).encode()
        self.config.write_bytes(content)
        proc = self.write("--eboot-elf", str(self.elf))
        self.assertUnchanged(
            proc,
            f"config.json left unchanged: {self.config} has no elf; "
            f'add "elf": {json.dumps(str(self.elf))}',
            content,
        )

    def test_existing_config_that_cannot_be_parsed(self):
        for content in (b"{not json", b"", b"\xff\xfe"):
            with self.subTest(content=content):
                self.config.write_bytes(content)
                proc = self.write()
                self.assertUnchanged(
                    proc,
                    f"config.json left unchanged: {self.config} cannot be parsed",
                    content,
                )

    # Usage

    def test_missing_options_are_usage_errors(self):
        for args in ((), ("--bin-dir", str(self.bin)), ("--game-dir", str(self.game))):
            with self.subTest(args=args):
                proc = self.run_script(*args)
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertIn("usage: tools/write-game-config.sh", proc.stderr)

    def test_unknown_option_is_a_usage_error(self):
        proc = self.write("--nope")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("unexpected argument: --nope", proc.stderr)
        self.assertFalse(self.config.exists())


if __name__ == "__main__":
    unittest.main()
