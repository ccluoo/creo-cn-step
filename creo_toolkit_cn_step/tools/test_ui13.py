# -*- coding: utf-8 -*-
"""
test_ui13.py - 在真实的 Creo 里把三个 UI 行为跑一遍并抓图。

被测的三件事（就是用户提的问题 1 / 3 / 4）：
  1. 点功能区「落落工具」选项卡 -> 组里两个命令都在
  2. 点「关于落落」          -> 弹出自绘对话框，不闪退不卡死
  3. 点「导出中文 STEP」     -> 弹出保存对话框，路径可改

跑法：
    python tools/test_ui13.py 13.4.1.0 build/part_path.txt

注意：必须一次调用里做完启动->点击->抓图，进程在调用结束后会被回收。
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


def grab_full(path):
    """整屏抓图。关于窗口是 WS_EX_TOPMOST，会浮在所有窗口之上，
    所以整屏抓比 PrintWindow 省事——不用先找到那个窗口句柄。"""
    from PIL import ImageGrab
    img = ImageGrab.grab(all_screens=True)
    img.save(path)
    return img.size


def find_dialog(keywords, tries=10):
    for _ in range(tries):
        for h, t in rs.all_windows():
            low = t.lower()
            if any(k.lower() in low for k in keywords):
                return h, t
        time.sleep(1.0)
    return None


def post_close(hwnd):
    user32.PostMessageW(hwnd, 0x0010, 0, 0)      # WM_CLOSE
    time.sleep(1.5)


def main():
    install = sys.argv[1]
    part = open(sys.argv[2], encoding="utf-8").read().strip()
    tag = install.replace(".", "")
    root = r"C:\Program Files\PTC\Creo " + install
    exe = os.path.join(root, "Parametric", "bin", "parametric.exe")

    work = os.path.join(os.environ["TEMP"], "luoluo_ui" + tag)
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
    time.sleep(30)                      # 等功能区画完

    # 1) 切到落落工具选项卡
    rs.click_window(hwnd, 916, 45)
    time.sleep(3)
    ok, w, h = shot.capture(hwnd, "build/ui1_tab.png")
    print("tab capture %dx%d -> build/ui1_tab.png" % (w, h))

    # 2) 关于落落
    rs.click_window(hwnd, 55, 100)
    time.sleep(3)
    hit = find_dialog(["关于落落"])
    print("about window:", hit)
    grab_full("build/ui2_about.png")
    print("about grabbed -> build/ui2_about.png")
    if hit:
        post_close(hit[0])

    # 3) 导出：这里会弹保存对话框
    rs.click_window(hwnd, 55, 74)
    time.sleep(5)
    hit2 = find_dialog(["导出中文 STEP", "选择保存位置", "Save As", "另存为"])
    print("save dialog:", hit2)
    grab_full("build/ui3_save.png")
    print("save grabbed -> build/ui3_save.png")
    if hit2:
        post_close(hit2[0])

    rs.kill_creo()


if __name__ == "__main__":
    main()
