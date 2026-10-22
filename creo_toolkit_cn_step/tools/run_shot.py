# -*- coding: utf-8 -*-
"""
run_shot.py - 启动一个已部署的 Creo，等主窗口出来后抓屏，再结束进程。

必须在一次调用里完成启动->操作->截图：这个环境里由工具调用派生的
GUI 进程在该调用结束后会被回收，跨调用就找不到窗口了。

用法:
    python run_shot.py <安装目录名> <输出.png> [最长等待秒] [--click X Y]

例:
    python run_shot.py "8.0.12.0" build/shot_creo8.png 240 --click 190 45
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

user32 = ctypes.windll.user32


class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]


def find_window(keywords):
    hits = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        if user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n > 0:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                t = buf.value
                if any(k.lower() in t.lower() for k in keywords):
                    hits.append((hwnd, t))
        return True

    user32.EnumWindows(cb, 0)
    return hits[0] if hits else None


def wait_for_window(keywords, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        hit = find_window(keywords)
        if hit:
            return hit
        time.sleep(3)
    return None


def kill_creo():
    subprocess.run(["taskkill", "/f", "/im", "xtop.exe"], capture_output=True)
    subprocess.run(["taskkill", "/f", "/im", "parametric.exe"], capture_output=True)


def all_windows():
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        if user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            if n > 0:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                out.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return out



class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


def client_origin(hwnd):
    """窗口图片坐标 -> 客户区坐标的偏移。
    PrintWindow 抓到的是含边框的整窗，而鼠标消息要的是客户区坐标。"""
    wr, cr = RECT(), RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(wr))
    user32.GetClientRect(hwnd, ctypes.byref(cr))
    ww, wh = wr.right - wr.left, wr.bottom - wr.top
    cw, ch = cr.right - cr.left, cr.bottom - cr.top
    bx = (ww - cw) // 2
    by = wh - ch - bx
    return bx, by


def click_window(hwnd, x, y):
    """按「窗口图片坐标」点一下。

    不用 SendInput：Creo 主窗口对注入输入不买账（mouse_event 和
    SendInput 绝对坐标都试过）。改成把消息直接投给目标子窗口：先算出
    屏幕坐标，再逐层 ChildWindowFromPointEx 找到最深的子窗口，
    最后 PostMessage WM_LBUTTONDOWN/UP。这条路径不依赖前台窗口和 z 序。
    """
    bx, by = client_origin(hwnd)
    sp = POINT(x - bx, y - by)
    user32.ClientToScreen(hwnd, ctypes.byref(sp))
    print("click window(%d,%d) -> screen(%d,%d)" % (x, y, sp.x, sp.y))

    target = hwnd
    for _ in range(16):
        org = POINT(0, 0)
        user32.ClientToScreen(target, ctypes.byref(org))
        loc = POINT(sp.x - org.x, sp.y - org.y)
        user32.ChildWindowFromPointEx.restype = wt.HWND
        user32.ChildWindowFromPointEx.argtypes = [wt.HWND, POINT, wt.UINT]
        nxt = user32.ChildWindowFromPointEx(target, loc, 0x0003)  # 含可见/禁用
        if not nxt or nxt == target:
            break
        target = nxt

    org = POINT(0, 0)
    user32.ClientToScreen(target, ctypes.byref(org))
    loc = POINT(sp.x - org.x, sp.y - org.y)
    lp = ((loc.y & 0xFFFF) << 16) | (loc.x & 0xFFFF)
    print("  target hwnd %d client(%d,%d)" % (target, loc.x, loc.y))

    for kind, flag in ((0x0200, 0), (0x0201, 1), (0x0202, 0)):
        user32.PostMessageW(target, kind, flag, lp)
        time.sleep(0.12)


def main():
    argv = list(sys.argv[1:])
    clicks = []
    while "--click" in argv:
        i = argv.index("--click")
        clicks.append((int(argv[i + 1]), int(argv[i + 2])))
        del argv[i:i + 3]

    part = None
    if "--partfile" in argv:
        i = argv.index("--partfile")
        part = open(argv[i + 1], encoding="utf-8").read().strip()
        del argv[i:i + 2]

    args = argv
    install = args[0]
    out = args[1]
    timeout = int(args[2]) if len(args) > 2 else 240

    root = r"C:\Program Files\PTC\Creo " + install
    exe = os.path.join(root, "Parametric", "bin", "parametric.exe")
    if not os.path.exists(exe):
        raise SystemExit("找不到 " + exe)

    log = os.path.join(root, "cc_luoluoo", "cn_step_load.log")
    if os.path.exists(log):
        try:
            os.rename(log, log + ".prev")
        except OSError:
            pass

    work = os.path.join(os.environ["TEMP"], "luoluo_shot_" + install.replace(".", ""))
    os.makedirs(work, exist_ok=True)
    for f in os.listdir(work):
        try:
            os.rename(os.path.join(work, f), os.path.join(work, f) + ".prev")
        except OSError:
            pass

    print("launch:", exe)
    cmd = [exe]
    if part:
        print("open model:", part)
        cmd.append(part)
    subprocess.Popen(cmd, cwd=work)

    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(log) and os.path.getmtime(log) > t0:
            break
        time.sleep(2)
    if os.path.exists(log) and os.path.getmtime(log) > t0 - 5:
        print("--- plugin log ---")
        print(open(log, encoding="utf-8", errors="replace").read().strip())

    hit = wait_for_window(["Creo Parametric"], 240)
    if not hit:
        print("WINDOW NOT FOUND")
        kill_creo()
        sys.exit(1)
    hwnd, title = hit
    print("window:", title)

    # 功能区要等 UI 全部画完；带模型启动时还要等模型载入
    if part:
        base = os.path.basename(part)
        dl = time.time() + 300
        while time.time() < dl:
            h2 = find_window(["Creo Parametric"])
            if h2 and base in h2[1]:
                break
            time.sleep(3)
        time.sleep(25)
    else:
        time.sleep(18)

    if clicks:
        for cx, cy in clicks:
            click_window(hwnd, cx, cy)
            time.sleep(2.5)
        time.sleep(1.5)
        print("--- 可见窗口 ---")
        for h, t in all_windows():
            print("   %d  %s" % (h, t))
        dlg = find_window(["关于", "About"])
        if dlg:
            ok2, w2, h2 = None, 0, 0
            import tools.shot as s2
            ok2, w2, h2 = s2.capture(dlg[0], out.replace(".png", "_dlg.png"))
            print("dialog %r -> %s (%dx%d)" % (dlg[1], out.replace(".png", "_dlg.png"), w2, h2))

    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import tools.shot as s
    ok, w, h = s.capture(hwnd, out)
    print("screenshot %dx%d -> %s (PrintWindow ok=%s)" % (w, h, out, ok))

    kill_creo()


if __name__ == "__main__":
    main()
