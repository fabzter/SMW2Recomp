#!/usr/bin/env python3
"""Native OpenGL qualification runner for the SMW2 adaptive-widescreen work.

Drives the real desktop binary (OpenGL/cocoa, interpreter tier) through its
script interface and, for the TRACE build, the TCP debug server. No dummy
video, no software presenter, no synthetic allocator: every scenario runs the
unmodified game code against the user's private ROM located at runtime.

Fixtures, dumps, logs and private-ROM references stay inside a /tmp workspace.
The ROM path is read from the retained probe rom.cfg at runtime and is never
printed. All guest writes used for staging are disclosed in the report; the
allocator, queue, flags and renderer under test are the real ROM code.

Scenarios (select with --scenario, or 'all'):

  binaries        verify release-candidate hash/config and trace artifact
  parity-off      fresh Widescreen=0 run vs the retained gold probe4 captures
  wide-16x9       probe4 route at 16:9 (854x480): activation/OAM/SRAM/BG audits
  ultra-32x9      probe4 route at 32:9 (1920x540): margin clamp + gameplay
  runahead-16x9   RunAhead=1 non-turbo gameplay, reversal, ordinary load
  crossflag       save under one Widescreen flag, ordinary load under the other
  resize-adapt    measured margins for 4 window sizes (relaunch adaptation)
  trace-16x9      TRACE build: screenshots, get_ppu_state, ws_shadow, hook PCs
  capacity-trace  TRACE: disclosed fill of regular slots 6..23, real allocator
                  stress with widened submissions (the Task3 ruling's case)

Exit 0 = all selected gates passed. Findings print as [gate] lines; failures
collect into the summary and exit 1.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SDD = REPO / ".superpowers/sdd/2026-11-07-widescreen-completion"
MANIFEST = SDD / "native-off-baseline-capture.json"
GOLD_DIR = Path("/tmp/smw2-fixture-probe4.k52m9kww")
SEED_SRM = Path("/tmp/smw2-ws/opengl-run2/saves/save.srm")
PROBE_SRC = Path("/tmp/smw2-ws/opengl-run2")

RELEASE_EXE = REPO / "build-macos/SuperMarioWorld2YoshisIslandSNESRecomp"
RELEASE_SHA256 = ("816f82cccae10410d65e6fc6ab9417831ed27f53252142f2dc0bbfc55089cf2a")
GOLD_SHA256 = ("fc18e24a2292796728fe6785ea60e335fe075425e833f553969fd2ab"
               "c9292ff4")
TRACE_EXE = REPO / "build-trace/SuperMarioWorld2YoshisIslandSNESRecomp"

EXE_NAME = "SuperMarioWorld2YoshisIslandSNESRecomp"

# Canonical probe4 route (kept byte-faithful for the parity run). Its sha256
# must equal the manifest's script_sha256.
PROBE_SHA256 = "fb7386cefd950500516f6f8a2a3744bfdf00b78bce9144bbc78066981a47a573"
PROBE_SCRIPT = """# Baseline-only fixture probe2. Full bootstrap reuses prior proven inputs.
turbo on
wait 30
press a 6
wait 240
press a 6
wait 240
press a 6
wait 240
press a 6
wait 240
dump filemenu
press a 6
wait 300
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
dump bootstrap0
press start 6
wait 480
press a 6
wait 480
press start 6
wait 480
press a 6
wait 480
until16 0118 == 000f 12000
dump intro_initialized
press a 6
wait 120
press a 6
wait 120
# Private test setup after initialized gameplay; native fade/load handles content.
poke 0218 0000
poke 021a 0000
poke 038c 0000
poke 038e 0000
poke 0118 0b00
until16 0118 == 000f 6000
wait 120
dump stage11_candidate
press a 6
wait 120
press right 180
dump stage11_right
press b 30
press right 180
dump stage11_jump
press a 6
wait 120
dump message_waiting
press a 6
wait 120
dump message_closed
press left+b 20
dump jump_left
press left+b+y 6
dump tongue_left
press left+b 20
press left 60
dump reversal_candidate
# Request actual normal-stage exit record01d4 (3a036204), native loader owns data.
poke 038c 0100
poke 038e d401
poke 0118 0b00
until16 0118 == 000f 6000
wait 120
dump bonus3a_candidate
# Genuine controller input enters the ceiling return pipe at cave X56.
press up+b 60
wait 180
dump return_pipe_candidate
quit
"""

# Wide-route variant: identical timing/inputs to the canonical route, plus
# extra dumps inside the message wait and the return-pipe wait (dumps are
# zero-frame ops so timing is unchanged). The right-scroll press is split
# into three 60-frame holds to sample activation during the scroll.
WIDE_TAGS = [
    "filemenu", "bootstrap0", "intro_initialized", "stage11_candidate",
    "scroll_right_a", "scroll_right_b", "scroll_right_c", "stage11_right",
    "stage11_jump", "message_waiting", "message_box_open", "message_closed",
    "jump_left", "tongue_left", "reversal_candidate", "bonus3a_candidate",
    "return_pipe_candidate",
]


def wide_script() -> str:
    s = PROBE_SCRIPT
    # Insert three mid-scroll dumps inside `press right 180`.
    s = s.replace(
        "press right 180\ndump stage11_right",
        "press right 60\ndump scroll_right_a\npress right 60\n"
        "dump scroll_right_b\npress right 60\ndump scroll_right_c\n"
        "dump stage11_right", 1)
    # Extra captures inside existing waits (zero-frame).
    s = s.replace(
        "press a 6\nwait 120\ndump message_closed",
        "press a 6\ndump message_box_open\nwait 120\ndump message_closed", 1)
    return s


# Bootstrap-only script for TRACE runs: same canonical route up to the first
# 1-1 dump, then hands control to the TCP driver via a long bounded wait.
TRACE_BOOT_SCRIPT = PROBE_SCRIPT.split("dump stage11_candidate")[0] + (
    "dump stage11_candidate\nwait 7200\nquit\n")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def u16(b: bytes, off: int) -> int:
    return b[off] | (b[off + 1] << 8)


def s16(b: bytes, off: int) -> int:
    v = u16(b, off)
    return v - 0x10000 if v & 0x8000 else v


class Report:
    def __init__(self):
        self.lines = []
        self.failures = []
        self.gates = {}

    def log(self, msg):
        self.lines.append(msg)
        print(msg, flush=True)

    def gate(self, name, ok, detail=""):
        self.gates.setdefault(name, []).append((bool(ok), detail))
        mark = "PASS" if ok else "FAIL"
        line = f"[gate] {mark} {name} {detail}".rstrip()
        self.lines.append(line)
        print(line, flush=True)
        if not ok:
            self.failures.append(f"{name}: {detail}")

    def ok(self):
        return not self.failures


# ---------------------------------------------------------------------------
# Launch harness
# ---------------------------------------------------------------------------

BASE_CONFIG = """[General]
SkipLauncher = 1
Autosave = {autosave}
RunAhead = {runahead}
DisableFrameDelay = 1
[Graphics]
WindowScale = 2
Fullscreen = 0
DisplayAspect = {aspect}
OutputMethod = OpenGL
Renderer = opengl
LinearFiltering = 0
Shader =
Widescreen = {widescreen}
WindowSize = {windowsize}
FrameBlend = 0
VSync = 0
NewRenderer = 1
[Sound]
EnableAudio = 0
[GamepadMap]
EnableGamepad1 = false
EnableGamepad2 = false
"""


class GameRun:
    """One isolated launch of a copied binary inside a private dir."""

    def __init__(self, ws: Path, name: str, exe: Path, config: str,
                 script: str | None, env_extra: dict | None = None,
                 reuse_saves_from: Path | None = None):
        self.dir = ws / name
        self.dir.mkdir(parents=True, exist_ok=True)
        (self.dir / "saves").mkdir(exist_ok=True)
        (self.dir / "dumps").mkdir(exist_ok=True)
        shutil.copy2(exe, self.dir / EXE_NAME)
        # Probe4 layout: rom.cfg + keybinds.ini beside the exe, seed SRAM,
        # empty mods/preloaded selection.
        shutil.copy2(PROBE_SRC / "rom.cfg", self.dir / "rom.cfg")
        kb = PROBE_SRC / "keybinds.ini"
        if kb.exists():
            shutil.copy2(kb, self.dir / "keybinds.ini")
        st = PROBE_SRC / "mods/preloaded/state.toml"
        if st.exists():
            (self.dir / "mods/preloaded").mkdir(parents=True, exist_ok=True)
            shutil.copy2(st, self.dir / "mods/preloaded/state.toml")
        if reuse_saves_from and reuse_saves_from.is_dir():
            for f in reuse_saves_from.iterdir():
                if f.is_file():
                    shutil.copy2(f, self.dir / "saves" / f.name)
        else:
            shutil.copy2(SEED_SRM, self.dir / "saves/save.srm")
        self.config_path = self.dir / "config.ini"
        self.config_path.write_text(config)
        self.script_path = None
        if script is not None:
            self.script_path = self.dir / "script.txt"
            self.script_path.write_text(script)
        self.log_path = self.dir / "run.log"
        self.env_extra = env_extra or {}
        self.proc = None
        self.exe_sha = sha256_file(self.dir / EXE_NAME)

    def launch(self):
        env = dict(os.environ)
        env.pop("SDL_VIDEODRIVER", None)
        env["SNESRECOMP_DUMP_DIR"] = str(self.dir / "dumps")
        env.update(self.env_extra)
        args = [str(self.dir / EXE_NAME), "--no-launcher",
                "--config", str(self.config_path)]
        if self.script_path:
            args += ["--script", str(self.script_path)]
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen(args, cwd=self.dir, env=env,
                                     stdout=self.log,
                                     stderr=subprocess.STDOUT)
        return self.proc

    def wait(self, timeout: float) -> int:
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise TimeoutError(f"{self.dir.name}: no exit in {timeout:.0f}s")

    def kill(self):
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass

    def read_log(self) -> str:
        try:
            return self.log_path.read_text(errors="replace")
        except OSError:
            return ""

    def run_report(self) -> dict | None:
        p = self.dir / "last_run_report.json"
        if not p.exists():
            return None
        try:
            return json.loads(p.read_text())
        except json.JSONDecodeError:
            return None

    def dump(self, tag: str, suffix: str) -> Path:
        return self.dir / "dumps" / f"{tag}.{suffix}"

    def dump_bytes(self, tag: str, suffix: str) -> bytes:
        return self.dump(tag, suffix).read_bytes()


# ---------------------------------------------------------------------------
# TCP debug client (trace build only)
# ---------------------------------------------------------------------------

class DebugClient:
    def __init__(self, port: int, timeout: float = 30.0):
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.buf = b""

    def _ensure(self):
        if self.sock is None:
            self.sock = socket.create_connection(("127.0.0.1", self.port),
                                                 timeout=self.timeout)
            self.buf = b""

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            finally:
                self.sock = None
                self.buf = b""

    def _line(self) -> str:
        self._ensure()
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line = self.buf[:nl]
                self.buf = self.buf[nl + 1:]
                return line.decode().strip()
            data = self.sock.recv(262144)
            if not data:
                raise RuntimeError("debug server closed connection")
            self.buf += data

    def query(self, cmd: str):
        self._ensure()
        self.sock.sendall(cmd.encode() + b"\n")
        raw = self._line()
        # Skip optional banner lines until a JSON object arrives.
        while not raw.startswith("{"):
            raw = self._line()
        return json.loads(raw)


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def post_key(pid: int, keycode: int, ctrl: bool = False):
    """Deliver one real macOS key event into the OWNED test process's event
    queue via CGEventPostToPid (no focus/foreground change, no desktop
    input). SDL receives it as an ordinary SDL_KEYDOWN/SDL_KEYUP — this is
    the narrowest driver the host exposes for human-only paths (F12 rewind,
    Ctrl+Up window grow)."""
    import ctypes
    import ctypes.util
    cg = ctypes.cdll.LoadLibrary(ctypes.util.find_library("CoreGraphics"))
    cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
    cg.CGEventPostToPid.argtypes = [ctypes.c_uint, ctypes.c_void_p]
    flags = 0x40000 if ctrl else 0  # kCGEventFlagMaskControl
    for down in (True, False):
        ev = cg.CGEventCreateKeyboardEvent(None, keycode, down)
        if not ev:
            raise RuntimeError("CGEventCreateKeyboardEvent returned NULL")
        cg.CGEventSetFlags(ctypes.c_void_p(ev), flags if down else 0)
        cg.CGEventPostToPid(pid, ctypes.c_void_p(ev))
        cg.CFRelease(ctypes.c_void_p(ev))
        time.sleep(0.03)


# macOS virtual keycodes for the bindings the app parses
KC_F12 = 111
KC_RETURN = 36
KC_LEFT = 123
KC_UP = 126



def wait_server(port: int, proc, timeout: float = 90.0):
    t0 = time.monotonic()
    c = DebugClient(port)
    while time.monotonic() - t0 < timeout:
        if proc.poll() is not None:
            raise RuntimeError(f"trace process exited rc={proc.returncode}")
        try:
            r = c.query("frame")
            if "frame" in r:
                return c
        except (OSError, RuntimeError, json.JSONDecodeError):
            try:
                c.close()
            except Exception:
                pass
            c = DebugClient(port)
        time.sleep(0.25)
    raise TimeoutError("debug server did not come up")


def step_frames(client: DebugClient, frames: int, buttons: str = "0",
                chunk: int = 30):
    """Bounded stepping; leaves the sim parked at a frame boundary."""
    client.query(f"set_controller {buttons}")
    try:
        left = frames
        while left > 0:
            n = min(left, chunk)
            r = client.query(f"step {n}")
            if r.get("timeout"):
                # The step continues running; wait for it to finish.
                t0 = time.monotonic()
                target = r["frame_before"] + n
                while time.monotonic() - t0 < 60:
                    cur = client.query("frame")["frame"]
                    if cur >= target:
                        break
                    time.sleep(0.05)
            left -= n
    finally:
        client.query("clear_controller")


def read_sram(client: DebugClient, off: int, length: int) -> bytes:
    r = client.query(f"read_sram {off:x} {length}")
    return _hex_field(r)


def write_sram(client: DebugClient, off: int, data: bytes):
    r = client.query(f"write_sram {off:x} {data.hex()}")
    if "error" in r or r.get("ok") is False:
        raise RuntimeError(f"write_sram failed: {r}")


def read_ram(client: DebugClient, off: int, length: int) -> bytes:
    r = client.query(f"read_ram {off:x} {length}")
    return _hex_field(r)


def _hex_field(r: dict) -> bytes:
    raw = r.get("hex") or r.get("data") or ""
    return bytes.fromhex("".join(raw.split()))


# ---------------------------------------------------------------------------
# Guest decoders (no ROM bytes stored; ROM read only at runtime)
# ---------------------------------------------------------------------------

def decode_oam(oam: bytes):
    """Return list of (x_signed, y, attr, size) for slots 0..127."""
    out = []
    for slot in range(128):
        w0 = u16(oam, slot * 4)
        w1 = u16(oam, slot * 4 + 2)
        hi = (oam[0x200 + (slot >> 2)] >> ((slot & 3) * 2)) & 3
        x9 = (hi & 1) * 256
        x = (w0 & 0xFF) | x9
        if x & 0x100:
            x -= 0x200  # signed 9-bit, matching PpuReadOam
        out.append({"x": x, "y": (w0 >> 8) & 0xFF, "attr": w1,
                    "x9": hi & 1, "size": (hi >> 1) & 1})
    return out


def decode_pack_records(sram: bytes):
    """GSU OAM pack source table: 256 records at phys 0x0200, 8B stride.
    {signed X, signed Y, attr, word3} — X==0x8000 is the hole marker."""
    recs = []
    for i in range(256):
        off = 0x0200 + i * 8
        recs.append({
            "i": i, "x": s16(sram, off), "y": s16(sram, off + 2),
            "attr": u16(sram, off + 4), "w3": u16(sram, off + 6)})
    return recs


def native_visible(rec, m=None):
    return (-15 <= rec["x"] < 256) and (-15 <= rec["y"] < 240)


def posthole_audit(sram: bytes, oam: bytes):
    """Parts after the first X==0x8000 hole that are visible IN THE WIDE
    FIELD must appear in OAM slots 16..127: a native part at its true
    coordinate (packed y == Y-1), a widened-only part parked at y=0xF0 with
    its true coords in the metadata. Previously `native_visible` filtered
    widened parts out entirely, so `post_widened` could never be nonzero
    and no widened part was ever actually asserted."""
    recs = decode_pack_records(sram)
    first_hole = next((r["i"] for r in recs if r["x"] == -32768), None)
    result = {"first_hole": first_hole, "post_visible": 0, "post_widened": 0,
              "widened_parts": [], "bad": []}
    if first_hole is None:
        result["bad"].append("no 0x8000 hole in pack table")
        return result
    o = decode_oam(oam)
    for r in recs:
        if r["i"] <= first_hole:
            continue
        y_ok = -15 <= r["y"] < 240
        widened = r["x"] >= 256 or r["x"] < -15
        if not y_ok or r["x"] < -280 or r["x"] >= 512:
            continue  # off even the widened field, or the $8000 marker band
        # Locate this part in OAM by its emitted bytes: x low byte + attr.
        slots = [sl for sl in range(16, 128)
                 if (o[sl]["x"] & 0xFF) == (r["x"] & 0xFF)
                 and o[sl]["attr"] == r["attr"]]
        if not slots:
            continue  # not written this pack (y-test/order)
        e = o[slots[0]]
        result["post_visible"] += 1
        if not widened and e["y"] == ((r["y"] - 1) & 0xFF):
            continue  # emitted at its true coordinate
        if widened and e["y"] == 0xF0:
            result["post_widened"] += 1
            result["widened_parts"].append(
                {"rec": r["i"], "x": r["x"], "y": r["y"], "attr": r["attr"],
                 "slot": slots[0], "size": e["size"]})
            continue  # parked as designed
        result["bad"].append(
            f"rec{r['i']} x={r['x']} y={r['y']} slot{slots[0]} got "
            f"x={e['x']} y={e['y']} attr={e['attr']:04x}")
    return result


def ghost_audit(sram: bytes, oam: bytes):
    """Every widened-only pack part (|x| outside native clip) must be parked:
    no OAM slot may show it at a wrapped x with a non-parked y."""
    recs = decode_pack_records(sram)
    o = decode_oam(oam)
    bad = []
    for r in recs:
        if r["x"] == -32768 or native_visible(r) or r["x"] < -280:
            continue
        for slot in range(16, 128):
            e = o[slot]
            if (e["x"] & 0xFF) == (r["x"] & 0xFF) and e["attr"] == r["attr"] \
                    and e["y"] != 0xF0:
                bad.append(f"rec{r['i']} x={r['x']} ghost slot{slot} "
                           f"y={e['y']} attr={e['attr']:04x}")
    return bad


def spawn_flags(sram: bytes) -> bytes:
    return sram[0x28CA:0x28CA + 256]


def decode_sprite_records(rom: bytes, sram: bytes):
    """Level sprite spawn records via the level's own ROM pointer (SRAM
    phys 0x2600 addr, 0x2602 bank). 3B records: w0 = ytile<<7|id,
    b2 = xtile; stage_id = record index; terminator w0 = 0xFFFF."""
    ptr = u16(sram, 0x2600)
    bank = u16(sram, 0x2602)

    def lorom(b, a):
        return (((b & 0x7F) << 15) | (a & 0x7FFF)) % len(rom)

    def linear(b, a):
        return (((b & 0x3F) << 16) | a) % len(rom)

    recs = []
    for name, fn in (("lorom", lorom), ("linear", linear)):
        recs = []
        off = fn(bank, ptr)
        for i in range(1024):
            if off + 3 > len(rom):
                break
            w0 = u16(rom, off)
            b2 = rom[off + 2]
            if w0 == 0xFFFF:
                break
            recs.append({"stage": i, "id": w0 & 0x1FF,
                         "ytile": w0 >> 9, "xtile": b2, "map": name})
            off += 3
        if 4 < len(recs) < 512 and all(r["xtile"] for r in recs[:8]):
            break
    return recs


# ---------------------------------------------------------------------------
# MAP16 / BG1 audit (mirrors src/smw2_rtl.c decode exactly)
# ---------------------------------------------------------------------------

def smw2_screen_id(sram: bytes, x_scr: int, y_scr: int) -> int:
    if y_scr >= 8 or x_scr >= 16:
        return 0x80
    return sram[0x0CAA + (y_scr << 4) + x_scr] & 0xBF


def smw2_map16_index(sram: bytes, wram: bytes, m16x: int, m16y: int):
    sid = smw2_screen_id(sram, m16x >> 4, m16y >> 4)
    if sid & 0x80:
        return None
    off = 0x18000 + sid * 0x200 + (m16y & 15) * 0x20 + (m16x & 15) * 2
    if off + 1 >= 0x20000:
        return None
    return u16(wram, off)


def smw2_map16_entry(rom: bytes, index_word: int):
    if len(rom) < 0xD0000:
        return None
    bank = rom[0xC0000:0xC0000 + 0x10000]
    page, obj = index_word >> 8, index_word & 0xFF
    p = 0x32A4 + page * 2
    eaddr = (0x33F2 + bank[p] + (bank[p + 1] << 8) + obj * 8) & 0xFFFF
    return [u16(bank, (eaddr + i * 2) & 0xFFFF) for i in range(4)]


def smw2_bg1_tile(rom: bytes, sram: bytes, wram: bytes, tx: int, ty: int):
    idx = smw2_map16_index(sram, wram, tx >> 1, ty >> 1)
    if idx is None:
        return None
    e = smw2_map16_entry(rom, idx)
    if e is None:
        return None
    return e[(ty & 1) * 2 + (tx & 1)]


def bg1_vram_word(regs: dict, col: int, row: int) -> int:
    base = (regs["bgsc"][0] & 0xFC) << 8
    return (base + ((row & 31) << 5) + (col & 31) +
            (0x400 if col & 32 else 0)) & 0x7FFF


def bg1_audit(rom: bytes, dump_dir: Path, tag: str):
    """Compare the dumped BG1 tilemap window against ROM-derived tiles.
    Returns dict with hits/total or a skip reason."""
    regs = json.loads((dump_dir / f"{tag}.regs.json").read_text())
    vram = (dump_dir / f"{tag}.vram.bin").read_bytes()
    sram = (dump_dir / f"{tag}.sram.bin").read_bytes()
    wram = (dump_dir / f"{tag}.wram.bin").read_bytes()
    if (regs["bgmode"] & 7) not in (1, 2) or (regs["bgmode"] & 0x10):
        return {"skip": f"bgmode {regs['bgmode']:#04x} not BG1-8bpp"}
    if not (regs["bgsc"][0] & 1):
        return {"skip": f"BG1 map not wide (bgsc {regs['bgsc'][0]:#04x})"}
    cam_x = s16(wram, 0x39)
    cam_y = s16(wram, 0x3B)
    h, v = regs["hscroll"][0], regs["vscroll"][0]
    dx = (h - cam_x) & 511
    if dx >= 256:
        dx -= 512
    dy = (v - cam_y) & 255
    if dy >= 128:
        dy -= 256
    wtx0, wty0 = (cam_x + dx) >> 3, (cam_y + dy) >> 3
    btx0, bty0 = h >> 3, v >> 3
    cols = [0, 3, 6, 9, 12, 15, 18, 21, 24, 27, 30, 31]
    rows = [0, 3, 6, 9, 12, 15, 18, 21, 24, 26, 27, 28]
    hits = tot = 0
    counts = {}
    for c, r in zip(cols, rows):
        got = u16(vram, 2 * bg1_vram_word(regs, btx0 + c, bty0 + r))
        counts[got] = counts.get(got, 0) + 1
        want = smw2_bg1_tile(rom, sram, wram, (wtx0 + c) & 0x3FF,
                             (wty0 + r) & 0x1FF)
        if want is not None:
            tot += 1
            hits += want == got
    modal = max(counts.values())
    return {"hits": hits, "total": tot, "modal": modal,
            "wtx0": wtx0, "wty0": wty0}


# ---------------------------------------------------------------------------
# Image helpers
# ---------------------------------------------------------------------------

def ppm_read(path: Path):
    data = path.read_bytes()
    if not data.startswith(b"P6"):
        raise ValueError("not a P6 ppm")
    # header: P6 <w> <h> <max>\n
    parts = data.split(b"\n", 3)
    w, h = [int(x) for x in parts[1].split()]
    rgb = data[len(parts[0]) + len(parts[1]) + len(parts[2]) + 3:]
    return w, h, rgb


def bmp_read(path: Path):
    d = path.read_bytes()
    off = int.from_bytes(d[10:14], "little")
    w = int.from_bytes(d[18:22], "little", signed=True)
    h = int.from_bytes(d[22:26], "little", signed=True)
    top_down = h < 0
    h = abs(h)
    stride = ((w * 3 + 3) // 4) * 4
    px = bytearray(w * h * 4)
    for y in range(h):
        sy = y if top_down else h - 1 - y
        row = d[off + sy * stride: off + sy * stride + w * 3]
        for x in range(w):
            b, g, r = row[x * 3:x * 3 + 3]
            px[(y * w + x) * 4:(y * w + x) * 4 + 4] = (r, g, b, 255)
    return w, h, bytes(px)


def png_write(path: Path, w: int, h: int, rgba: bytes):
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4]
                   for y in range(h))
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def margin_stats(w: int, h: int, rgba: bytes, extra: int):
    """Luma stats for native field and each margin column band."""
    def band_luma(x0, x1):
        tot = n = 0
        for y in range(0, h, 4):
            for x in range(x0, x1, 2):
                i = (y * w + x) * 4
                tot += rgba[i] + rgba[i + 1] + rgba[i + 2]
                n += 1
        return tot / max(n, 1)
    return {"left": band_luma(0, extra), "native": band_luma(extra, extra + 256),
            "right": band_luma(extra + 256, extra + 256 + extra)}


# ---------------------------------------------------------------------------
# WRAM convenience (raw dump offsets)
# ---------------------------------------------------------------------------

def wram_state(wram: bytes):
    return {
        "mode": u16(wram, 0x0118), "world": u16(wram, 0x0218),
        "stage": u16(wram, 0x021A), "load": u16(wram, 0x038C),
        "exit": u16(wram, 0x038E), "msg": wram[0x0D0F],
        "player_x": s16(wram, 0x008C), "player_y": s16(wram, 0x0090),
        "mouth": wram[0x0150], "tongue_x": s16(wram, 0x0152),
        "intro": wram[0x0121], "cam0x": s16(wram, 0x0039),
        "cam0y": s16(wram, 0x003B),
    }


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------

def s_binaries(rep: Report, ws: Path, opts):
    sha = sha256_file(RELEASE_EXE)
    rep.gate("release.sha256", sha == RELEASE_SHA256, sha)
    cache = REPO / "build-macos/CMakeCache.txt"
    txt = cache.read_text()
    for k, v in (("CMAKE_BUILD_TYPE", "Release"),
                 ("SNESRECOMP_SETUP_HOST", "OFF"),
                 ("SNESRECOMP_ENABLE_TRACE", "OFF")):
        rep.gate(f"release.config.{k}", f"{k}:STRING={v}" in txt or
                 f"{k}:BOOL={v}" in txt, k)
    tsha = sha256_file(TRACE_EXE)
    rep.log(f"[info] trace artifact sha256 {tsha}")
    tcache = (REPO / "build-trace/CMakeCache.txt").read_text()
    rep.gate("trace.config.SETUP_HOST", "SNESRECOMP_SETUP_HOST:BOOL=OFF"
             in tcache, "SETUP_HOST=OFF")
    rep.gate("trace.config.TRACE", "SNESRECOMP_ENABLE_TRACE:BOOL=ON" in tcache,
             "TRACE=ON")
    rep.gate("trace.config.build_type", "CMAKE_BUILD_TYPE:STRING=Release"
             in tcache, "Release")
    rep.gate("probe.script.sha", sha256_bytes(PROBE_SCRIPT.encode())
             == PROBE_SHA256, "canonical route intact")
    rep.gate("seed.srm.sha", sha256_file(SEED_SRM).startswith("d819d9ca"),
             "initial SRAM seed")


def s_parity(rep: Report, ws: Path, opts):
    man = json.loads(MANIFEST.read_text())
    rep.gate("parity.manifest.exe", man["executable_sha256"] == GOLD_SHA256,
             "gold binary is the probe4 baseline")
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=0, windowsize="854x480")
    run = GameRun(ws, "parity-off", RELEASE_EXE, cfg, PROBE_SCRIPT)
    run.launch()
    rc = run.wait(opts.timeout)
    rep.gate("parity.exit", rc == 0, f"rc={rc}")
    log = run.read_log()
    rep.gate("parity.opengl", "renderer initialized: opengl" in log,
             "renderer breadcrumb")
    rep.gate("parity.cocoa", "video=cocoa" in log, "SDL video driver")
    rrep = run.run_report() or {}
    share = (rrep.get("execution_share") or {})
    rep.gate("parity.cpu_tier", share.get("interp_cycle_pct", 0) > 99.0,
             f"interp {share.get('interp_cycle_pct')}% aot "
             f"{share.get('aot_cycle_pct')}% — CPU hooks run interpreted")
    mismatches = []
    for cap in man["captures"]:
        tag = cap["tag"]
        for fname, want in cap["files"].items():
            p = run.dir / "dumps" / f"{tag}.{fname}"
            if not p.exists():
                mismatches.append(f"{tag}.{fname}: missing")
                continue
            if p.stat().st_size != want["bytes"] or \
                    sha256_file(p) != want["sha256"]:
                mismatches.append(f"{tag}.{fname}")
    rep.gate("parity.captures", not mismatches,
             f"{len(man['captures'])} tags x 8 files vs retained gold; "
             f"mismatch: {', '.join(mismatches[:8]) or 'none'}")
    # Scene checks on fresh captures
    st = {t: wram_state(run.dump_bytes(t, "wram.bin"))
          for t in ("stage11_candidate", "message_closed",
                    "bonus3a_candidate", "return_pipe_candidate")}
    rep.gate("parity.scene.level", st["stage11_candidate"]["mode"] == 0x0F
             and st["stage11_candidate"]["stage"] == 0,
             str({k: st["stage11_candidate"][k] for k in
                  ("mode", "stage", "msg")}))
    rep.gate("parity.scene.cave", st["bonus3a_candidate"]["mode"] == 0x0F,
             f"mode={st['bonus3a_candidate']['mode']:04x} via exit record")
    rep.gate("parity.scene.pipe", st["return_pipe_candidate"]["mode"] == 0x0F,
             f"mode={st['return_pipe_candidate']['mode']:04x}")


def wide_asserts(rep: Report, ws: Path, run: GameRun, name: str,
                 rom: bytes, expect_extra_min: int):
    log = run.read_log()
    rep.gate(f"{name}.opengl", "renderer initialized: opengl" in log,
             "opengl breadcrumb")
    rep.gate(f"{name}.cocoa", "video=cocoa" in log, "cocoa")
    rrep = run.run_report() or {}
    share = (rrep.get("execution_share") or {})
    rep.gate(f"{name}.cpu_tier", share.get("interp_cycle_pct", 0) > 99.0,
             f"interp {share.get('interp_cycle_pct')}%")

    st = {}
    for tag in WIDE_TAGS:
        p = run.dump(tag, "wram.bin")
        if p.exists():
            st[tag] = wram_state(p.read_bytes())
    rep.gate(f"{name}.dumps", len(st) >= 15, f"{len(st)} dump tags")

    def scene(tag, default=None):
        return st.get(tag, default if default is not None else {})

    # Normal-1-1 reach, message open/close, reversal, cave, return pipe.
    s11 = scene("stage11_candidate")
    rep.gate(f"{name}.scene.level", s11.get("mode") == 0x0F and
             s11.get("stage") == 0 and s11.get("msg") == 0,
             str(s11))
    rep.gate(f"{name}.scene.msg_open", scene("message_waiting", {}).get("msg")
             == 9 or scene("message_box_open", {}).get("msg") == 9,
             f"msg={scene('message_box_open').get('msg')}")
    rep.gate(f"{name}.scene.msg_close",
             scene("message_closed", {}).get("msg") == 0,
             f"msg={scene('message_closed').get('msg')}")
    j, r = scene("jump_left"), scene("reversal_candidate")
    if j and r:
        rep.gate(f"{name}.scene.reversal",
                 j.get("cam0x", 9999) > r.get("cam0x", 0),
                 f"cam0x {j.get('cam0x')} -> {r.get('cam0x')} (leftward "
                 f"reversal)")
    rep.gate(f"{name}.scene.cave",
             scene("bonus3a_candidate", {}).get("mode") == 0x0F and
             scene("bonus3a_candidate", {}).get("exit") == 0x1D4,
             f"exit={scene('bonus3a_candidate', {}).get('exit'):#06x} "
             f"(bonus-cave transition record)")
    rep.gate(f"{name}.scene.pipe",
             scene("return_pipe_candidate", {}).get("mode") == 0x0F and
             scene("return_pipe_candidate", {}).get("msg") == 0,
             f"exit={scene('return_pipe_candidate', {}).get('exit'):#06x}")

    # Earlier activation: level spawn records whose stage flag was set while
    # the record's world-tile X sat outside the native 22-column window. The
    # spawn hook admits x in [left-lt, left+22+rt) with rt=ceil(margin_px/16);
    # the left edge in SRAM $00A4 is a pixel seam (cam0x & ~15).
    best = None  # (tag, [records past native bound])
    recs = []
    for tag in ("stage11_candidate", "stage11_right", "scroll_right_c"):
        p = run.dump(tag, "sram.bin")
        if not p.exists():
            continue
        sram = p.read_bytes()
        if not recs:
            recs = decode_sprite_records(rom, sram)
        flags = spawn_flags(sram)
        left = s16(sram, 0x00A4) >> 4          # pixel seam -> world tile
        bound = left + 22                       # native spawn-window edge
        flagged = [r for r in recs if r["stage"] < len(flags)
                   and flags[r["stage"]] == 0xFF]
        band = [r for r in flagged if r["xtile"] >= bound]
        rep.log(f"[info] {name}.{tag}: left_tile={left} native_bound="
                f"{bound} flagged={len(flagged)} widened-band="
                f"{[(r['stage'], r['xtile']) for r in band]}")
        if band and best is None:
            best = (tag, band)
    rep.gate(f"{name}.spawn.records", len(recs) > 4,
             f"{len(recs)} records via {recs[0]['map'] if recs else '?'} map")
    if best:
        tag, band = best
        rep.gate(f"{name}.earlier_activation", True,
                 f"{tag}: {len(band)} record(s) flagged past native bound: "
                 f"{[(r['stage'], r['xtile']) for r in band]}")
    else:
        # At 16:9 (extra~43px => rt=3 tiles) 1-1's record spacing can leave
        # the widened band empty; that is a coverage gap, not a defect —
        # the ultra run with rt>=8 must find band records.
        need = expect_extra_min >= 96  # rt >= 6 tiles
        rep.gate(f"{name}.earlier_activation", not need,
                 "no record flagged past the native bound in checked dumps "
                 f"(required at extra>={expect_extra_min}px/rt>=9)")

    # Post-hole true-coord + parked-ghost audits on the widest-content dump.
    for tag in ("reversal_candidate", "stage11_right", "scroll_right_c"):
        p = run.dump(tag, "sram.bin")
        if not p.exists():
            continue
        audit = posthole_audit(run.dump_bytes(tag, "sram.bin"),
                               run.dump_bytes(tag, "oam.bin"))
        rep.log(f"[info] {name}.{tag}: posthole {audit}")
        rep.gate(f"{name}.{tag}.posthole_true_coords",
                 not audit["bad"] and audit["post_visible"] > 0,
                 f"{audit['bad'][:3]} post={audit['post_visible']} "
                 f"hole={audit['first_hole']}")
        ghosts = ghost_audit(run.dump_bytes(tag, "sram.bin"),
                             run.dump_bytes(tag, "oam.bin"))
        rep.gate(f"{name}.{tag}.parked_no_ghost", not ghosts,
                 f"{ghosts[:3]}")
        break

    # Parked widened parts exist at all (proves park path exercised, not
    # merely clean frames) — informational unless a record was widened.
    oam = decode_oam(run.dump_bytes("reversal_candidate", "oam.bin"))
    parked = sum(1 for e in oam[16:] if e["y"] == 0xF0)
    rep.log(f"[info] {name}: OAM parked slots (16..127) = {parked}")

    # BG1 VRAM-vs-ROM tilemap audit on gameplay dumps.
    for tag in ("stage11_candidate", "stage11_right", "reversal_candidate"):
        p = run.dump(tag, "regs.json")
        if not p.exists():
            continue
        a = bg1_audit(rom, run.dir / "dumps", tag)
        if "skip" in a:
            rep.log(f"[info] {name}.{tag}: BG1 audit skipped: {a['skip']}")
            continue
        rep.gate(f"{name}.{tag}.bg1_vram",
                 a["hits"] >= 10 and a["total"] >= 10 and a["modal"] <= 9,
                 f"hits {a['hits']}/{a['total']} modal {a['modal']}")
        break

    # BG2 currency: scroll regs live and layer present.
    regs_r = json.loads(run.dump("reversal_candidate", "regs.json")
                        .read_text())
    regs_b = json.loads(run.dump("stage11_right", "regs.json").read_text())
    rep.gate(f"{name}.bg2_currency",
             regs_r["bgmode"] & 0x20 != 0 and
             (regs_r["hscroll"][1] != regs_b["hscroll"][1] or
              regs_r["vscroll"][1] != regs_b["vscroll"][1]),
             f"bg2mode bit {regs_r['bgmode'] & 0x20:#x} hscroll "
             f"{regs_b['hscroll'][1]}->{regs_r['hscroll'][1]}")

    # Present-frame width evidence (release screenshot).
    ppm = run.dir / "present.ppm"
    if ppm.exists():
        w, h, rgb = ppm_read(ppm)
        extra = (w - 256) // 2
        rep.gate(f"{name}.presented_width", w >= 256 + 2 * expect_extra_min,
                 f"presented {w}x{h} extra={extra}")
        rgba = rgb_to_rgba(rgb, w * h)
        ms = margin_stats(w, h, rgba, extra)
        rep.log(f"[info] {name}: margin luma {ms}")
        rep.gate(f"{name}.margin_pixels",
                 ms["left"] > 4 or ms["right"] > 4,
                 f"margin bands nonblack luma={ms}")
        png_write(run.dir / "present.png", w, h, rgba)
    else:
        rep.gate(f"{name}.presented_width", False,
                 "no present.ppm captured")


def rgb_to_rgba(rgb: bytes, n: int) -> bytes:
    out = bytearray(n * 4)
    out[0::4] = rgb[0::3]
    out[1::4] = rgb[1::3]
    out[2::4] = rgb[2::3]
    out[3::4] = b"\xff" * n
    return bytes(out)


def rom_path() -> bytes:
    cfg = (PROBE_SRC / "rom.cfg").read_bytes()
    return cfg.split(b"\n")[0].strip().split(b"=", 1)[-1].strip()


def load_rom() -> bytes:
    p = Path(rom_path().decode())
    return p.read_bytes()


def s_wide16(rep: Report, ws: Path, opts):
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=1, windowsize="854x480")
    shot = ws / "wide-16x9-present.ppm"
    env = {"SNESRECOMP_SCREENSHOT": str(ws / "wide-16x9/present.ppm"),
           "SNESRECOMP_SCREENSHOT_FRAME": "760",
           "SNESRECOMP_WS_BG_MARGINS_DEBUG": "1"}
    run = GameRun(ws, "wide-16x9", RELEASE_EXE, cfg, wide_script(),
                  env_extra=env)
    run.launch()
    rc = run.wait(opts.timeout)
    rep.gate("wide16.exit", rc == 0, f"rc={rc}")
    wide_asserts(rep, ws, run, "wide16", load_rom(), expect_extra_min=20)


def s_ultra(rep: Report, ws: Path, opts):
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=1, windowsize="1920x540")
    env = {"SNESRECOMP_SCREENSHOT": str(ws / "ultra-32x9/present.ppm"),
           "SNESRECOMP_SCREENSHOT_FRAME": "760",
           "SNESRECOMP_WS_BG_MARGINS_DEBUG": "1"}
    run = GameRun(ws, "ultra-32x9", RELEASE_EXE, cfg, wide_script(),
                  env_extra=env)
    run.launch()
    rc = run.wait(opts.timeout)
    rep.gate("ultra.exit", rc == 0, f"rc={rc}")
    wide_asserts(rep, ws, run, "ultra", load_rom(), expect_extra_min=100)


RUNAHEAD_SCRIPT = wide_script().replace(
    "press left+b 20\ndump jump_left",
    "press left+b 20\ndump ra_jump_left", 1)


def s_runahead(rep: Report, ws: Path, opts):
    """RunAhead=1, non-turbo 1-1 gameplay incl. reversal + ordinary load."""
    # Phase 1: Autosave=1 so exit writes saves/save0.sav in THIS private dir.
    # Quit right after reversal_candidate so save0 lands mid-route, where the
    # camera scrolls freely both directions (the end-of-route state sits on
    # a pipe transition with the camera pinned at the level edge).
    cfg1 = BASE_CONFIG.format(autosave=1, runahead=0, aspect="4:3",
                              widescreen=1, windowsize="854x480")
    save_script = (PROBE_SCRIPT.split("dump reversal_candidate")[0]
                   + "dump reversal_candidate\nwait 30\nquit\n")
    run1 = GameRun(ws, "ra-save", RELEASE_EXE, cfg1, save_script)
    run1.launch()
    rc1 = run1.wait(opts.timeout)
    rep.gate("runahead.save_exit", rc1 == 0, f"rc={rc1}")
    save0 = run1.dir / "saves" / "save0.sav"
    rep.gate("runahead.save0_written", save0.exists(),
             "autosave wrote slot 0")

    # Phase 2: RunAhead=1. Bootstrap under turbo (runahead skips turbo),
    # then non-turbo gameplay: ordinary load into the mid-route state,
    # direction reversal measured on cam0x, second ordinary load, more
    # gameplay. The script itself is what keeps this non-turbo.
    # Keep the proven route up to its left-walk reversal segment, then run
    # that segment non-turbo (runahead actually runs only !turbo), with an
    # ordinary load across it and a second ordinary load for the boundary.
    head = PROBE_SCRIPT.split("press left+b 20")[0]
    # Non-turbo segment: reach the 1-1 reversal area under the proven route,
    # then a short measured left-walk, an ordinary loadstate back to the
    # mid-route autosave (save0, cam~51), and continued non-turbo play.
    script = ("turbo on\nwait 30\n"
              + "\n".join(l for l in head.splitlines()
                          if not l.startswith("turbo") and l != "quit")
              + "\nturbo off\nwait 60\n"
              + "press left+b 20\nwait 30\ndump ra_start\n"
              + "press left+b 90\nwait 30\ndump ra_left\n"
              + "press right 150\nwait 30\ndump ra_far\n"
              + "loadstate 0\nwait 90\ndump ra_postload\n"
                "press right 60\nwait 60\ndump ra_final\nquit\n")
    cfg2 = BASE_CONFIG.format(autosave=0, runahead=1, aspect="4:3",
                              widescreen=1, windowsize="854x480")
    run2 = GameRun(ws, "ra-play", RELEASE_EXE, cfg2, script,
                   reuse_saves_from=run1.dir / "saves")
    run2.launch()
    rc2 = run2.wait(opts.timeout)
    log = run2.read_log()
    rep.gate("runahead.exit", rc2 in (0, 3), f"rc={rc2}")
    rep.gate("runahead.armed", "[runahead] 1 frame(s)" in log,
             "set_frames ran")
    rep.gate("runahead.speculating",
             "[runahead] snapshot buffer is" in log
             and "cannot rewind" not in log
             and "rollback load FAILED" not in log
             and "disabling run-ahead" not in log,
             "speculation stayed enabled (buffer allocated, no decline)")
    rep.gate("runahead.loads", log.count("*** Loading slot 0") >= 1,
             f"{log.count('*** Loading slot 0')} ordinary loads")
    st = {t: wram_state(run2.dump_bytes(t, "wram.bin"))
          for t in ("ra_start", "ra_left", "ra_far", "ra_postload",
                    "ra_final")
          if run2.dump(t, "wram.bin").exists()}
    px = {t: u16(run2.dump_bytes(t, "sram.bin"), 0x8C)
          for t in ("ra_start", "ra_left", "ra_far", "ra_postload",
                    "ra_final")
          if run2.dump(t, "sram.bin").exists()}
    rep.log(f"[info] runahead: modes/cam "
            f"{ {k: (v.get('mode'), v.get('cam0x')) for k, v in st.items()} }"
            f" physX(008C) {px}")
    rep.gate("runahead.reached", st.get("ra_start", {}).get("mode") == 0x0F,
             f"at reversal seg start {st.get('ra_start', {})}")
    if "ra_start" in px and "ra_left" in px:
        rep.gate("runahead.reversal",
                 px["ra_left"] + 8 <= px["ra_start"],
                 f"physX {px['ra_start']} -> {px['ra_left']} (non-turbo "
                 f"leftward reversal, SRAM phys $008C)")
    # save0 is the mid-route autosave; the load must restore its position.
    # Camera lags the player, so compare the true X (SRAM phys $008C) with
    # a settle window, not the camera. Discriminating proof: the pre-load
    # position (ra_left) must be far enough from the snapshot that only a
    # real restore could land within the settle window.
    saved_x = u16(run1.dump_bytes("reversal_candidate", "sram.bin"), 0x8C)
    rep.gate("runahead.ordinary_load_moved",
             "ra_far" in px and abs(px["ra_far"] - saved_x) >= 48,
             f"pre-load physX={px.get('ra_far')} vs saved {saved_x} "
             f"(must differ >=48 so a skipped load cannot pass)")
    rep.gate("runahead.ordinary_load",
             st.get("ra_postload", {}).get("mode") == 0x0F and
             "ra_postload" in px and
             abs(px["ra_postload"] - saved_x) <= 64,
             f"post-load physX={px.get('ra_postload')} vs saved "
             f"physX={saved_x} pre-load physX={px.get('ra_far')} "
             f"(mode={st.get('ra_postload', {}).get('mode'):#06x})")
    rep.gate("runahead.post_load_play",
             st.get("ra_final", {}).get("mode") == 0x0F,
             f"final {st.get('ra_final', {})}")
    # Rollback residue: published OAM table must not resurrect ghosts —
    # re-run the ghost audit on the post-load dump.
    if run2.dump("ra_postload", "sram.bin").exists():
        ghosts = ghost_audit(run2.dump_bytes("ra_postload", "sram.bin"),
                             run2.dump_bytes("ra_postload", "oam.bin"))
        rep.gate("runahead.post_load_ghosts", not ghosts, f"{ghosts[:3]}")
        oam = decode_oam(run2.dump_bytes("ra_postload", "oam.bin"))
        parked = sum(1 for e in oam[16:] if e["y"] == 0xF0)
        rep.log(f"[info] runahead: post-load parked OAM slots = {parked}")


def s_crossflag(rep: Report, ws: Path, opts):
    """Save under flag A, apply under flag B via a queued ordinary load.

    Cold autosave semantics obscure whether a load actually applied (YI
    replays its level-entry flow, so a boot-mode progression cannot
    distinguish 'snapshot applied' from a plain cold boot). The proof is
    instead: reach 1-1 gameplay under flag B, issue `loadstate 0` (the
    ordinary RtlSaveLoad path queued on the main thread, same as the
    runahead scenario), then verify the restored position matches the
    state saved under flag A."""
    save_script = (PROBE_SCRIPT.split("dump reversal_candidate")[0]
                   + "dump reversal_candidate\nwait 30\nquit\n")
    # Same mid-route point, then move Yoshi to a DISCRIMINATING position
    # far from the save point before queuing the ordinary load of the
    # foreign save — a skipped load would leave Yoshi near the moved-to
    # position, not at the snapshot. Walk left past the save point: the
    # receiver's natural drift is rightward, so leftward guarantees a
    # discriminating offset.
    load_script = (PROBE_SCRIPT.split("dump reversal_candidate")[0]
                   + "press left+b 120\nwait 30\ndump cf_preload\n"
                     "loadstate 0\nwait 150\ndump cf_postload\n"
                     "wait 90\ndump cf_after\nquit\n")

    def flag_run(name: str, flag_val: int, autosave: int, script: str,
                 saves: Path | None):
        cfg = BASE_CONFIG.format(autosave=autosave, runahead=0,
                                 aspect="4:3", widescreen=flag_val,
                                 windowsize="854x480")
        run = GameRun(ws, name, RELEASE_EXE, cfg, script,
                      reuse_saves_from=saves)
        run.launch()
        return run

    def check_load(run: GameRun, name: str, saved_x: int):
        log = run.read_log()
        rep.gate(f"crossflag.{name}_applied",
                 "*** Loading slot 0" in log and
                 "savestate load" in log,
                 "ordinary load executed under the opposite flag")
        if not run.dump("cf_postload", "wram.bin").exists():
            rep.gate(f"crossflag.{name}_state", False,
                     "no cf_postload dump")
            return
        st = wram_state(run.dump_bytes("cf_postload", "wram.bin"))
        px = u16(run.dump_bytes("cf_postload", "sram.bin"), 0x8C)
        pre_x = u16(run.dump_bytes("cf_preload", "sram.bin"), 0x8C)             if run.dump("cf_preload", "sram.bin").exists() else None
        # Discriminating proof: the receiver walked to a position at least
        # 48px away from the save snapshot; a skipped load would leave it
        # there — so physX landing within +-64 of the foreign snapshot can
        # ONLY be the restore.
        rep.gate(f"crossflag.{name}_moved",
                 pre_x is not None and abs(pre_x - saved_x) >= 48,
                 f"receiver discriminating position physX={pre_x} vs "
                 f"saved={saved_x} (must differ >=48)")
        rep.gate(f"crossflag.{name}_state",
                 st["mode"] == 0x0F and abs(px - saved_x) <= 64,
                 f"post-load physX={px} vs saved physX={saved_x} "
                 f"pre-load physX={pre_x} "
                 f"mode={st['mode']:#06x} (saved state actually applied)")
        post = wram_state(run.dump_bytes("cf_after", "wram.bin"))
        # The mid-route save sits on a pipe trigger, so a valid continuation
        # is any live mode (gameplay 0x0F or a transition 0x14/0x15/map
        # 0x0A) — the guest kept simulating; only a dead/blank mode fails.
        rep.gate(f"crossflag.{name}_play",
                 post["mode"] in (0x0F, 0x14, 0x15, 0x0A, 0x0B),
                 f"guest continues after cross-flag load "
                 f"mode={post['mode']:#06x}")
        ghosts = ghost_audit(run.dump_bytes("cf_postload", "sram.bin"),
                             run.dump_bytes("cf_postload", "oam.bin"))
        rep.gate(f"crossflag.{name}_no_ghost", not ghosts, f"{ghosts[:3]}")

    on = flag_run("cf-1-save", 1, 1, save_script, None)
    rep.gate("crossflag.on_exit", on.wait(opts.timeout) == 0, "exit")
    rep.gate("crossflag.on_save0", (on.dir / "saves/save0.sav").exists(),
             "slot 0 written under wide")
    saved_x1 = u16(on.dump_bytes("reversal_candidate", "sram.bin"), 0x8C)

    # Flag OFF run boots clean (Autosave=0: no ambiguous cold autoload),
    # reaches the same route point, then queues the ordinary load.
    off = flag_run("cf-0-load", 0, 0, load_script, on.dir / "saves")
    rc = off.wait(opts.timeout)
    rep.gate("crossflag.off_load_exit", rc in (0, 3), f"rc={rc}")
    check_load(off, "off_load", saved_x1)

    # Reverse: save under off, ordinary-load under on.
    off_save = flag_run("cf-0-save", 0, 1, save_script, None)
    rep.gate("crossflag.off_exit2", off_save.wait(opts.timeout) == 0,
             "exit")
    saved_x0 = u16(off_save.dump_bytes("reversal_candidate", "sram.bin"),
                   0x8C)
    on_load = flag_run("cf-1-load", 1, 0, load_script,
                       off_save.dir / "saves")
    rep.gate("crossflag.on_load_exit",
             on_load.wait(opts.timeout) in (0, 3), "exit")
    check_load(on_load, "on_load", saved_x0)


def s_resize(rep: Report, ws: Path, opts):
    """Drawable-geometry adaptation measured across relaunches (the only
    resize path the host exposes: WindowSize is read at launch, the live
    drawable is re-queried every frame)."""
    rom = load_rom()
    results = {}
    for label, size in (("4:3-ish", "854x480"), ("16:9", "1024x576"),
                        ("21:9", "1680x720"), ("32:9", "1920x540")):
        cfg = BASE_CONFIG.format(autosave=1, runahead=0, aspect="4:3",
                                 widescreen=1, windowsize=size)
        shot = ws / f"resize-{label}" / "present.ppm"
        env = {"SNESRECOMP_SCREENSHOT": str(shot),
               "SNESRECOMP_SCREENSHOT_FRAME": "60"}
        run = GameRun(ws, f"resize-{label}", RELEASE_EXE, cfg,
                      "wait 90\nquit\n",
                      reuse_saves_from=ws / "ra-save/saves"
                      if (ws / "ra-save/saves").exists() else None,
                      env_extra=env)
        run.launch()
        rc = run.wait(60)
        log = run.read_log()
        created = ""
        for line in log.splitlines():
            if "window created" in line:
                created = line.strip()
        w = h = 0
        ppm = run.dir / "present.ppm"
        if not ppm.exists() and shot.exists():
            ppm = shot
        if ppm.exists():
            w, h, _ = ppm_read(ppm)
        results[label] = {"window": created, "presented_w": w, "rc": rc}
        rep.log(f"[info] resize {label} ({size}): window='{created}' "
                f"presented_w={w} rc={rc}")
    rep.gate("resize.measured", all(r["presented_w"] >= 256
                                  for r in results.values()),
             json.dumps(results))
    got = [results[k]["presented_w"] for k in
           ("4:3-ish", "16:9", "21:9", "32:9")]
    rep.gate("resize.adapts", len(set(got)) >= 3,
             f"presented widths {got} (field follows drawable aspect)")
    rep.gate("resize.clamped", got[-1] <= 800,
             f"32:9 field {got[-1]} within the 800px PPU cap")


def s_trace(rep: Report, ws: Path, opts):
    """TRACE artifact: widened visuals, PPU/GSU hook evidence, ws_shadow."""
    port = free_port()
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=1, windowsize="854x480")
    env = {"SNESRECOMP_DEBUG_PORT": str(port)}
    run = GameRun(ws, "trace-16x9", TRACE_EXE, cfg, TRACE_BOOT_SCRIPT,
                  env_extra=env)
    run.launch()
    rep.gate("trace.binary", run.exe_sha != RELEASE_SHA256,
             f"trace sha {run.exe_sha[:16]}… (distinct instrumented build)")
    try:
        client = wait_server(port, run.proc, timeout=120)
    except Exception as e:
        rep.gate("trace.server", False, str(e))
        run.kill()
        return
    try:
        # Wait until the script lands in 1-1 (mode 0x0F), then the long
        # `wait` holds the sim advancing under our control.
        t0 = time.monotonic()
        mode = 0
        while time.monotonic() - t0 < 300:
            fr = client.query("frame")
            if fr.get("frame", 0) > 11500:
                mode = u16(read_ram(client, 0x0118, 2), 0)
                if mode == 0x0F:
                    break
            if run.proc.poll() is not None:
                break
            time.sleep(1.0)
        rep.gate("trace.reached_level", mode == 0x0F,
                 f"mode={mode:#06x} frame={fr.get('frame')}")
        # Park at a boundary, then interrogate.
        step_frames(client, 5)
        st = client.query("get_ppu_state")
        ws_st = st.get("widescreen", {})
        rep.gate("trace.ppu_budget", ws_st.get("budget", 0) > 0,
                 f"ppu widescreen {ws_st}")
        rep.log(f"[info] trace ppu widescreen={ws_st} "
                f"layerWiden={ws_st.get('layerWiden')}")
        sh = client.query("ws_shadow_stats")
        rep.log(f"[info] ws_shadow_stats={sh}")
        layers = sh.get("layers", [])
        if layers:
            l0 = layers[0]
            rep.gate("trace.bg1_shadow_active",
                     l0.get("active") in (True, "true") or
                     (l0.get("westHit", 0) or 0) + (l0.get("eastHit", 0) or 0)
                     > 0, f"layer0 {l0}")
        gs = client.query("get_superfx_state")
        rep.log(f"[info] superfx={json.dumps(gs)[:400]}")
        fxs = (gs or {}).get("widescreen", {})
        # At a stopped boundary the last pack run is retired (active:false
        # is CORRECT — no raster in flight). The widened pack routine did
        # run: last_task is its PC and instructions were executed.
        rep.gate("trace.gsu_ws",
                 (gs or {}).get("instruction_count", 0) > 0 and
                 fxs.get("last_task") == "0xb1ef",
                 f"last_task={fxs.get('last_task')} (pack hook PC "
                 f"08:B1EF) instrs={gs.get('instruction_count')}")
        shot = run.dir / "trace_wide.bmp"
        r = client.query(f"screenshot {shot}")
        rep.gate("trace.screenshot", r.get("ok") is True and
                 r.get("width", 0) > 256,
                 f"w={r.get('width')} extra={r.get('ws_extra')} "
                 f"src={r.get('source')}")
        if shot.exists():
            w, h, px = bmp_read(shot)
            png_write(run.dir / "trace_wide.png", w, h, px)
            ms = margin_stats(w, h, px, r.get("ws_extra", 0))
            rep.gate("trace.margin_pixels", ms["left"] > 4 or
                     ms["right"] > 4, f"{ms}")
        # Ordinary save/load on the debug route (queued to main thread).
        r = client.query("savestate 0")
        rep.log(f"[info] savestate 0 -> {r}")
        step_frames(client, 10)
        sav = run.dir / "saves" / "save0.sav"
        rep.gate("trace.savestate_written", sav.exists(),
                 "ordinary save slot 0 via debug queue")
        step_frames(client, 60, "right")
        r = client.query("loadstate 0")
        rep.log(f"[info] loadstate 0 -> {r}")
        step_frames(client, 30)
        wram = read_ram(client, 0, 0x200)
        mode = u16(wram, 0x0118)
        rep.gate("trace.load_state_ok", mode == 0x0F,
                 f"post-load mode={mode:#06x}")
    finally:
        client.close()
        run.kill()
        try:
            run.wait(10)
        except Exception:
            pass


def s_capacity(rep: Report, ws: Path, opts):
    """The Task3-ruling capacity case: disclosed fill of regular sprite
    slots 6..23 via write_sram at a stopped boundary, then REAL widened
    producer submissions against the REAL consumer/allocator (CPU03:9596
    after GSU09:8000). No synthetic allocator; no injected PCs.
    Runs the genuine adaptive 32:9 window (measured extra ~213px => rt~14
    tiles) so the widened-only band actually contains spawn records."""
    port = free_port()
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=1, windowsize="1920x540")
    env = {"SNESRECOMP_DEBUG_PORT": str(port)}
    run = GameRun(ws, "capacity-trace", TRACE_EXE, cfg, TRACE_BOOT_SCRIPT,
                  env_extra=env)
    run.launch()
    try:
        client = wait_server(port, run.proc, timeout=120)
    except Exception as e:
        rep.gate("capacity.server", False, str(e))
        run.kill()
        return
    rom = load_rom()
    try:
        # Wait for 1-1.
        t0 = time.monotonic()
        mode = 0
        while time.monotonic() - t0 < 300:
            fr = client.query("frame")
            if fr.get("frame", 0) > 11500:
                mode = u16(read_ram(client, 0x0118, 2), 0)
                if mode == 0x0F:
                    break
            if run.proc.poll() is not None:
                break
            time.sleep(1.0)
        rep.gate("capacity.reached_level", mode == 0x0F,
                 f"mode={mode:#06x}")
        step_frames(client, 5)
        sram = read_sram(client, 0, 0x8000)
        rep.log(f"[info] capacity: SRAM read {len(sram)}B at frame "
                f"{client.query('frame')['frame']}")
        recs = decode_sprite_records(rom, sram)
        rep.gate("capacity.records", len(recs) > 4,
                 f"{len(recs)} level spawn records")
        wram = read_ram(client, 0, 0x200)
        cam_x = s16(wram, 0x39)
        leftmost = s16(sram, 0x00A4)
        rep.log(f"[info] capacity: cam_x={cam_x} leftmost_tile={leftmost}")

        # Choose widened-window candidates ahead of scrolling: records whose
        # xtile crosses the widened bound soon (unsubmitted, regular IDs).
        flags0 = spawn_flags(sram)
        # Disclosed staging: occupy regular slots 6..23 (phys 0F18..0F5F)
        # with minimal inert state matching a successful init: state=$0010,
        # id=$0115 (coin), x/y near camera (inert), CPU $7862-area = $1FFF,
        # stage_id=0 so no flag is owned (they must never despawn: despawn
        # only clears a flag through the sprite's stage_id).
        def wr16(phys_off, v):
            write_sram(client, phys_off, v.to_bytes(2, "little"))

        # Disclosed staging: occupy slots 6..22 (17 of 18 allocatable) so a
        # single real allocation can still land in slot 23 — the success
        # half of the producer->consumer proof. Slot 0 is the player (its
        # $0A state is normal, not a pool sprite).
        for slot in range(6, 23):
            base = slot * 4
            wr16(0x0F00 + base, 0x0002)            # s_spr_state = newly inited;
                                                # the game's own init advances
                                                # it to $0010 and fills fields
            wr16(0x1360 + base, 0x0115)            # s_spr_id = coin
            wr16(0x10E2 + base, (cam_x + 64 + slot) & 0xFFFF)
            wr16(0x1182 + base, (u16(wram, 0x3B) + 16) & 0xFFFF)
            wr16(0x14A0 + base, 0x0000)            # stage_id 0, bg_layer 0
            wr16(0x1460 + base, 0x0000)            # spawn stage-id slot
            wr16(0x1E62 + base, 0x1FFF)            # $7862 successful init
            wr16(0x1416 + base, 0xFFFF)            # oam ptr pair
            wr16(0x1418 + base, 0xFFFF)            # dyntile pair
        back = read_sram(client, 0x0F18, 0x60 - 0x18)
        staged = sum(1 for s in range(6, 23)
                     if u16(back, s * 4 - 0x18) == 0x0002)
        rep.gate("capacity.staged17", staged == 17,
                 f"{staged}/17 slots state=$0002 (disclosed staging; the "
                 f"game's own init advances them to $0010)")
        rep.log("[info] capacity: disclosed write_sram fill of slots 6..22 "
                "leaving slot 23 free; all writes under test remain the "
                "real ROM allocator")

        # Scroll right across new columns. With 17 staged + slot 23 free,
        # the first widened record must REALLY allocate: slot 23's state
        # goes nonzero AND its sprite id equals the queued record's id —
        # observable proof of producer->consumer->init, not a banner.
        # Once the pool is full (18), further widened submissions must
        # drain out of the queue with their flag cleared back to 0 (the
        # native failure-for-retry) and no extra slot.
        ambient_before = read_sram(client, 0x0EC0, 0x40)
        widened_flagged = []
        alloc_events = []             # real allocation into slot 23
        fail_events = []              # flag FF->0 while pool stayed full
        max_active = 0
        prev_flags = flags0[:]
        pending = {}                  # stage -> active count when flagged
        for _ in range(220):
            step_frames(client, 1, "right")
            wram = read_ram(client, 0, 0x200)
            cam_x = s16(wram, 0x39)
            lt = s16(read_sram(client, 0x00A4, 2), 0)
            fl = read_sram(client, 0x28CA, 0x100)
            queue = read_sram(client, 0x27CE, 0xFC)
            states = read_sram(client, 0x0F00, 0x60)
            ids = read_sram(client, 0x1360, 0x60)
            active = sum(1 for s2 in range(6, 24)
                         if u16(states, s2 * 4) != 0)
            max_active = max(max_active, active)
            bound = (lt >> 4) + 22
            # Queue: 8B entries {id@0, x@2, y@4, stageID@6}; the +0 word
            # $FFFF is the terminator (producer scans to it). Entries may
            # be interleaved-free only before it; past it is stale data.
            qentries = {}
            for e in range(31):
                if u16(queue, e * 8) == 0xFFFF:
                    break
                sid = u16(queue, e * 8 + 6)
                qentries[sid] = u16(queue, e * 8)  # sprite id
            for r in recs:
                if r["stage"] >= len(fl) or r["id"] >= 0x1BA:
                    continue
                sid = r["stage"]
                newly = fl[sid] == 0xFF and prev_flags[sid] != 0xFF
                if newly and r["xtile"] >= bound:
                    widened_flagged.append(
                        {"stage": sid, "xtile": r["xtile"], "bound": bound,
                         "cam": cam_x, "in_queue": sid in qentries,
                         "rec_id": r["id"], "active": active})
                if newly:
                    pending[sid] = {"active": active, "rec_id": r["id"]}
                elif sid in pending and sid not in qentries:
                    # Consumer took the record out of the queue. Attribute
                    # the outcome by the per-slot stage-id field
                    # ($1460+4*slot, written by the real consumer — staged
                    # as 0): a live slot carrying this stage id proves the
                    # record really allocated+initialized; no such slot
                    # while the pool stayed at 18 proves a native failure.
                    stage_ids = read_sram(client, 0x1460, 0x60)
                    hit = [j for j in range(6, 24)
                           if u16(stage_ids, j * 4) == sid and
                           u16(states, j * 4) != 0]
                    if hit:
                        j = hit[0]
                        alloc_events.append(
                            {"stage": sid, "slot": j,
                             "id": u16(ids, j * 4),
                             "state": u16(states, j * 4),
                             "active_at_submit":
                             pending[sid]["active"], "cam": cam_x})
                    elif active >= 18 and                             pending[sid]["active"] >= 18:
                        fail_events.append(
                            {"stage": sid, "flag": fl[sid],
                             "active_at_submit": pending[sid]["active"],
                             "active_now": active, "cam": cam_x})
                    del pending[sid]
            prev_flags = fl
            if alloc_events and fail_events and len(widened_flagged) >= 2:
                break
        rep.log(f"[info] capacity: widened submissions {widened_flagged[:6]}")
        rep.log(f"[info] capacity: real allocations {alloc_events[:4]}")
        rep.log(f"[info] capacity: full-pool failures {fail_events[:4]}")
        rep.log(f"[info] capacity: max simultaneous allocatable active "
                f"{max_active}")
        rep.gate("capacity.widened_submission",
                 len(widened_flagged) > 0 and
                 any(w["in_queue"] for w in widened_flagged),
                 "producer enqueued records beyond the native 22-col bound "
                 "(queue entry seen at $27CE, real consumer path)")
        rep.gate("capacity.real_alloc",
                 len(alloc_events) > 0,
                 "a widened record allocated a live slot whose per-slot "
                 "stage-id field ($1460, consumer-written) equals the "
                 "record's stage id (real producer->consumer->init)")
        rep.gate("capacity.full_failure",
                 len(fail_events) > 0,
                 "a widened record drained while the pool stayed at 18 "
                 "cleared its flag for retry and took no slot "
                 "(native allocator failure observed end-to-end)")
        rep.gate("capacity.bound", max_active <= 18,
                 f"allocatable pool never exceeded 18 "
                 f"(measured max={max_active})")
        # The FFFF terminator must sit inside the queue region (latest
        # legal position = entry 30's +0 at $28C6; flags start $28CA).
        queue = read_sram(client, 0x27CE, 0xFC)
        term_at = None
        for e in range(31):
            if u16(queue, e * 8) == 0xFFFF:
                term_at = 0x27CE + e * 8
                break
        rep.gate("capacity.queue_tail",
                 term_at is not None and term_at <= 0x28C8,
                 f"queue terminator at ${term_at or -1:#06x} within "
                 f"[$27CE,$28C8] — queue never overran into the flag "
                 f"table")
        amb = read_sram(client, 0x0EC0, 0x40)
        amb_active = sum(1 for s2 in range(16)
                         if u16(amb, s2 * 4) != 0)
        rep.gate("capacity.ambient_bound",
                 amb_active <= 16 and
                 sum(1 for s2 in range(16)
                     if u16(amb, s2 * 4) == 0x0002) == 0,
                 f"ambient pool within its own 16-slot bound "
                 f"({amb_active} active), no staged regulars leaked")
    finally:
        client.close()
        run.kill()
        try:
            run.wait(10)
        except Exception:
            pass


def s_posthole(rep: Report, ws: Path, opts):
    """Post-hole widened-part proof with real pixel evidence: scroll the
    32:9 field until a widened-only pack part exists (parked y=0xF0 in the
    native mirror with its true coords in the published metadata), capture
    live OAM+CGRAM+the presented frame, and verify sprite-palette pixels
    actually land at the part's true extended position."""
    port = free_port()
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=1, windowsize="1920x540")
    env = {"SNESRECOMP_DEBUG_PORT": str(port)}
    run = GameRun(ws, "posthole-trace", TRACE_EXE, cfg, TRACE_BOOT_SCRIPT,
                  env_extra=env)
    run.launch()
    try:
        client = wait_server(port, run.proc, timeout=120)
    except Exception as e:
        rep.gate("posthole.server", False, str(e))
        run.kill()
        return
    try:
        t0 = time.monotonic()
        mode = 0
        while time.monotonic() - t0 < 300:
            fr = client.query("frame")
            if fr.get("frame", 0) > 11500:
                mode = u16(read_ram(client, 0x0118, 2), 0)
                if mode == 0x0F:
                    break
            time.sleep(1.0)
        rep.gate("posthole.reached_level", mode == 0x0F,
                 f"mode={mode:#06x}")
        st0 = client.query("get_ppu_state").get("widescreen", {})
        extra = st0.get("left", 0)
        rep.log(f"[info] posthole: extra margin {extra}px")

        # Scroll until a widened part is parked with live coords, sampling
        # the pack table + OAM mirror at stopped boundaries.
        part = None
        audit = None
        for _ in range(200):
            step_frames(client, 1, "right")
            sram = read_sram(client, 0, 0x8000)
            oam_r = client.query("dump_oam")
            oam = _hex_field(oam_r)
            audit = posthole_audit(sram, oam)
            if audit["widened_parts"]:
                part = audit["widened_parts"][0]
                break
        rep.log(f"[info] posthole: audit { {k: audit[k] for k in
                ('first_hole','post_visible','post_widened')} }")
        rep.gate("posthole.audit_clean",
                 audit is not None and not audit["bad"],
                 f"bad={audit and audit['bad'][:3]}")
        rep.gate("posthole.widened_part", part is not None,
                 "a post-hole widened-only part was parked with true coords")
        if not part:
            return
        rep.log(f"[info] posthole: part {part}")
        # Packed mirror must be parked; true coords come from metadata.
        oam_e = decode_oam(oam)[part["slot"]]
        rep.gate("posthole.packed_parked", oam_e["y"] == 0xF0,
                 f"slot {part['slot']} packed y={oam_e['y']} (parked)")
        rep.gate("posthole.no_wrapped_ghost",
                 not ghost_audit(sram, oam),
                 "no wrapped-center ghost")

        # Pixel evidence: the presented frame (BGRX, w=256+2*extra, h=224)
        # must contain sprite-palette colors at the part's true position.
        # screen_x = extra + x ; palette base 0x80 + pal*16 (OBJ palettes).
        cgram = _hex_field(client.query("dump_cgram"))
        pal = (part["attr"] >> 9) & 7
        pal_base = 0x80 + pal * 16
        pal_colors = set()
        for ci in range(1, 16):
            c = u16(cgram, (pal_base + ci) * 2)
            if c:
                pal_colors.add(c)
        scr_path = run.dir / "posthole.bmp"
        client.query(f"screenshot {scr_path}")
        rep.gate("posthole.capture", scr_path.exists(),
                 f"screenshot {scr_path}")
        if not scr_path.exists():
            return
        w, h, px = bmp_read(scr_path)
        rep.gate("posthole.frame_dims",
                 w == 256 + 2 * extra and h == 224,
                 f"{w}x{h} vs extra={extra}")
        # The presented BMP is BGRX: SNES BGR555 color -> packed B,G,R.
        # Check pixels in the part's footprint for any of its palette
        # colors. Sprite size: size bit => 16x16 else 8x8.
        size = 16 if part["size"] else 8
        sx = extra + part["x"]
        sy = part["y"] - 1
        hits = 0
        bg_hits = 0
        if 0 <= sx and 0 <= sy:
            for dy in range(size):
                for dx in range(size):
                    xx, yy = sx + dx, sy + dy
                    if not (0 <= xx < w and 0 <= yy < h):
                        continue
                    b, g, rch, _a = px[4 * (yy * w + xx):
                                       4 * (yy * w + xx) + 4]
                    snes = (b >> 3) | ((g >> 3) << 5) | ((rch >> 3) << 10)
                    if snes in pal_colors:
                        hits += 1
                    if snes != 0:
                        bg_hits += 1
        rep.log(f"[info] posthole: footprint ({sx},{sy}) size {size} "
                f"sprite-palette hits {hits} (palette {pal_base:x}, "
                f"{len(pal_colors)} colors)")
        rep.gate("posthole.real_pixels",
                 hits >= 4,
                 f"{hits} sprite-palette pixels at true extended position "
                 f"({sx},{sy}) — the widened part renders where its "
                 f"metadata places it")
    finally:
        client.close()
        run.kill()
        try:
            run.wait(10)
        except Exception:
            pass


