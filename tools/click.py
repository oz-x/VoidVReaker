# Clicks at a position relative to the game window (fractions of its client area), then restores focus.
import ctypes, sys, time
from ctypes import wintypes
from fg import focus, game_window
user32 = ctypes.windll.user32
hwnd = game_window()
fx, fy = float(sys.argv[1]), float(sys.argv[2])
prev = user32.GetForegroundWindow()
old = wintypes.POINT(); user32.GetCursorPos(ctypes.byref(old))
focus(hwnd); time.sleep(0.4)
r = wintypes.RECT(); user32.GetClientRect(hwnd, ctypes.byref(r))
pt = wintypes.POINT(int(r.right * fx), int(r.bottom * fy)); user32.ClientToScreen(hwnd, ctypes.byref(pt))
user32.SetCursorPos(pt.x, pt.y); time.sleep(0.3)
user32.mouse_event(0x0002, 0, 0, 0, 0); time.sleep(0.1); user32.mouse_event(0x0004, 0, 0, 0, 0); time.sleep(0.5)
user32.SetCursorPos(old.x, old.y)
if prev and prev != hwnd:
    focus(prev)
print("clicked", pt.x, pt.y)
