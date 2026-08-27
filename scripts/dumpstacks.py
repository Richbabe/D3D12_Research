"""Minimal x64 minidump reader: reports per-thread RIP and a scanned pseudo callstack.

Written because no CLI debugger (cdb/windbg) is installed on this machine. Symbolization
goes through dbghelp so locally-present PDBs get resolved; modules without symbols fall
back to module+offset.
"""

import ctypes
import ctypes.wintypes as wt
import struct
import sys
from bisect import bisect_right

DUMP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\haydenhong\Documents\D3D12.dmp"

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_EXCEPTION = 6
STREAM_SYSTEM_INFO = 7
STREAM_MEMORY64_LIST = 9

CTX_RSP = 0x98
CTX_RBP = 0xA0
CTX_RIP = 0xF8


class Dump:
    def __init__(self, path):
        self.f = open(path, "rb")
        sig, ver, nstreams, dir_rva, _chk, _ts, _flags = struct.unpack(
            "<IIIIIIQ", self._at(0, 32)
        )
        if sig != 0x504D444D:
            raise SystemExit("not a minidump (bad signature)")
        self.streams = {}
        for i in range(nstreams):
            st, size, rva = struct.unpack("<III", self._at(dir_rva + i * 12, 12))
            self.streams[st] = (size, rva)

        self.modules = []          # (base, size, name)
        self.mod_bases = []
        self.ranges = []           # (start, size, file_offset)
        self.range_starts = []
        self.threads = []

        self._read_memory64()
        self._read_memory_list()
        self._read_modules()
        self._read_threads()

    def _at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    # ---- streams -------------------------------------------------------
    def _read_memory64(self):
        if STREAM_MEMORY64_LIST not in self.streams:
            return
        _size, rva = self.streams[STREAM_MEMORY64_LIST]
        nranges, base_rva = struct.unpack("<QQ", self._at(rva, 16))
        blob = self._at(rva + 16, int(nranges) * 16)
        off = base_rva
        for i in range(int(nranges)):
            start, dsize = struct.unpack_from("<QQ", blob, i * 16)
            self.ranges.append((start, dsize, off))
            off += dsize

    def _read_memory_list(self):
        if STREAM_MEMORY_LIST not in self.streams:
            return
        _size, rva = self.streams[STREAM_MEMORY_LIST]
        (n,) = struct.unpack("<I", self._at(rva, 4))
        blob = self._at(rva + 4, n * 16)
        for i in range(n):
            start, dsize, drva = struct.unpack_from("<QII", blob, i * 16)
            self.ranges.append((start, dsize, drva))

    def _finish_ranges(self):
        self.ranges.sort(key=lambda r: r[0])
        self.range_starts = [r[0] for r in self.ranges]

    def _read_modules(self):
        _size, rva = self.streams[STREAM_MODULE_LIST]
        (n,) = struct.unpack("<I", self._at(rva, 4))
        blob = self._at(rva + 4, n * 108)
        for i in range(n):
            base, msize, _chk, _ts, name_rva = struct.unpack_from("<QIIII", blob, i * 108)
            (slen,) = struct.unpack("<I", self._at(name_rva, 4))
            name = self._at(name_rva + 4, slen).decode("utf-16-le", "replace")
            self.modules.append((base, msize, name))
        self.modules.sort(key=lambda m: m[0])
        self.mod_bases = [m[0] for m in self.modules]

    def _read_threads(self):
        _size, rva = self.streams[STREAM_THREAD_LIST]
        (n,) = struct.unpack("<I", self._at(rva, 4))
        blob = self._at(rva + 4, n * 48)
        for i in range(n):
            (tid, _susp, _pc, _pri, _teb,
             stk_start, stk_size, stk_rva,
             ctx_size, ctx_rva) = struct.unpack_from("<IIIIQQIIII", blob, i * 48)
            self.threads.append(dict(tid=tid, stack=(stk_start, stk_size, stk_rva),
                                     ctx=(ctx_size, ctx_rva)))

    # ---- lookups -------------------------------------------------------
    def module_of(self, addr):
        i = bisect_right(self.mod_bases, addr) - 1
        if i < 0:
            return None
        base, msize, name = self.modules[i]
        return self.modules[i] if base <= addr < base + msize else None

    def read(self, addr, n):
        i = bisect_right(self.range_starts, addr) - 1
        if i < 0:
            return None
        start, dsize, foff = self.ranges[i]
        if not (start <= addr < start + dsize):
            return None
        n = min(n, int(start + dsize - addr))
        return self._at(foff + (addr - start), n)

    def context(self, t):
        size, rva = t["ctx"]
        return self._at(rva, size)