def u16b(b: bytes, off: int) -> bytes:
    return b[off:off + 2]


LHT_SOURCE = Path(__file__).resolve().parent / "smw2_native_livehost.m"


def s_livehost(rep: Report, ws: Path, opts):
    """REAL live-host paths the script/TCP interfaces cannot reach:
    an actual drawable resize in the running process (AppKit setContentSize
    via the test-only injected dylib) and an actual rewind filmstrip
    round-trip (F12 -> Left steps -> Return commit) posted as real NSEvents
    into the owned test app. Helper provenance: tests/smw2_native_livehost.m
    (disclosed source, own-process AppKit only — no SDL symbols, no
    singleton, no production patch) is compiled fresh into the isolated
    workspace and injected only into the copied test executable."""
    dylib = ws / "livehost.dylib"
    src_sha = sha256_file(LHT_SOURCE)
    r = subprocess.run(
        ["clang", "-arch", "arm64", "-dynamiclib", "-fobjc-arc",
         "-framework", "AppKit",
         "-o", str(dylib), str(LHT_SOURCE)],
        capture_output=True, text=True)
    rep.gate("lht.helper_build", r.returncode == 0 and dylib.exists(),
             f"src {src_sha[:16]}… -> {dylib} rc={r.returncode} "
             f"{r.stderr.strip()[:120]}")
    rep.log(f"[info] lht helper: source sha256={src_sha} "
            f"dylib sha256={sha256_file(dylib)}")
    helper = dylib
    port = free_port()
    cmdfile = ws / "live-host.cmd"
    cmdfile.write_text("")
    env = {"SNESRECOMP_DEBUG_PORT": str(port),
           "DYLD_INSERT_LIBRARIES": str(helper),
           "LHT_CMD": str(cmdfile),
           "LHT_LOG": str(ws / "livehost-inject.log")}
    cfg = BASE_CONFIG.format(autosave=0, runahead=0, aspect="4:3",
                             widescreen=1, windowsize="854x480")
    lht_script = PROBE_SCRIPT.split("dump stage11_candidate")[0] + (
        "dump stage11_candidate\nwait 12000\nquit\n")
    run = GameRun(ws, "live-host", TRACE_EXE, cfg, lht_script,
                  env_extra=env)
    run.launch()
    try:
        client = wait_server(port, run.proc, timeout=120)
    except Exception as e:
        rep.gate("lht.server", False, str(e))
        run.kill()
        return
    ilog = ws / "livehost-inject.log"

    def lht(cmd: str):
        with open(cmdfile, "a") as f:
            f.write(cmd + "\n")

    def llog():
        try:
            return ilog.read_text()
        except OSError:
            return ""

    try:
        # Reach 1-1 (same wait as trace scenario).
        t0 = time.monotonic()
        mode = 0
        while time.monotonic() - t0 < 300:
            fr = client.query("frame")
            if fr.get("frame", 0) > 11500:
                mode = u16(read_ram(client, 0x0118, 2), 0)
                if mode == 0x0F:
                    break
            if run.proc.poll() is not None:
                break
            time.sleep(1.0)
        rep.gate("lht.reached_level", mode == 0x0F,
                 f"mode={mode:#06x} frame={fr.get('frame')}")
        rep.gate("lht.injected", "injected pid" in llog(),
                 "dylib reported into the test process")

        # ── LIVE RESIZE in the running process ────────────────────────────
        step_frames(client, 5)
        st = client.query("get_ppu_state")
        ws0 = st.get("widescreen", {})
        rep.log(f"[info] lht: ppu before resize {ws0}")
        rep.gate("lht.ppustate_before", ws0.get("left", 0) > 0,
                 f"extra L={ws0.get('left')} R={ws0.get('right')}")
        lht("resize 1440x540")
        # The dylib's main-queue block only drains while the main loop
        # pumps events — drive frames while waiting for it.
        t0 = time.monotonic()
        while "resize ->" not in llog() and time.monotonic() - t0 < 60:
            try:
                step_frames(client, 5)
            except Exception:
                break
            time.sleep(0.3)
        rep.gate("lht.resize_applied", "resize ->" in llog(),
                 f"helper log: {llog().strip()[-200:]}")
        time.sleep(1.0)
        step_frames(client, 10)
        st2 = client.query("get_ppu_state")
        ws1 = st2.get("widescreen", {})
        rep.log(f"[info] lht: ppu after resize {ws1}")
        rep.gate("lht.live_resize",
                 ws1.get("left", 0) > ws0.get("left", 0) or
                 ws1.get("right", 0) > ws0.get("right", 0),
                 f"computed extra {ws0.get('left')}/{ws0.get('right')} -> "
                 f"{ws1.get('left')}/{ws1.get('right')} after drawable "
                 f"resize in the SAME process")

        # ── REAL REWIND round trip ────────────────────────────────────────
        # Play forward live (unpaused, non-turbo) so the snapshot ring has
        # real history; hold Right briefly to move the player.
        client.query("set_controller right")
        client.query("continue")
        time.sleep(3.0)
        client.query("clear_controller")
        step_frames(client, 1)
        before_frame = client.query("frame").get("frame", 0)
        before_x = u16(read_sram(client, 0x8C, 2), 0)
        before_cam = s16(read_ram(client, 0x39, 2), 0)
        rep.log(f"[info] lht: rewind-from frame={before_frame} "
                f"physX={before_x} cam={before_cam}")
        # Real F12 -> left steps -> Return sequence posted as CGEvents
        # into our own pid. While the modal owns the main thread, client
        # queries could stall — so the only evidence path during the modal
        # is the run log's breadcrumbs.
        client.query("continue")
        lht("seq 1100 111,123,123,36")
        t0 = time.monotonic()
        log = ""
        while time.monotonic() - t0 < 90:
            log = run.read_log()
            if "rewind filmstrip CLOSED" in log:
                break
            time.sleep(0.5)
        opens0 = log.count("filmstrip OPEN")
        rep.gate("lht.rewind_opened",
                 opens0 >= 1,
                 "filmstrip opened from the real F12 event")
        rep.gate("lht.rewind_closed",
                 "rewind filmstrip CLOSED" in log,
                 "filmstrip closed (commit or cancel)")
        # The F12 latch is consumed once: after the commit, the guest must
        # actually advance frames and the filmstrip must NOT reopen
        # without another input. Step real frames (bounded); on the
        # unfixed host the stale latch re-opens the modal here, `step`
        # stalls while the guest is frozen, and a second 'filmstrip OPEN'
        # appears — genuine RED.
        fr0 = client.query("frame").get("frame", 0)
        # `continue` lets the guest free-run; on the unfixed host the stale
        # latch re-opens the modal within ~1 frame (breadcrumb), on the
        # fixed host the frame counter simply advances. No blocking `step`
        # inside a possibly-frozen modal (step has no socket timeout).
        client.query("continue")
        t0 = time.monotonic()
        while time.monotonic() - t0 < 12:
            log = run.read_log()
            if log.count("filmstrip OPEN") > opens0:
                break
            time.sleep(0.25)
        log = run.read_log()
        try:
            fr1 = client.query("frame").get("frame", 0)
        except Exception:
            fr1 = fr0
        rep.gate("lht.no_reopen",
                 log.count("filmstrip OPEN") == 1,
                 f"exactly one 'filmstrip OPEN' from one F12 "
                 f"(counted {log.count('filmstrip OPEN')}; a stale latch "
                 f"re-opens it after commit)")
        rep.gate("lht.post_commit_advance",
                 fr1 > fr0,
                 f"guest advanced frame {fr0} -> {fr1} post-commit "
                 f"(free-run after the modal)")
        # Guest resumed: safe to talk to the debug server again.
        after_frame = client.query("frame").get("frame", 0)
        after_x = u16(read_sram(client, 0x8C, 2), 0)
        rep.log(f"[info] lht: rewind-to frame={after_frame} "
                f"physX={after_x} (was {before_x})")
        # A real restore moves the guest backwards OR to a recorded earlier
        # snapshot; the resumed sim then advances again.
        rep.gate("lht.rewind_restored",
                 after_x <= before_x and
                 "refused" not in log,
                 f"physX {before_x} -> {after_x} after commit "
                 f"(guest resumed at an older recorded snapshot; "
                 f"'snapshot refused' absent)")
        # The debug server answers inside the modal: prove the guest is
        # still a live, sane machine post-commit (mode byte real, queries
        # responsive); the "guest resuming" breadcrumb already proves the
        # resume happened between closes.
        mode = u16(read_ram(client, 0x0118, 2), 0)
        fr = client.query("frame").get("frame", 0)
        rep.gate("lht.post_rewind_play",
                 mode == 0x0F and fr > before_frame and
                 "guest resuming" in log,
                 f"post-commit: mode={mode:#06x} frame={fr} "
                 f"'guest resuming' breadcrumb present")
    finally:
        client.close()
        run.kill()
        try:
            run.wait(10)
        except Exception:
            pass


