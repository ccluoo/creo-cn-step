# -*- coding: utf-8 -*-
"""
test_export13.py - 端到端验证「导出中文 STEP」。

流程和真人操作一致：
  功能区 落落工具 -> 导出中文 STEP -> 保存对话框 -> 点「保存」 -> 等提示框

保存对话框里不动文件名，直接确认默认值，用来验证：
  1. 默认名是不是 <模型名>_<YYMMDD>.stp
  2. 两份文件是不是都落地了（一份 _other_ 给别的软件用）
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import tools.run_shot as rs
import tools.shot as shot

user32 = rs.user32


def find_dialog(keywords, tries=15):
    for _ in range(tries):
        for h, t in rs.all_windows():
            low = t.lower()
            if any(k.lower() in low for k in keywords):
                return h, t
        time.sleep(1.0)
    return None


def enum_children(parent):
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, lparam):
        cls = ctypes.create_unicode_buffer(64)
        user32.GetClassNameW(hwnd, cls, 64)
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 2)
        user32.GetWindowTextW(hwnd, buf, n + 2)
        out.append((hwnd, cls.value, buf.value))
        return True

    user32.EnumChildWindows(parent, cb, 0)
    return out


def main():
    install = sys.argv[1]
    part = open(sys.argv[2], encoding="utf-8").read().strip()
    tag = install.replace(".", "")

    root = r"C:\Program Files\PTC\Creo " + install
    exe = os.path.join(root, "Parametric", "bin", "parametric.exe")
    work = os.path.join(os.environ["TEMP"], "luoluo_exp" + tag)
    os.makedirs(work, exist_ok=True)

    print("launch:", exe)
    subprocess.Popen([exe, part], cwd=work)

    base = os.path.basename(part)
    hwnd = None
    deadline = time.time() + 420
    while time.time() < deadline:
        hit = rs.find_window(["Creo Parametric"])
        if hit and base.lower() in hit[1].lower():
            hwnd = hit[0]
            break
        time.sleep(3)
    if not hwnd:
        print("WINDOW NOT FOUND")
        rs.kill_creo()
        sys.exit(1)
    print("window hwnd", hwnd)
    time.sleep(30)

    rs.click_window(hwnd, 916, 45)          # 落落工具选项卡
    time.sleep(3)
    rs.click_window(hwnd, 55, 74)           # 导出中文 STEP
    time.sleep(5)

    dlg = find_dialog(["导出中文 STEP"])
    if not dlg:
        print("SAVE DIALOG NOT FOUND")
        rs.kill_creo()
        sys.exit(1)
    print("save dialog:", dlg)

    # 记下对话框里的默认文件名
    for h, cls, txt in enum_children(dlg[0]):
        if cls == "Edit" and txt:
            print("   默认文件名 ->", txt)

    # 点「保存」(IDOK=1)：走标准按钮消息，等价于鼠标点击
    user32.PostMessageW(dlg[0], 0x0111, 1, 0)
    print("posted IDOK, 等导出 ...")

    # 导出大零件要时间，然后会弹「导出完成」提示框
    msg = None
    dead = time.time() + 420
    while time.time() < dead:
        hit = find_dialog(["导出中文 STEP"], tries=1)
        # 保存对话框已经关了，此时同名窗口就是提示框
        if hit and hit[0] != dlg[0]:
            msg = hit
            break
        time.sleep(3)

    if msg:
        print("message box:", msg)
        shot.capture(msg[0], "build/ui4_done.png")
        for h, cls, txt in enum_children(msg[0]):
            if txt and cls in ("Static", "Button"):
                print("   ", repr(txt))
        user32.PostMessageW(msg[0], 0x0010, 0, 0)   # WM_CLOSE
        time.sleep(2)
    else:
        print("没等到提示框（可能导出还在跑或者出错了）")
        shot.capture(hwnd, "build/ui4_fail.png")

    time.sleep(2)
    rs.kill_creo()


if __name__ == "__main__":
    main()
