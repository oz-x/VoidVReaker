# Briefly focuses the game, sends real key presses (SendInput), then restores the previous window.
import ctypes, sys, time
from ctypes import wintypes
from fg import focus, game_window
user32 = ctypes.windll.user32
VK = {"RETURN": 0x0D, "ESCAPE": 0x1B, "UP": 0x26, "DOWN": 0x28, "LEFT": 0x25, "RIGHT": 0x27,
      "SPACE": 0x20, "W": 0x57, "A": 0x41, "S": 0x53, "D": 0x44, "E": 0x45, "TAB": 0x09, "F6": 0x75, "F7": 0x76,
      "OEM3": 0xC0}
class KI(ctypes.Structure):
    _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]
class INPUT(ctypes.Structure):
    _fields_ = [("type", wintypes.DWORD), ("ki", KI), ("pad", ctypes.c_byte * 8)]
def key(vk, up):
    scan = user32.MapVirtualKeyW(vk, 0)
    flags = 0x0008 | (0x0002 if up else 0)  # KEYEVENTF_SCANCODE
    if vk in (0x25, 0x26, 0x27, 0x28): flags |= 0x0001  # extended
    inp = INPUT(1, KI(0, scan, flags, 0, 0))
    user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(INPUT))
hwnd = game_window()
if not hwnd:
    print("window not found"); sys.exit(1)
prev = user32.GetForegroundWindow()
focus(hwnd)
time.sleep(0.4)
for name in sys.argv[1:]:
    hold = 0.1
    if ":" in name:
        name, hold = name.split(":"); hold = float(hold)
    vk = VK.get(name.upper()) or int(name, 16)
    key(vk, False); time.sleep(hold); key(vk, True); time.sleep(0.7)
if prev and prev != hwnd:
    focus(prev)
print("sent", sys.argv[1:])
