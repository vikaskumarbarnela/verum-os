"""Build and run the bare-metal VERUM kernel.

    python build.py          build  -> build/verum.elf
    python build.py run      build, then boot it in QEMU (-kernel: Multiboot, no ISO needed)
    python build.py test     build, boot headless with the demo typed in, dump the screen as text

Toolchain: nasm, clang + ld.lld (LLVM) as the i386-elf cross-compiler, qemu-system-i386.
"""

from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(HERE, "build")
KSRC = os.path.join(HERE, "kernel")

CANDIDATES = {
    "nasm": [r"C:\Program Files\NASM\nasm.exe", r"C:\Users\%USERNAME%\AppData\Local\bin\NASM\nasm.exe"],
    "clang": [r"C:\Program Files\LLVM\bin\clang.exe"],
    "ld.lld": [r"C:\Program Files\LLVM\bin\ld.lld.exe"],
    "qemu-system-i386": [r"C:\Program Files\qemu\qemu-system-i386.exe"],
}


def tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for c in CANDIDATES.get(name, []):
        c = os.path.expandvars(c)
        if os.path.exists(c):
            return c
    sys.exit(f"missing tool: {name} (install it, or add it to PATH)")


def run(cmd: list[str]) -> None:
    print("  " + " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
    subprocess.run(cmd, check=True)


CFLAGS = ["--target=i386-elf", "-m32", "-ffreestanding", "-fno-builtin", "-nostdlib", "-nostdinc",
          "-fno-stack-protector", "-fno-pic", "-fno-pie", "-mno-sse", "-mno-mmx", "-mno-80387",
          "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-std=c11"]


def build() -> str:
    os.makedirs(BUILD, exist_ok=True)
    # fonts: regenerate on Windows (GDI shapes the Devanagari); elsewhere the committed kernel/font.h is used
    if sys.platform == "win32":
        try:
            subprocess.run([sys.executable, "-X", "utf8", os.path.join(HERE, "fontgen.py")], check=True)
        except subprocess.CalledProcessError as e:
            print(f"fontgen failed ({e}); using the existing kernel/font.h")
    nasm, clang, lld = tool("nasm"), tool("clang"), tool("ld.lld")
    objs = []
    boot_o = os.path.join(BUILD, "boot.o")
    run([nasm, "-f", "elf32", os.path.join(HERE, "boot.asm"), "-o", boot_o])
    objs.append(boot_o)
    for src in sorted(glob.glob(os.path.join(KSRC, "*.c"))):
        obj = os.path.join(BUILD, os.path.basename(src)[:-2] + ".o")
        run([clang, *CFLAGS, "-c", src, "-o", obj])
        objs.append(obj)
    elf = os.path.join(BUILD, "verum.elf")
    run([lld, "-m", "elf_i386", "-T", os.path.join(HERE, "linker.ld"), "-o", elf, *objs])
    print(f"built {elf} ({os.path.getsize(elf) // 1024} KiB)")
    return elf


DISK = os.path.join(BUILD, "verum.img")
DISK_MB = 8


def ensure_disk(fresh: bool = False) -> str:
    """The ledger lives on a raw 8 MiB image attached as the primary ATA master."""
    if fresh or not os.path.exists(DISK):
        with open(DISK, "wb") as f:
            f.truncate(DISK_MB * 1024 * 1024)
    return DISK


def qemu_cmd(elf: str, headless: bool) -> list[str]:
    cmd = [tool("qemu-system-i386"), "-kernel", elf, "-m", "128", "-no-reboot",
           "-drive", f"file={ensure_disk()},format=raw,if=ide,index=0,media=disk"]
    if headless:
        cmd += ["-display", "none"]
    return cmd


KEYS = {" ": "spc", "-": "minus", ",": "comma", ".": "dot", "/": "slash", "=": "equal", ";": "semicolon",
        "'": "apostrophe", '"': "shift-apostrophe", "(": "shift-9", ")": "shift-0", "*": "shift-8",
        "+": "shift-equal", "_": "shift-minus", "<": "shift-comma", ">": "shift-dot", ":": "shift-semicolon",
        "?": "shift-slash", "!": "shift-1", "@": "shift-2", "\n": "ret"}


def sendkeys(text: str) -> list[str]:
    """Translate text into QEMU monitor `sendkey` commands."""
    out = []
    for ch in text:
        if ch in KEYS:
            out.append(f"sendkey {KEYS[ch]}")
        elif ch.isupper():
            out.append(f"sendkey shift-{ch.lower()}")
        else:
            out.append(f"sendkey {ch}")
    return out


SCRIPT = [
    "note The quick brown fox jumps over the lazy dog",
    "spawn wc wordcount", "spawn shout shout",
    "grant wc store get,put", "grant wc console print", "grant shout store get,put", "link shout wc next",
    "cells", "send shout @note",
    "run", "find stats",
    "@dump pipeline",
    "spawn mal malware", "grant mal console print",
    "send mal 1", "send mal 2", "send mal 3", "send mal 4", "send mal 5", "run", "cells", "verify",
    "@dump attacks",
    "@reboot",                                   # power-cycle: everything below must survive via the disk
    "find stats", "cells", "verify",
    "@dump reboot",
    "history", "rewind 8", "yes", "find stats", "cells", "verify",
    "@dump rewind",
    "@reboot",
    "cells", "verify", "dash",
    "@dump dash",
    "",                                          # any key leaves the dash
    "attic", "restore 0", "find stats", "verify",
    "@dump restore",
    "seat alice", "grant alice store get,find", "grant alice console print", "switch alice",
    "note alice was here", "spawn x wordcount", "grant wc store get", "whoami", "switch shell", "find note",
    "@dump seats",
    "seat bob hunter2", "switch bob wrong", "switch bob", "switch bob hunter2", "whoami", "switch shell", "ledger 4",
    "@dump login",
    "spawn e echo", "grant e console print", "spawn d deleg", "grant d store get,delegate", "link d e next",
    "send d \"get,put\"", "run", "send d \"get\"", "run", "cells",
    "@dump delegate",
    "spawn a worker", "spawn b worker", "grant a console print", "grant b console print",
    "send a 200", "send b 200", "run 2",
    "@dump preempt",
    "cells",
    "@dump preempt2",
    "spawn g ganana", "grant g store get,put", "grant g console print", "send g @note", "run", "find stats",
    "@dump sanskrit",
    "show ganana",
    "@dump devanagari",
    "lipi on", "eval (pratyekam (kAryam (ka) (guNanam ka ka)) (zreNI 5))", "note dharmo rakSati rakSitaH", "find note", "lipi off",
    "@dump lipi",
    "alarm e 1 \"tick\"", "alarms", "bg on",
    "@wait 3.5",
    "log", "alarms", "bg off", "unalarm 0",
    "@dump alarm",
    "open", "follow 2", "allow", "follow 1",
    "@dump browser",
    "spawn dbg abhajya", "grant dbg console print", "break dbg 4", "send dbg 30", "run", "where", "inspect", "step", "inspect", "unbreak dbg", "continue",
    "profile on", "send dbg 40", "run", "profile show",
    "@dump debugger",
    "uptime", "trap",
    "@dump trap",
]


def test(elf: str) -> None:
    """Boot headless, type the script via the QEMU monitor, dump the VGA screen at each @dump."""
    import time

    ensure_disk(fresh=True)             # every test starts from a blank disk
    cmd = qemu_cmd(elf, headless=True) + ["-monitor", "stdio"]
    proc = None
    dumps = []

    def mon(text: str, wait: float = 0.0) -> None:
        proc.stdin.write((text + "\n").encode()); proc.stdin.flush(); time.sleep(wait)

    def boot() -> None:
        nonlocal proc
        proc = subprocess.Popen(cmd, cwd=BUILD, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(3.0)                 # let the firmware load the kernel
        mon("")                         # the monitor swallows the first byte before its prompt

    def shutdown() -> None:
        mon("quit")
        proc.wait(timeout=10)

    boot()
    for line in SCRIPT:
        if line.startswith("@dump"):
            name = line.split()[1]
            time.sleep(1.0)
            mon(f"screendump {name}.ppm", 1.0)
            dumps.append(name)
            continue
        if line == "@reboot":           # power-cycle: only the disk carries state across this
            shutdown()
            boot()
            continue
        if line.startswith("@wait"):    # let background work happen
            time.sleep(float(line.split()[1]))
            continue
        for key in sendkeys(line + "\n"):
            mon(key, 0.02)
        time.sleep(0.4)
    shutdown()
    for name in dumps:
        subprocess.run([sys.executable, os.path.join(HERE, "ppm2png.py"),
                        os.path.join(BUILD, name + ".ppm"), os.path.join(BUILD, name + ".png")], check=True)


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "build"
    elf = build()
    if mode == "run":
        subprocess.run(qemu_cmd(elf, headless=False))
    elif mode == "test":
        test(elf)
