# -*- coding: utf-8 -*-
"""独立验证「关于落落」窗口：不经过 Creo，直接 ctypes 调用 DLL 里的
cn_about_test()，抓图后关掉，确认不闪退。"""

import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

LOADER = r'''
import ctypes, os, sys
# Python 3.8 起不再用 PATH 找依赖 DLL，必须显式 add_dll_directory
for _d in os.environ.get("CREO_DIRS", "").split(";"):
    if _d:
        try:
            os.add_dll_directory(_d)
        except OSError:
            pass
dll = ctypes.WinDLL(sys.argv[1])
rc = dll.cn_about_test()
print("cn_about_test rc=", rc, flush=True)
'''


def find_window(title_part):
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(h, l):
        if user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            if n > 0:
                b = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(h, b, n + 1)
                if title_part in b.value:
                    found.append((h, b.value))
        return True

    user32.EnumWindows(cb, 0)
    return found


def capture(hwnd, path):
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return None

    hdc = user32.GetDC(hwnd)
    mdc = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    gdi32.SelectObject(mdc, bmp)
    # PW_RENDERFULLCONTENT = 2, 对被遮挡/后台窗口也能抓到内容
    user32.PrintWindow(hwnd, mdc, 2)

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG),
                    ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD),
                    ("biSizeImage", wt.DWORD), ("biXPelsPerMeter", wt.LONG),
                    ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]

    bi = BITMAPINFOHEADER()
    bi.biSize = ctypes.sizeof(bi)
    bi.biWidth = w
    bi.biHeight = -h
    bi.biPlanes = 1
    bi.biBitCount = 32
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bi), 0)

    from PIL import Image
    im = Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1)
    im.convert("RGB").save(path)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(hwnd, hdc)
    return (w, h)


def creo_dirs():
    """我们的 DLL 静态链接了 PTC 的 protk 库，运行时要能找到 Creo 的 DLL，
    单独 ctypes 加载前必须把 Creo 的目录加进 PATH。"""
    import glob
    dirs = []
    for root in glob.glob(r"C://Program Files//PTC//Creo *"):
        for sub in ("Parametric\\bin", r"Common Files\x86e_win64\obj",
                    r"Common Files\x86e_win64",
                    r"Common Files\proe\x86e_win64\obj"):
            d = os.path.join(root, sub)
            if os.path.isdir(d):
                dirs.append(d)
    return dirs


def main():
    dll = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "build/cn_step_c8.dll")
    out = sys.argv[2] if len(sys.argv) > 2 else "build/about.png"
    print("dll:", dll)

    env = os.environ.copy()
    env["PATH"] = ";".join(creo_dirs()) + ";" + env.get("PATH", "")
    env["CREO_DIRS"] = ";".join(creo_dirs())

    p = subprocess.Popen([sys.executable, "-c", LOADER, dll],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         env=env)
    hwnd = None
    t0 = time.time()
    while time.time() - t0 < 15:
        hits = find_window("关于落落")
        if hits:
            hwnd = hits[0][0]
            break
        if p.poll() is not None:
            break
        time.sleep(0.3)

    if not hwnd:
        print("NO WINDOW FOUND")
        try:
            print("loader output:", p.communicate(timeout=5)[0].decode("utf-8", "replace"))
        except Exception:
            p.kill()
        return 1

    print("window ok, hwnd =", hwnd)
    time.sleep(0.8)
    size = capture(hwnd, out)
    print("captured", size, "->", out)

    user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    try:
        o = p.communicate(timeout=8)[0].decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        print("LOADER HUNG after close")
        p.kill()
        return 1
    print("loader exit:", p.returncode, "|", o.strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())
