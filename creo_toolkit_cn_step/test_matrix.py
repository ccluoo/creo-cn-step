# -*- coding: utf-8 -*-
"""交叉加载矩阵：用 N 版本的 ucore68.dll 加载 M 版本编译的 cn_step DLL。

做法：为每个 ucore 版本建一个独立目录，把该版本的 ucore68.dll 和全部 6 个
cn_step_cM.dll 都放进去。Windows 加载器总是先查 DLL 自身所在目录，所以
"哪个 ucore68.dll 被用上"是确定的，不受 PATH 顺序影响。每个组合跑一个独立
子进程，避免同名模块被进程内缓存。
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = r"C:\Program Files\PTC"
INST = {
    "8": "8.0.12.0",
    "9": "9.0.10.0",
    "10": "10.0.8.0",
    "11": "11.0.4.0",
    "12": "12.4.1.0",
    "13": "13.4.1.0",
}
HERE = os.path.dirname(os.path.abspath(__file__))

CHILD = r'''
import ctypes, sys
try:
    h = ctypes.WinDLL(sys.argv[1])
    f = ctypes.cast(getattr(h, "user_initialize"), ctypes.c_void_p).value
    print("OK %#x" % f)
except OSError as e:
    print("FAIL " + str(e))
'''


def main():
    vers = list(INST)
    tmp = tempfile.mkdtemp(prefix="cncore_")
    child = os.path.join(tmp, "child.py")
    open(child, "w", encoding="utf-8").write(CHILD)

    for n in vers:
        d = os.path.join(tmp, "core" + n)
        os.makedirs(d, exist_ok=True)
        # ucore68.dll itself depends on udata68.dll - both must be staged
        src = os.path.join(ROOT, "Creo " + INST[n], "Common Files", "x86e_win64", "obj")
        for f in ("ucore68.dll", "udata68.dll"):
            shutil.copy2(os.path.join(src, f), os.path.join(d, f))
        for m in vers:
            shutil.copy2(
                os.path.join(HERE, "build", "cn_step_c%s.dll" % m),
                os.path.join(d, "cn_step_c%s.dll" % m),
            )

    print("rows = ucore68.dll version (from that Creo install)")
    print("cols = which SDK the DLL was compiled against")
    print("       " + "".join("%-7s" % ("c" + m) for m in vers))
    fails = []
    for n in vers:
        row = []
        for m in vers:
            dll = os.path.join(tmp, "core" + n, "cn_step_c%s.dll" % m)
            r = subprocess.run([sys.executable, child, dll],
                               capture_output=True, text=True, timeout=180)
            out = (r.stdout or "").strip().splitlines()
            last = out[-1] if out else ((r.stderr or "?").strip().splitlines() or ["?"])[-1]
            ok = last.startswith("OK")
            row.append("OK" if ok else "FAIL")
            if not ok:
                fails.append((n, m, last))
        print("core%-3s" % n + "".join("%-7s" % c for c in row))

    for n, m, msg in fails:
        print("\nFAIL core%s + c%s: %s" % (n, m, msg[:220]))

    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d/%d combos OK" % (len(vers) ** 2 - len(fails), len(vers) ** 2))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