# ---- symbolization via dbghelp -----------------------------------------
class Sym:
    def __init__(self, modules):
        self.dbg = ctypes.WinDLL("dbghelp.dll")
        self.h = ctypes.c_void_p(0x1234)
        self.dbg.SymSetOptions(0x00000002 | 0x00000004 | 0x00000010)  # UNDNAME|DEFERRED|LOAD_LINES
        search = "F:\\GitHub\\D3D12_Research\\Build\\D3D12_x64_Debug"
        if not self.dbg.SymInitializeW(self.h, ctypes.c_wchar_p(search), False):
            raise SystemExit("SymInitializeW failed")
        self.dbg.SymLoadModuleExW.restype = ctypes.c_uint64
        self.dbg.SymLoadModuleExW.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                              ctypes.c_wchar_p, ctypes.c_wchar_p,
                                              ctypes.c_uint64, wt.DWORD,
                                              ctypes.c_void_p, wt.DWORD]
        self.dbg.SymFromAddrW.argtypes = [ctypes.c_void_p, ctypes.c_uint64,
                                          ctypes.POINTER(ctypes.c_uint64), ctypes.c_void_p]
        self.loaded = 0
        for base, msize, name in modules:
            if self.dbg.SymLoadModuleExW(self.h, None, name, None, base, msize, None, 0):
                self.loaded += 1

    # SYMBOL_INFOW: SizeOfStruct=88, MaxNameLen at 80, Name[] at 84.
    def resolve(self, addr):
        buf = ctypes.create_string_buffer(88 + 4096)
        struct.pack_into("<I", buf, 0, 88)
        struct.pack_into("<I", buf, 80, 2000)
        disp = ctypes.c_uint64(0)
        if self.dbg.SymFromAddrW(self.h, ctypes.c_uint64(addr), ctypes.byref(disp), buf):
            name = ctypes.wstring_at(ctypes.addressof(buf) + 84)
            return name, disp.value
        return None, 0


def short(path):
    return path.rsplit("\\", 1)[-1]


def main():
    d = Dump(DUMP)
    d._finish_ranges()
    print(f"threads={len(d.threads)}  modules={len(d.modules)}  mem_ranges={len(d.ranges)}")

    interesting = [m for m in d.modules
                   if any(k in short(m[2]).lower()
                          for k in ("nvda", "warpviz", "nsight", "nomad", "nvwgf", "d3d12",
                                    "pix", "renderdoc", "dxgi", "nvapi", "ngfx"))]
    print("\n=== tool / graphics modules loaded ===")
    for base, msize, name in interesting:
        print(f"  {base:016x} {msize:>9} {name}")

    sym = Sym(d.modules)

    print("\n=== per-thread RIP ===")
    details = []
    for idx, t in enumerate(d.threads):
        ctx = d.context(t)
        if len(ctx) < CTX_RIP + 8:
            continue
        rip = struct.unpack_from("<Q", ctx, CTX_RIP)[0]
        rsp = struct.unpack_from("<Q", ctx, CTX_RSP)[0]
        m = d.module_of(rip)
        nm, off = sym.resolve(rip)
        label = nm if nm else (f"{short(m[2])}+0x{rip - m[0]:x}" if m else "?")
        print(f"  [{idx:3}] tid={t['tid']:<6} rip={rip:016x}  {label}")
        details.append((idx, t, rsp, label))

    print("\n=== scanned stacks for threads containing D3D12.exe frames ===")
    for idx, t, rsp, label in details:
        stk_start, stk_size, _rva = t["stack"]
        stk_end = stk_start + stk_size
        if not (stk_start <= rsp < stk_end):
            continue
        data = d.read(rsp, min(int(stk_end - rsp), 512 * 1024))
        if not data:
            continue
        frames = []
        seen = set()
        for o in range(0, len(data) - 8, 8):
            (v,) = struct.unpack_from("<Q", data, o)
            m = d.module_of(v)
            if not m or v in seen:
                continue
            seen.add(v)
            nm, off = sym.resolve(v)
            if nm:
                frames.append(f"{nm}+0x{off:x}")
            else:
                frames.append(f"{short(m[2])}+0x{v - m[0]:x}")
            if len(frames) > 60:
                break
        if not any(("D3D12.exe" in f or "::" in f or "WarpViz" in f or "NGFX" in f)
                   for f in frames):
            continue
        print(f"\n--- thread[{idx}] tid={t['tid']}  rip: {label}")
        for f in frames[:45]:
            print(f"      {f}")


main()
