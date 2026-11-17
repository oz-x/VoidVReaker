# Maps offsets inside build/Release/version.dll to function+line using its PDB (DbgHelp).
import ctypes, sys
from ctypes import wintypes as wt
dbg = ctypes.WinDLL("dbghelp")
dbg.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbg.SymLoadModuleExW.restype = ctypes.c_uint64
dbg.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", wt.ULONG),
                ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG), ("Tag", wt.ULONG),
                ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", wt.WCHAR * 512)]
class LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD), ("FileName", ctypes.c_char_p), ("Address", ctypes.c_uint64)]
h = wt.HANDLE(0x1234)
dbg.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES
dbg.SymInitializeW(h, None, False)
base = 0x10000000
dll = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith(".dll") else r"C:\Users\oz\Desktop\VoidVReaker\build\Release\version.dll"
args = [a for a in sys.argv[1:] if not a.endswith(".dll")]
dbg.SymLoadModuleExW(h, None, dll, None, base, 0, None, 0)
for a in args:
    off = int(a, 16)
    s = SYMBOL_INFOW(); s.SizeOfStruct = 88; s.MaxNameLen = 512
    disp = ctypes.c_uint64()
    ok = dbg.SymFromAddrW(h, ctypes.c_uint64(base + off), ctypes.byref(disp), ctypes.byref(s))
    line = LINE64(); line.SizeOfStruct = ctypes.sizeof(LINE64); d2 = wt.DWORD()
    lok = dbg.SymGetLineFromAddr64(h, ctypes.c_uint64(base + off), ctypes.byref(d2), ctypes.byref(line))
    print(f"+{off:X}: {s.Name if ok else '?'}+0x{disp.value:X}  {line.FileName.decode() if lok else ''}:{line.LineNumber if lok else ''}")
