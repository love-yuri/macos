#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Install a root-owned, self-contained launchd Wi-Fi service."""
import argparse
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile

LABEL = "com.aic8800.wifi"
BASE = Path("/Library/Application Support/AIC8800")
PLIST = Path("/Library/LaunchDaemons") / (LABEL + ".plist")
LOG_DIR = Path("/Library/Logs/AIC8800")
LOG = LOG_DIR / "service.log"
SOURCE = Path(__file__).resolve().parent
APPLICATION_ID = "com.aic8800.aicusb"
FIRMWARE = (
    "fw_adid_8800d80_u02.bin",
    "fw_patch_8800d80_u02.bin",
    "fw_patch_8800d80_u02_ext0.bin",
    "fw_patch_table_8800d80_u02.bin",
    "fmacfw_8800d80_u02.bin",
)


def service_plist(ssid, location):
    return {
        "Label": LABEL,
        "ProgramArguments": ["/bin/zsh", "-f", str(BASE / "daemon.zsh"), ssid, location],
        "WorkingDirectory": str(BASE),
        "UserName": "root",
        "GroupName": "wheel",
        "AssociatedBundleIdentifiers": [APPLICATION_ID],
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 15,
        "ExitTimeOut": 20,
        "Umask": 0o077,
        "StandardOutPath": str(LOG),
        "StandardErrorPath": str(LOG),
    }


def application_plist():
    return {
        "CFBundleIdentifier": APPLICATION_ID,
        "CFBundleName": "AIC8800",
        "CFBundleDisplayName": "AIC8800",
        "CFBundleExecutable": "aicusb",
        "CFBundlePackageType": "APPL",
        "CFBundleVersion": "1",
        "CFBundleShortVersionString": "1.0",
        "LSUIElement": True,
        "NSRemovableVolumesUsageDescription": "切换 AIC8800 网卡的 USB 安装盘模式，以连接 Wi-Fi。",
    }


def check_directory(path):
    # Every ancestor of executable/credential storage must resist replacement
    # by an unprivileged user. Never follow a symlink supplied at the target.
    for parent in [path, *path.parents]:
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise RuntimeError("Unsafe system directory: " + str(parent))


def ensure_directory(path, mode=0o755):
    check_directory(path.parent)
    if not path.exists() and not path.is_symlink():
        path.mkdir(mode=mode)
    check_directory(path)
    path.chmod(mode)


