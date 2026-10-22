# -*- coding: utf-8 -*-
"""
shot.py - 抓取指定标题的窗口（默认 Creo 主窗口）存成 PNG。

不要求窗口在前台：用 PrintWindow + PW_RENDERFULLCONTENT，
DirectX 画的内容（Creo 功能区就是 DX 画的）也能抓到。

用法: python shot.py <输出.png> [标题关键字]
"""
import ctypes
import ctypes.wintypes as wt
import sys

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

PW_RENDERFULLCONTENT = 0x00000002


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def find_windows(keyword):
    hits = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        if keyword.lower() in buf.value.lower():
            hits.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return hits


def capture(hwnd, path):
    r = RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        raise SystemExit("窗口尺寸异常")

    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, h)
    old = gdi32.SelectObject(mem, bmp)
    ok = user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
    gdi32.SelectObject(mem, old)

    class BMPINFOHEADER(ctypes.Structure):
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
                    ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
                    ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
                    ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
                    ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
                    ("biClrImportant", ctypes.c_uint32)]

    bi = BMPINFOHEADER()
    bi.biSize = ctypes.sizeof(BMPINFOHEADER)
    bi.biWidth = w
    bi.biHeight = -h          # 负数 = 自上而下
    bi.biPlanes = 1
    bi.biBitCount = 32
    bi.biCompression = 0      # BI_RGB
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bi), 0)

    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mem)
    user32.ReleaseDC(hwnd, hdc)

    from PIL import Image
    img = Image.frombuffer("RGBA", (w, h), buf.raw, "raw", "BGRA", 0, 1)
    img = img.convert("RGB")
    img.save(path)
    return ok, w, h


def main():
    path = sys.argv[1]
    keyword = sys.argv[2] if len(sys.argv) > 2 else "Creo"
    hits = find_windows(keyword)
    if not hits:
        raise SystemExit("没有找到标题包含 %r 的可见窗口" % keyword)
    hwnd, title = hits[0]
    print("capture:", title, "hwnd=0x%X" % hwnd)
    ok, w, h = capture(hwnd, path)
    print("PrintWindow ok=%s  %dx%d  -> %s" % (ok, w, h, path))


if __name__ == "__main__":
    main()