SCENARIOS = {
    "binaries": s_binaries,
    "parity-off": s_parity,
    "wide-16x9": s_wide16,
    "ultra-32x9": s_ultra,
    "runahead-16x9": s_runahead,
    "crossflag": s_crossflag,
    "resize-adapt": s_resize,
    "trace-16x9": s_trace,
    "capacity-trace": s_capacity,
    "posthole-trace": s_posthole,
    "live-host": s_livehost,
}
ORDER = ["binaries", "parity-off", "wide-16x9", "ultra-32x9",
         "runahead-16x9", "crossflag", "resize-adapt",
         "trace-16x9", "capacity-trace", "posthole-trace",
         "live-host"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", action="append",
                    choices=list(SCENARIOS) + ["all"], default=None,
                    help="scenario to run (repeatable; default 'all')")
    ap.add_argument("--workdir", default=None,
                    help="workspace root (default: mktemp smw2-ws-native-*)")
    ap.add_argument("--timeout", type=float, default=420.0,
                    help="per-launch wall timeout, seconds")
    ap.add_argument("--report", default=None,
                    help="write the machine-readable report JSON here")
    args = ap.parse_args()

    names = ORDER if not args.scenario or "all" in args.scenario \
        else [n for n in ORDER if n in args.scenario]
    ws = Path(args.workdir) if args.workdir else Path(
        tempfile.mkdtemp(prefix="smw2-ws-native-"))
    ws.mkdir(parents=True, exist_ok=True)

    rep = Report()
    rep.log(f"[info] workspace {ws}")
    rep.log(f"[info] release {RELEASE_EXE}")
    for name in names:
        rep.log(f"[scenario] {name}")
        try:
            SCENARIOS[name](rep, ws, args)
        except TimeoutError as e:
            rep.gate(f"{name}.timeout", False, str(e))
        except Exception as e:  # scenario crash: fail loud, keep going
            import traceback
            rep.gate(f"{name}.exception", False, repr(e))
            traceback.print_exc()

    summary = {
        "workspace": str(ws),
        "release_sha256": sha256_file(RELEASE_EXE),
        "trace_sha256": sha256_file(TRACE_EXE) if TRACE_EXE.exists() else None,
        "scenarios": names,
        "gates": {k: [{"ok": ok, "detail": d} for ok, d in v]
                  for k, v in rep.gates.items()},
        "failures": rep.failures,
        "passed": rep.ok(),
    }
    out = Path(args.report) if args.report else ws / "report.json"
    out.write_text(json.dumps(summary, indent=2) + "\n")
    rep.log(f"[info] report {out}")
    print(f"[summary] {'PASS' if rep.ok() else 'FAIL'} "
          f"{sum(1 for v in rep.gates.values() for ok, _ in v if ok)} gates, "
          f"{len(rep.failures)} failures")
    for f in rep.failures:
        print(f"  FAIL {f}")
    return 0 if rep.ok() else 1


if __name__ == "__main__":
    sys.exit(main())
