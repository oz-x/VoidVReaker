# Foreground helpers that do not synthesise ALT (which can pop the Start menu).
import ctypes
from ctypes import wintypes
user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
user32.SetProcessDPIAware()

def game_window():
    """Top-level visible window of the game process."""
    import subprocess
    out = subprocess.run(["tasklist", "/fi", "imagename eq VOIDBREAKER-Win64-Shipping.exe", "/fo", "csv", "/nh"],
                         capture_output=True, text=True).stdout
    pids = {int(line.split('","')[1]) for line in out.splitlines() if '","' in line}
    found = []
    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(cb, 0)
    return found[0] if found else None

def focus(hwnd):
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return fg
    me = kernel32.GetCurrentThreadId()
    other = user32.GetWindowThreadProcessId(fg, None)
    user32.AttachThreadInput(me, other, True)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(me, other, False)
    return fg

if __name__ == "__main__":
    h = game_window()
    print("window", h)
    if h:
        buf = ctypes.create_unicode_buffer(256); user32.GetWindowTextW(h, buf, 256); print(buf.value)
        focus(h)