def atomic_write(path, data, mode):
    check_directory(path.parent)
    fd, temporary = tempfile.mkstemp(prefix=".aic-install-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fchmod(stream.fileno(), mode)
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def loaded():
    return subprocess.run(
        ["/bin/launchctl", "print", "system/" + LABEL],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    ).returncode == 0


def stop():
    if loaded():
        run("/bin/launchctl", "bootout", "system/" + LABEL)


def install(ssid, location, replace_credential):
    # TCC needs a valid executable identity for removable-volume access.
    # A root daemon cannot inherit the installing Terminal's privacy grants.
    run("/usr/bin/codesign", "--verify", "--strict", SOURCE / "build/aicusb")
    application = BASE / "AIC8800.app"
    executable = application / "Contents/MacOS/aicusb"
    payload = [(SOURCE / "build/aicusb", executable, 0o755),
               (SOURCE / "daemon.zsh", BASE / "daemon.zsh", 0o644),
               (SOURCE / "prepare.zsh", BASE / "prepare.zsh", 0o644)]
    payload += [(SOURCE / "firmware" / name, BASE / "firmware" / name, 0o644)
                for name in FIRMWARE]
    # Read all inputs before changing the installed service.
    contents = [(destination, source.read_bytes(), mode) for source, destination, mode in payload]
    # Privacy settings can select the named application. Only filenames become
    # visible to ordinary users; the credential itself remains root-only.
    ensure_directory(BASE, 0o755)
    ensure_directory(application)
    ensure_directory(application / "Contents")
    ensure_directory(application / "Contents/MacOS")
    ensure_directory(BASE / "firmware")
    ensure_directory(LOG_DIR)
    check_directory(PLIST.parent)
    stop()
    for destination, data, mode in contents:
        atomic_write(destination, data, mode)
    atomic_write(application / "Contents/Info.plist", plistlib.dumps(application_plist()), 0o644)
    run("/usr/bin/codesign", "--force", "--sign", "-", application)
    run("/usr/bin/codesign", "--verify", "--strict", application)
    credential = BASE / "wifi.credential"
    if replace_credential or not credential.exists():
        print("配置开机联网凭据；首次可能需要授权读取本网络的钥匙串或输入 Wi-Fi 密码。", flush=True)
        run(executable, "save-credential", "--ssid", ssid, "--credential-file", credential)
    else:
        # Validate the existing credential without opening USB or prompting.
        run(executable, "check-credential", "--ssid", ssid, "--credential-file", credential)
    # Precreate the log without following a preexisting symlink.
    fd = os.open(LOG, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != 0 or info.st_nlink != 1:
            raise RuntimeError("Unsafe log file: " + str(LOG))
        os.fchmod(fd, 0o644)
    finally:
        os.close(fd)
    data = plistlib.dumps(service_plist(ssid, location))
    # Finish the foreground connection's normal cleanup before USB takeover.
    run("/bin/zsh", "-f", SOURCE / "disconnect.command")
    # Remove the old standalone copy so the Privacy picker has one target.
    (BASE / "aicusb").unlink(missing_ok=True)
    atomic_write(PLIST, data, 0o644)
    run("/bin/launchctl", "enable", "system/" + LABEL)
    run("/bin/launchctl", "bootstrap", "system", PLIST)
    print("已安装并启动开机联网服务；以后开机无需 sudo 或钥匙串弹窗。")
    print("若系统日志拒绝可移动宗卷访问，需在 macOS 隐私与安全性中单独授权 " + str(application))
    print("连接失败或网卡未插入时，每隔约 15 秒重试。日志：" + str(LOG))


def uninstall():
    check_directory(PLIST.parent)
    if BASE.exists() or BASE.is_symlink():
        check_directory(BASE)
    stop()
    if PLIST.exists() or PLIST.is_symlink():
        PLIST.unlink()
    if BASE.exists():
        shutil.rmtree(BASE)
    print("已卸载开机联网服务，并删除保存的 Wi-Fi 凭据。日志保留于 " + str(LOG))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["install", "uninstall", "start", "stop", "restart", "status", "preview"])
    parser.add_argument("--ssid", default="2-1501-5G")
    parser.add_argument("--location", default="0x00b00000")
    parser.add_argument("--replace-credential", action="store_true")
    args = parser.parse_args()
    if not 1 <= len(args.ssid.encode("utf-8")) <= 32:
        parser.error("SSID must contain 1-32 UTF-8 bytes")
    try:
        value = int(args.location, 0)
        if not 0 < value <= 0xffffffff:
            raise ValueError()
    except ValueError:
        parser.error("Invalid USB location")
    args.location = "0x%08x" % value
    if args.action == "preview":
        sys.stdout.buffer.write(plistlib.dumps(service_plist(args.ssid, args.location)))
        return
    if args.action == "status":
        run("/bin/launchctl", "print", "system/" + LABEL)
        return
    if os.geteuid() != 0:
        parser.error("This action requires sudo (installation only; boot runs as root automatically)")
    if args.action == "install":
        install(args.ssid, args.location, args.replace_credential)
    elif args.action == "uninstall":
        uninstall()
    elif args.action == "stop":
        stop()
    elif args.action == "restart":
        run("/bin/launchctl", "kickstart", "-k", "system/" + LABEL)
    elif args.action == "start":
        if not loaded():
            run("/bin/launchctl", "enable", "system/" + LABEL)
            run("/bin/launchctl", "bootstrap", "system", PLIST)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print("AIC autostart: " + str(error), file=sys.stderr)
        sys.exit(1)
