"""Tests for tools/install-ghidra-cell.sh against fake trees in temporary directories.

Every run passes --ghidra-dir and/or --settings-dir and replaces every environment variable the
script could use to find a real directory, so a default-path bug cannot reach the real Ghidra
install or the real user settings.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
SCRIPT = TOOLS_DIR / "install-ghidra-cell.sh"
SH = shutil.which("sh")

VERSION = "12.1.4"
RELEASE = "PUBLIC"
SETTINGS_LEAF = f"ghidra_{VERSION}_{RELEASE}"
# The script reads the user name from the environment, so the tests can pin it.
USER_NAME = "testuser"
SOURCE_FILES = {
    "Module.manifest": b"",
    "extension.properties": f"name=CellPPU\ndescription=test\nversion={VERSION}\n".encode(),
    "LICENSE-Ghidra.txt": b"licence text\n",
    "data/sleighArgs.txt": b"",
    "data/languages/cell.ldefs": b"<language_definitions/>\n",
    "data/languages/cell_vmx.sinc": b"# constructors\n",
    "data/languages/ppc_cell.slaspec": b"@include \"x.sinc\"\n",
    "data/languages/ppc_isa.sinc": b"# stock file\n",
    "data/languages/ppc_cell.sla": b"compiled\n",
}


def snapshot(root: Path) -> dict[str, tuple]:
    """Relative path -> (kind, sha256, mtime_ns) for every entry below root."""
    result = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        stat = path.lstat()
        if path.is_symlink():
            result[rel] = ("link", os.readlink(path), stat.st_mtime_ns)
        elif path.is_dir():
            result[rel] = ("dir", "", stat.st_mtime_ns)
        else:
            result[rel] = ("file", hashlib.sha256(path.read_bytes()).hexdigest(), stat.st_mtime_ns)
    return result


def write_tree(root: Path, files: dict[str, bytes]) -> None:
    for rel, data in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


@unittest.skipUnless(os.name == "posix" and SH, "needs a POSIX sh")
class InstallScriptTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        # The whole fake world; the script may only change the destination inside it.
        self.world = Path(self._tmp.name).resolve()
        self.ghidra = self.world / "ghidra_install"
        (self.ghidra / "Ghidra" / "Processors" / "PowerPC").mkdir(parents=True)
        (self.ghidra / "Ghidra" / "Extensions").mkdir()
        (self.ghidra / "Ghidra" / "application.properties").write_text(
            f"application.name=Ghidra\napplication.version={VERSION}\n"
            f"application.release.name={RELEASE}\n",
            encoding="utf-8",
        )
        self.settings = self.world / "settings" / SETTINGS_LEAF
        self.settings.mkdir(parents=True)
        self.source = self.world / "staged" / "CellPPU"
        write_tree(self.source, SOURCE_FILES)
        self.home = self.world / "home"
        self.home.mkdir()
        self.cwd = self.world / "cwd"
        self.cwd.mkdir()

    # Environment and invocation

    def child_env(self, **overrides: str | None) -> dict[str, str]:
        env = dict(os.environ)
        env.update(
            HOME=str(self.home),
            USER=USER_NAME,
        )
        env.pop("GHIDRA_INSTALL_DIR", None)
        env.pop("XDG_CONFIG_HOME", None)
        for key, value in overrides.items():
            if value is None:
                env.pop(key, None)
            else:
                env[key] = value
        return env

    def run_script(self, *args: str, env: dict[str, str] | None = None, stdin: str | None = None):
        # Without --yes the confirmation prompt reads its answer from stdin; no input means end of
        # input.
        proc = subprocess.run(
            [SH, str(SCRIPT), *args],
            env=env if env is not None else self.child_env(),
            cwd=self.cwd,
            input=stdin if stdin is not None else "",
            capture_output=True,
            text=True,
            timeout=120,
        )
        return proc

    def install(self, *extra: str, target: str = "user", env: dict[str, str] | None = None):
        args = ["--ghidra-dir", str(self.ghidra), "--target", target, "--source", str(self.source),
                "--settings-dir", str(self.settings)]
        return self.run_script(*args, *extra, "--yes", env=env)

    def dest_for(self, target: str) -> Path:
        if target == "user":
            return self.settings / "Extensions" / "CellPPU"
        return self.ghidra / "Ghidra" / "Processors" / "CellPPU"

    def assertOk(self, proc):
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def assertFails(self, proc, text: str):
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn(text, proc.stderr)
        self.assertNotIn("installed:", proc.stdout)

    def installed_files(self, dest: Path) -> dict[str, bytes]:
        return {
            p.relative_to(dest).as_posix(): p.read_bytes() for p in dest.rglob("*") if p.is_file()
        }

    # Install

    def test_fresh_install_to_each_target(self):
        for target in ("user", "processors"):
            with self.subTest(target=target):
                self.setUp()
                before = snapshot(self.world)
                proc = self.install(target=target)
                self.assertOk(proc)
                dest = self.dest_for(target)
                self.assertEqual(self.installed_files(dest), SOURCE_FILES)
                self.assertIn(f"ghidra: {VERSION} {RELEASE} ({self.ghidra})", proc.stdout)
                self.assertIn(f"target: {dest}", proc.stdout)
                self.assertIn(
                    f"plan: added {len(SOURCE_FILES)}, updated 0, removed 0, unchanged 0", proc.stdout
                )
                self.assertIn("  added data/languages/ppc_cell.sla", proc.stdout)
                self.assertIn(
                    "installed: restart Ghidra to load PowerPC:BE:64:CellPPU-32addr", proc.stdout
                )
                self.assertOutsideUntouched(before, dest, allow_new={self.settings / "Extensions"})

    def test_sla_is_newest_installed_file(self):
        # Give the sources a later modification time than "now" to prove the script outdoes them.
        future = self.source.joinpath("data/languages/cell_vmx.sinc")
        stamp = future.stat().st_mtime + 3600
        os.utime(future, (stamp, stamp))
        self.assertOk(self.install())
        dest = self.dest_for("user")
        sla = (dest / "data/languages/ppc_cell.sla").stat().st_mtime_ns
        for path in dest.rglob("*"):
            if path.is_file() and path.suffix != ".sla":
                self.assertGreater(sla, path.stat().st_mtime_ns, path.name)

    def test_second_run_is_up_to_date_and_changes_nothing(self):
        for target in ("user", "processors"):
            with self.subTest(target=target):
                self.setUp()
                self.assertOk(self.install(target=target))
                before = snapshot(self.world)
                proc = self.install(target=target)
                self.assertOk(proc)
                self.assertIn(f"up to date: {self.dest_for(target)}", proc.stdout)
                self.assertNotIn("plan:", proc.stdout)
                self.assertEqual(snapshot(self.world), before)

    def test_changed_and_extra_destination_files(self):
        self.assertOk(self.install())
        dest = self.dest_for("user")
        (dest / "data/languages/cell_vmx.sinc").write_bytes(b"# edited\n")
        extra = dest / "data" / "extra" / "left.txt"
        extra.parent.mkdir()
        extra.write_bytes(b"left over\n")
        before = snapshot(self.world)
        proc = self.install()
        self.assertOk(proc)
        self.assertIn(
            f"plan: added 0, updated 1, removed 1, unchanged {len(SOURCE_FILES) - 1}", proc.stdout
        )
        self.assertIn("  updated data/languages/cell_vmx.sinc", proc.stdout)
        self.assertIn("  removed data/extra/left.txt", proc.stdout)
        self.assertEqual(self.installed_files(dest), SOURCE_FILES)
        self.assertFalse((dest / "data" / "extra").exists(), "empty directory is left behind")
        self.assertOutsideUntouched(before, dest)
        sla = (dest / "data/languages/ppc_cell.sla").stat().st_mtime_ns
        self.assertGreater(sla, (dest / "data/languages/cell_vmx.sinc").stat().st_mtime_ns)

    def test_missing_file_is_added_again(self):
        self.assertOk(self.install())
        dest = self.dest_for("user")
        (dest / "LICENSE-Ghidra.txt").unlink()
        proc = self.install()
        self.assertOk(proc)
        self.assertIn("plan: added 1, updated 0, removed 0", proc.stdout)
        self.assertEqual(self.installed_files(dest), SOURCE_FILES)

    def test_dry_run_writes_nothing(self):
        before = snapshot(self.world)
        proc = self.run_script(
            "--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
            "--source", str(self.source), "--dry-run",
        )
        self.assertOk(proc)
        self.assertIn(f"plan: added {len(SOURCE_FILES)}", proc.stdout)
        self.assertIn("cancelled: nothing changed", proc.stdout)
        self.assertNotIn("installed:", proc.stdout)
        self.assertEqual(snapshot(self.world), before)

    def test_dry_run_with_existing_destination_writes_nothing(self):
        self.assertOk(self.install())
        dest = self.dest_for("user")
        (dest / "data/languages/cell_vmx.sinc").write_bytes(b"# edited\n")
        before = snapshot(self.world)
        proc = self.install("--dry-run")
        self.assertOk(proc)
        self.assertIn("updated 1", proc.stdout)
        self.assertEqual(snapshot(self.world), before)

    def test_declined_confirmation_writes_nothing(self):
        before = snapshot(self.world)
        proc = self.run_script(
            "--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
            "--source", str(self.source),
            stdin="n\n",
        )
        self.assertOk(proc)
        self.assertIn("cancelled: nothing changed", proc.stdout)
        self.assertEqual(snapshot(self.world), before)

    def test_accepted_confirmation_installs(self):
        for answer in ("y\n", "yes\n", "Y\n", "YES\n"):
            with self.subTest(answer=answer):
                self.setUp()
                proc = self.run_script(
                    "--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
                    "--source", str(self.source),
                    stdin=answer,
                )
                self.assertOk(proc)
                self.assertIn("proceed? [y/N]", proc.stdout)
                self.assertIn("installed:", proc.stdout)
                self.assertEqual(self.installed_files(self.dest_for("user")), SOURCE_FILES)

    def test_other_answers_and_end_of_input_cancel(self):
        for answer in ("", "\n", "n\n", "no\n", "maybe\n", "yess\n"):
            with self.subTest(answer=answer):
                before = snapshot(self.world)
                proc = self.run_script(
                    "--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
                    "--source", str(self.source),
                    stdin=answer,
                )
                self.assertOk(proc)
                self.assertIn("cancelled: nothing changed", proc.stdout)
                self.assertNotIn("installed:", proc.stdout)
                self.assertEqual(snapshot(self.world), before)

    def test_dry_run_wins_over_yes_and_does_not_ask(self):
        before = snapshot(self.world)
        proc = self.install("--dry-run")
        self.assertOk(proc)
        self.assertIn("cancelled: nothing changed", proc.stdout)
        self.assertNotIn("proceed?", proc.stdout)
        self.assertEqual(snapshot(self.world), before)

    def test_uninstall_asks_first(self):
        self.assertOk(self.install())
        dest = self.dest_for("user")
        args = ["--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings), "--uninstall"]
        declined = self.run_script(*args, stdin="n\n")
        self.assertOk(declined)
        self.assertIn(f"plan: remove {len(SOURCE_FILES)} files", declined.stdout)
        self.assertIn("cancelled: nothing changed", declined.stdout)
        self.assertTrue(dest.exists())
        accepted = self.run_script(*args, stdin="y\n")
        self.assertOk(accepted)
        self.assertIn(f"uninstalled: {dest}", accepted.stdout)
        self.assertFalse(dest.exists())

    # Refusals

    def test_foreign_destination_is_refused(self):
        for label, files in (
            ("other extension", {"extension.properties": b"name=Other\nversion=1\n"}),
            ("no properties", {"readme.txt": b"mine\n"}),
        ):
            with self.subTest(label):
                self.setUp()
                dest = self.dest_for("user")
                write_tree(dest, files)
                before = snapshot(self.world)
                self.assertFails(self.install(), "extension.properties does not name CellPPU")
                self.assertEqual(snapshot(self.world), before)
                self.assertFails(self.run_script(
                    "--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
                    "--uninstall", "--yes",
                ), "refusing to touch")
                self.assertEqual(snapshot(self.world), before)

    def test_source_for_another_ghidra_version_is_refused(self):
        props = self.source / "extension.properties"
        props.write_text("name=CellPPU\nversion=11.0.0\n", encoding="utf-8")
        before = snapshot(self.world)
        proc = self.install()
        self.assertFails(proc, "11.0.0")
        self.assertIn("ghidra-cell-lang build", proc.stderr)
        self.assertEqual(snapshot(self.world), before)

    def test_unbuilt_source_is_refused(self):
        (self.source / "data/languages/ppc_cell.sla").unlink()
        before = snapshot(self.world)
        proc = self.install()
        self.assertFails(proc, "ppc_cell.sla")
        self.assertIn("ghidra-cell-lang build", proc.stderr)
        self.assertEqual(snapshot(self.world), before)
        shutil.rmtree(self.source)
        self.assertFails(self.install(), "ghidra-cell-lang build")

    def test_source_named_for_another_module_is_refused(self):
        (self.source / "extension.properties").write_text(
            f"name=Other\nversion={VERSION}\n", encoding="utf-8"
        )
        self.assertFails(self.install(), "not a CellPPU module")

    def test_missing_ghidra_dir_is_an_error(self):
        before = snapshot(self.world)
        proc = self.run_script("--settings-dir", str(self.settings), "--source", str(self.source),
                               "--yes")
        self.assertFails(proc, "pass --ghidra-dir or set GHIDRA_INSTALL_DIR")
        self.assertEqual(snapshot(self.world), before)

    def test_ghidra_dir_from_environment(self):
        env = self.child_env(GHIDRA_INSTALL_DIR=str(self.ghidra))
        proc = self.run_script("--settings-dir", str(self.settings), "--source", str(self.source),
                               "--yes", env=env)
        self.assertOk(proc)
        self.assertEqual(self.installed_files(self.dest_for("user")), SOURCE_FILES)

    def test_not_a_ghidra_install_is_an_error(self):
        empty = self.world / "empty"
        empty.mkdir()
        proc = self.run_script("--ghidra-dir", str(empty), "--settings-dir", str(self.settings),
                               "--source", str(self.source), "--yes")
        self.assertFails(proc, "not a Ghidra install")

    def test_user_target_needs_an_existing_settings_directory(self):
        shutil.rmtree(self.settings)
        before = snapshot(self.world)
        self.assertFails(self.install(), "settings directory not found")
        self.assertEqual(snapshot(self.world), before)

    def test_unknown_target_is_an_error(self):
        proc = self.run_script("--ghidra-dir", str(self.ghidra), "--target", "Elsewhere")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("usage:", proc.stderr)

    def test_target_is_case_insensitive(self):
        self.assertOk(self.install(target="Processors"))
        self.assertEqual(self.installed_files(self.dest_for("processors")), SOURCE_FILES)

    def test_unknown_option_is_a_usage_error(self):
        proc = self.run_script("--ghidra-dir", str(self.ghidra), "--nope")
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("unexpected argument: --nope", proc.stderr)

    def test_paths_with_spaces(self):
        # Every path below holds a space: quoting bugs, including in the duplicate check, show here.
        spaced = self.world / "with space"
        ghidra = spaced / "Ghidra 12.1"
        (ghidra / "Ghidra" / "Processors" / "PowerPC").mkdir(parents=True)
        (ghidra / "Ghidra" / "application.properties").write_text(
            f"application.version={VERSION}\napplication.release.name={RELEASE}\n", encoding="utf-8"
        )
        settings = spaced / "my settings" / SETTINGS_LEAF
        settings.mkdir(parents=True)
        source = spaced / "staged files" / "CellPPU"
        write_tree(source, SOURCE_FILES)
        write_tree(source, {"data/with space.txt": b"spaced\n"})
        args = ["--ghidra-dir", str(ghidra), "--settings-dir", str(settings), "--source", str(source)]
        proc = self.run_script(*args, "--yes")
        self.assertOk(proc)
        dest = settings / "Extensions" / "CellPPU"
        self.assertEqual(
            self.installed_files(dest), {**SOURCE_FILES, "data/with space.txt": b"spaced\n"}
        )
        again = self.run_script(*args, "--yes")
        self.assertOk(again)
        self.assertIn(f"up to date: {dest}", again.stdout)
        stray = ghidra / "Ghidra" / "Processors" / "CellPPU"
        write_tree(stray, {"extension.properties": b"name=CellPPU\n"})
        before = snapshot(self.world)
        refused = self.run_script(*args, "--yes")
        self.assertFails(refused, f"CellPPU already exists at {stray}")
        self.assertEqual(snapshot(self.world), before)
        removed = self.run_script(
            "--ghidra-dir", str(ghidra), "--settings-dir", str(settings), "--uninstall", "--yes"
        )
        self.assertOk(removed)
        self.assertFalse(dest.exists())

    def test_duplicate_module_is_refused(self):
        cases = (
            ("user", "Ghidra/Processors/CellPPU", "ghidra"),
            ("user", "Ghidra/Extensions/CellPPU", "ghidra"),
            ("processors", "Extensions/CellPPU", "settings"),
            ("processors", "Ghidra/Extensions/CellPPU", "ghidra"),
        )
        for target, rel, base in cases:
            with self.subTest(target=target, existing=rel):
                self.setUp()
                existing = getattr(self, base) / rel
                write_tree(
                    existing, {"extension.properties": b"name=CellPPU\n", "keep.txt": b"x\n"}
                )
                before = snapshot(self.world)
                proc = self.install(target=target)
                self.assertFails(proc, f"CellPPU already exists at {existing}")
                self.assertIn("uninstall it first", proc.stderr)
                self.assertEqual(snapshot(self.world), before)

    # Uninstall

    def test_uninstall_removes_only_the_destination(self):
        for target in ("user", "processors"):
            with self.subTest(target=target):
                self.setUp()
                self.assertOk(self.install(target=target))
                dest = self.dest_for(target)
                neighbour = dest.parent / "Other" / "keep.txt"
                write_tree(dest.parent, {"Other/keep.txt": b"keep\n"})
                before = snapshot(self.world)
                args = ["--ghidra-dir", str(self.ghidra), "--target", target]
                if target == "user":
                    args += ["--settings-dir", str(self.settings)]
                proc = self.run_script(*args, "--uninstall", "--yes")
                self.assertOk(proc)
                self.assertIn(f"uninstalled: {dest}", proc.stdout)
                self.assertFalse(dest.exists())
                self.assertTrue(neighbour.exists())
                after = snapshot(self.world)
                prefix = dest.relative_to(self.world).as_posix()
                parent = dest.parent.relative_to(self.world).as_posix()
                self.assertEqual(
                    self.without_destination(after, prefix, parent),
                    self.without_destination(before, prefix, parent),
                )
                again = self.run_script(*args, "--uninstall", "--yes")
                self.assertOk(again)
                self.assertIn(f"not installed: {dest}", again.stdout)

    def test_uninstall_whatif_removes_nothing(self):
        self.assertOk(self.install())
        before = snapshot(self.world)
        proc = self.run_script("--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
                               "--uninstall", "--dry-run")
        self.assertOk(proc)
        self.assertNotIn("uninstalled:", proc.stdout)
        self.assertEqual(snapshot(self.world), before)

    def test_uninstall_ignores_other_copies_and_missing_source(self):
        # The duplicate guard applies to installs only, so a stray copy can still be removed.
        self.assertOk(self.install())
        stray = self.ghidra / "Ghidra" / "Processors" / "CellPPU"
        write_tree(stray, {"extension.properties": b"name=CellPPU\n"})
        shutil.rmtree(self.source)
        proc = self.run_script("--ghidra-dir", str(self.ghidra), "--settings-dir", str(self.settings),
                               "--uninstall", "--yes")
        self.assertOk(proc)
        self.assertFalse(self.dest_for("user").exists())

    # Links

    def test_destination_path_through_a_link_is_refused(self):
        elsewhere = self.world / "elsewhere"
        elsewhere.mkdir()
        link = self.settings / "Extensions"
        try:
            link.symlink_to(elsewhere, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"cannot create symbolic links here: {exc}")
        before = snapshot(self.world)
        self.assertFails(self.install(), "symbolic link or junction")
        self.assertEqual(snapshot(self.world), before)
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_link_inside_the_destination_is_refused(self):
        self.assertOk(self.install())
        dest = self.dest_for("user")
        outside = self.world / "outside"
        write_tree(outside, {"file.txt": b"precious\n"})
        try:
            (dest / "data" / "linked").symlink_to(outside, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"cannot create symbolic links here: {exc}")
        before = snapshot(self.world)
        self.assertFails(self.install(), "symbolic link or junction")
        self.assertEqual(snapshot(self.world), before)

    def test_link_inside_the_source_is_refused(self):
        outside = self.world / "outside"
        write_tree(outside, {"file.txt": b"not part of the module\n"})
        try:
            (self.source / "data" / "linked").symlink_to(outside, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"cannot create symbolic links here: {exc}")
        before = snapshot(self.world)
        self.assertFails(self.install(), "symbolic link or junction")
        self.assertEqual(snapshot(self.world), before)

    # Default settings directory

    @staticmethod
    def without_destination(snap: dict, dest_rel: str, parent_rel: str) -> dict:
        # The parent directory's own timestamp changes when the destination is removed.
        return {
            k: v
            for k, v in snap.items()
            if k != dest_rel and not k.startswith(dest_rel + "/") and k != parent_rel
        }

    def default_install(self, env: dict[str, str]):
        return self.run_script(
            "--ghidra-dir", str(self.ghidra), "--source", str(self.source), "--yes", env=env
        )

    def platform_settings_base(self) -> Path:
        if os.uname().sysname == "Darwin":
            return self.home / "Library" / "ghidra" / SETTINGS_LEAF
        return self.home / ".config" / "ghidra" / SETTINGS_LEAF

    def assertInstalledUnder(self, base: Path, env: dict[str, str]):
        before = snapshot(self.world)
        proc = self.default_install(env)
        self.assertOk(proc)
        dest = base / "Extensions" / "CellPPU"
        self.assertEqual(self.installed_files(dest), SOURCE_FILES)
        self.assertOutsideUntouched(before, dest, allow_new={base / "Extensions"})

    def test_default_settings_with_xdg_inside_the_home(self):
        xdg = self.home / "cfg"
        base = xdg / "ghidra" / SETTINGS_LEAF
        base.mkdir(parents=True)
        self.assertInstalledUnder(base, self.child_env(XDG_CONFIG_HOME=str(xdg)))

    def test_default_settings_with_xdg_outside_the_home(self):
        xdg = self.world / "xdg"
        base = xdg / f"{USER_NAME}-ghidra" / SETTINGS_LEAF
        base.mkdir(parents=True)
        (xdg / "ghidra" / SETTINGS_LEAF).mkdir(parents=True)
        self.assertInstalledUnder(base, self.child_env(XDG_CONFIG_HOME=str(xdg)))
        self.assertFalse((xdg / "ghidra" / SETTINGS_LEAF / "Extensions").exists())

    def test_default_settings_user_name_is_cleaned_like_ghidra(self):
        names = (
            ("Alice Smith", "AliceSmith"),
            ("CORP\\Alice Smith", "AliceSmith"),
            ("/home/alice", "alice"),
        )
        for raw, clean in names:
            with self.subTest(raw=raw):
                self.setUp()
                xdg = self.world / "xdg"
                base = xdg / f"{clean}-ghidra" / SETTINGS_LEAF
                base.mkdir(parents=True)
                env = self.child_env(XDG_CONFIG_HOME=str(xdg), USERNAME=raw, USER=raw)
                self.assertInstalledUnder(base, env)

    def test_default_settings_without_xdg(self):
        base = self.platform_settings_base()
        base.mkdir(parents=True)
        self.assertInstalledUnder(base, self.child_env())

    def test_default_settings_blank_xdg_counts_as_unset(self):
        base = self.platform_settings_base()
        base.mkdir(parents=True)
        self.assertInstalledUnder(base, self.child_env(XDG_CONFIG_HOME="   "))

    def test_default_settings_xdg_is_trimmed(self):
        xdg = self.home / "cfg"
        base = xdg / "ghidra" / SETTINGS_LEAF
        base.mkdir(parents=True)
        self.assertInstalledUnder(base, self.child_env(XDG_CONFIG_HOME=f"  {xdg} "))

    def test_default_settings_relative_xdg_is_an_error(self):
        (self.cwd / "cfg" / "ghidra" / SETTINGS_LEAF).mkdir(parents=True)
        before = snapshot(self.world)
        proc = self.default_install(self.child_env(XDG_CONFIG_HOME="cfg"))
        self.assertFails(proc, "XDG_CONFIG_HOME must be an absolute path")
        self.assertEqual(snapshot(self.world), before)

    def test_default_settings_missing_directory_is_an_error(self):
        proc = self.default_install(self.child_env())
        self.assertFails(proc, "settings directory not found")
        self.assertIn(SETTINGS_LEAF, proc.stderr)

    # Containment

    def assertOutsideUntouched(self, before: dict, dest: Path, allow_new: set[Path] = frozenset()):
        """Nothing outside dest changed, and nothing new exists outside it except allow_new."""
        after = snapshot(self.world)
        dest_rel = dest.relative_to(self.world).as_posix()
        allowed = {p.relative_to(self.world).as_posix() for p in allow_new}
        for rel, entry in after.items():
            if rel == dest_rel or rel.startswith(dest_rel + "/"):
                continue
            if rel not in before:
                self.assertIn(rel, allowed, f"new path outside the destination: {rel}")
                continue
            if entry[0] == "dir":  # directory times change when entries are added below them
                continue
            self.assertEqual(entry, before[rel], f"changed outside the destination: {rel}")
        for rel in before:
            if not (rel == dest_rel or rel.startswith(dest_rel + "/")):
                self.assertIn(rel, after, f"removed outside the destination: {rel}")


if __name__ == "__main__":
    unittest.main()
