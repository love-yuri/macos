# SPDX-License-Identifier: GPL-2.0-only
"""No live launchd/USB/Keychain operations; root adds credential read tests."""
import importlib.util
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("autostart", HERE / "autostart.py")
autostart = importlib.util.module_from_spec(spec)
spec.loader.exec_module(autostart)


class AutostartTests(unittest.TestCase):
    def test_plist_is_unattended_and_handles_special_ssid(self):
        ssid = 'A&B <网络> "test"'
        data = plistlib.loads(plistlib.dumps(autostart.service_plist(ssid, "0x00b00000")))
        self.assertEqual(data["ProgramArguments"],
                         ["/bin/zsh", "-f", str(autostart.BASE / "daemon.zsh"), ssid, "0x00b00000"])
        self.assertTrue(data["KeepAlive"])
        self.assertTrue(data["RunAtLoad"])
        self.assertEqual(data["UserName"], "root")
        self.assertEqual(data["AssociatedBundleIdentifiers"], [autostart.APPLICATION_ID])
        self.assertNotIn("sudo", str(data))
        self.assertNotIn(str(HERE), str(data))

    def test_reject_writable_or_symlink_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            directory.chmod(0o777)
            with self.assertRaises(RuntimeError):
                autostart.check_directory(directory)
            link = directory / "link"
            link.symlink_to(directory, target_is_directory=True)
            with self.assertRaises(RuntimeError):
                autostart.check_directory(link)

    def test_install_reinstall_and_uninstall_without_live_system_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base, logs = root / "installed", root / "logs"
            plist = root / "service.plist"
            calls = []

            def fake_run(*args):
                args = [str(value) for value in args]
                calls.append(args)
                if len(args) > 1 and args[1] == "save-credential":
                    (base / "wifi.credential").write_bytes(b"test secret")
                    (base / "wifi.credential").chmod(0o600)

            original_fstat = os.fstat

            def fake_root_fstat(fd):
                info = original_fstat(fd)
                fields = list(info)
                fields[4] = 0
                return os.stat_result(fields)

            with patch.multiple(autostart, BASE=base, LOG_DIR=logs, LOG=logs / "service.log", PLIST=plist), \
                 patch.object(autostart, "check_directory"), \
                 patch.object(autostart, "run", side_effect=fake_run), \
                 patch.object(autostart, "loaded", return_value=False), \
                 patch.object(autostart.os, "fstat", side_effect=fake_root_fstat):
                autostart.install("test", "0x00b00000", False)
                installed = plistlib.loads(plist.read_bytes())
                self.assertEqual(installed["ProgramArguments"][2], str(base / "daemon.zsh"))
                self.assertEqual(stat.S_IMODE(base.stat().st_mode), 0o755)
                application = base / "AIC8800.app"
                self.assertEqual(stat.S_IMODE((application / "Contents/MacOS/aicusb").stat().st_mode), 0o755)
                identity = plistlib.loads((application / "Contents/Info.plist").read_bytes())
                self.assertEqual(identity["CFBundleIdentifier"], installed["AssociatedBundleIdentifiers"][0])
                self.assertEqual(identity["CFBundleExecutable"], "aicusb")
                self.assertTrue(identity["NSRemovableVolumesUsageDescription"])
                self.assertFalse((base / "aicusb").exists())
                self.assertEqual(stat.S_IMODE(plist.stat().st_mode), 0o644)
                self.assertEqual(len(list((base / "firmware").iterdir())), 5)
                self.assertNotIn(b"test secret", plist.read_bytes())
                self.assertTrue(any(call[:4] == ["/usr/bin/codesign", "--force", "--sign", "-"] for call in calls))
                calls.clear()
                autostart.install("test", "0x00b00000", False)
                self.assertFalse(any("save-credential" in call for call in calls))
                self.assertTrue(any("check-credential" in call for call in calls))
                autostart.uninstall()
                self.assertFalse(base.exists())
                self.assertFalse(plist.exists())
                self.assertTrue((logs / "service.log").exists())


class CredentialTests(unittest.TestCase):
    def check(self, path, ssid="test", ok=False):
        result = subprocess.run([str(HERE / "build/aicusb"), "check-credential", "--ssid", ssid,
                                 "--credential-file", str(path)], capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0 if ok else 1, result.stderr.decode())
        self.assertNotIn(b"secret123", result.stdout + result.stderr)

    def test_file_policy_and_unattended_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "credential"
            self.check(path)
            path.write_bytes(b"AICWIFI1\x04\x09testsecret123")
            path.chmod(0o644)
            self.check(path)
            path.chmod(0o600)
            self.check(path, ok=os.geteuid() == 0)
            link = path.with_name("symlink")
            link.symlink_to(path)
            self.check(link)
            hardlink = path.with_name("hardlink")
            os.link(path, hardlink)
            self.check(path)
            hardlink.unlink()
            self.check(path, ssid="other")
            if os.geteuid() == 0:
                # Exercise the decoder only with actual root-owned fixtures.
                for malformed in (b"", b"AICWIFI1\xff\xff", b"AICWIFI1\x04\x09testshort",
                                  b"AICWIFI1\x04\x09testsecret\x0023", b"AICWIFI1\x04\x07testshort12",
                                  b"AICWIFI1\x04\x09testsecret123extra", b"x" * 107):
                    path.write_bytes(malformed)
                    self.check(path)
                path.write_bytes(b"AICWIFI1\x04\x40test" + b"a" * 64)
                self.check(path, ok=True)
                path.write_bytes(b"AICWIFI1\x04\x40test" + b"z" * 64)
                self.check(path)


if __name__ == "__main__":
    unittest.main()
