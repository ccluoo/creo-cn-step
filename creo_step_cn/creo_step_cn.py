# -*- coding: utf-8 -*-
"""
Creo STEP 中文名修复工具 (creo_step_cn)

解决什么问题
------------
Creo 13 导出 STEP 时无法使用中文文件名。绕过方式：在 Creo 里用英文/拼音名导出，
再用本工具把文件重命名成中文，同时改写 STEP 头部 FILE_NAME 字段（按 ISO 10303-21
的 \\X2\\ 编码），这样 SolidWorks / NX / CATIA / Creo 打开时零件名也是中文。

用法
----
1) 双击 启动.bat（或 python creo_step_cn.py）
2) 选目录 -> 扫描 -> 逐行填中文名（或载入 名称对照.csv）-> 执行
3) 可选：勾选“先备份”，原文件会复制到 _step_backup_<时间戳>/ 后再改

命令行自检
----------
python creo_step_cn.py --self-test
"""

import csv
import os
import re
import shutil
import sys
import time

STEP_EXTS = (".stp", ".step", ".STP", ".STEP")
MAPPING_NAME = "名称对照.csv"


# ---------------------------------------------------------------- STEP 编解码

def encode_step_string(s):
    """把任意字符串编码成 ISO 10303-21 的 part 21 字符串内容。
    非 ASCII 字符用 \\X2\\XXXX...\\X0\\ 包裹；单引号写成两个单引号。"""
    out = []
    buf = []

    def flush():
        if buf:
            out.append("\\X2\\" + "".join("%04X" % ord(c) for c in buf) + "\\X0\\")
            del buf[:]

    for ch in s:
        code = ord(ch)
        if ch == "'":
            flush()
            out.append("''")
        elif ch == "\\":
            flush()
            out.append("\\\\")
        elif 0x20 <= code <= 0x7E:
            flush()
            out.append(ch)
        else:
            buf.append(ch)
    flush()
    return "".join(out)


def decode_step_string(s):
    """把 \\X2\\ 编码还原成可读文本，用于列表显示。"""
    def x2(m):
        hexs = m.group(1)
        return "".join(chr(int(hexs[i:i + 4], 16)) for i in range(0, len(hexs) - 3, 4))
    s = re.sub(r"\\X2\\([0-9A-Fa-f]+)\\X0\\", x2, s)
    s = s.replace("''", "'")
    return s


def find_file_name(text):
    """返回 (start, end, value)：FILE_NAME(...) 第一个字符串字面量在 text 中的位置。"""
    i = text.find("FILE_NAME(")
    if i < 0:
        return None
    j = i + len("FILE_NAME(")
    while j < len(text) and text[j] in " \r\n\t":
        j += 1
    if j >= len(text) or text[j] != "'":
        return None
    k = j + 1
    buf = []
    while k < len(text):
        c = text[k]
        if c == "'":
            if k + 1 < len(text) and text[k + 1] == "'":
                buf.append("'")
                k += 2
                continue
            break
        buf.append(c)
        k += 1
    return j + 1, k, "".join(buf)


def read_head(path, limit=200000):
    with open(path, "r", encoding="latin-1", errors="replace") as f:
        return f.read(limit)


def get_step_name(path):
    """读 STEP 头里的 FILE_NAME（已解码）。没有则返回 None。"""
    head = read_head(path)
    got = find_file_name(head)
    return decode_step_string(got[2]) if got else None


def rewrite_step_name(path, chinese_name):
    """把文件头 FILE_NAME 的第一个参数改成 chinese_name（UTF-8 磁盘、\\X2\\ 编码值）。"""
    with open(path, "r", encoding="latin-1", errors="replace") as f:
        text = f.read()
    got = find_file_name(text)
    if not got:
        return False
    start, end, _old = got
    new_text = text[:start] + encode_step_string(chinese_name) + text[end:]
    with open(path, "w", encoding="latin-1", errors="replace", newline="") as f:
        f.write(new_text)
    return True


# ---------------------------------------------------------------- 批量处理

def scan(folder):
    files = []
    for name in sorted(os.listdir(folder)):
        if name.lower().endswith((".stp", ".step")):
            full = os.path.join(folder, name)
            if os.path.isfile(full):
                files.append(full)
    return files


def load_mapping(folder):
    """读 名称对照.csv：两列，第一列=导出时的英文名(不含扩展名)或文件名，第二列=中文名。"""
    path = os.path.join(folder, MAPPING_NAME)
    mapping = {}
    if not os.path.isfile(path):
        return mapping
    for enc in ("utf-8-sig", "gbk"):
        try:
            with open(path, "r", encoding=enc, newline="") as f:
                for row in csv.reader(f):
                    if len(row) < 2:
                        continue
                    key = row[0].strip()
                    val = row[1].strip()
                    if key and val and key != val:
                        mapping[key] = val
                        mapping[os.path.splitext(key)[0]] = val
            break
        except UnicodeDecodeError:
            mapping.clear()
            continue
    return mapping


def unique_target(folder, stem, ext, taken):
    candidate = stem + ext
    n = 2
    while candidate.lower() in taken or os.path.exists(os.path.join(folder, candidate)):
        candidate = "%s_%d%s" % (stem, n, ext)
        n += 1
    taken.add(candidate.lower())
    return candidate


def apply_changes(folder, plan, backup=True, rewrite_header=True, dry_run=False):
    """plan: [(原完整路径, 中文名)]，返回日志行列表。"""
    logs = []
    backup_dir = None
    if backup and not dry_run:
        stamp = time.strftime("%Y%m%d_%H%M%S")
        backup_dir = os.path.join(folder, "_step_backup_%s" % stamp)
        os.makedirs(backup_dir, exist_ok=True)

    taken = set()
    for src, cn in plan:
        cn = (cn or "").strip()
        if not cn:
            logs.append("跳过（未填中文名）：%s" % os.path.basename(src))
            continue
        ext = os.path.splitext(src)[1]
        safe_cn = re.sub(r'[\\/:*?"<>|\r\n\t]', "_", cn).strip(" .")
        if not safe_cn:
            logs.append("跳过（中文名不可用）：%s" % os.path.basename(src))
            continue
        if os.path.normcase(safe_cn + ext) == os.path.normcase(os.path.basename(src)):
            if dry_run:
                logs.append("[预览] %s  已同名，仅改写头部" % os.path.basename(src))
                continue
            try:
                ok = rewrite_step_name(src, safe_cn) if rewrite_header else False
                logs.append("仅改写头部：%s%s" % (os.path.basename(src),
                                            "" if ok or not rewrite_header else "（警告：未找到 FILE_NAME）"))
            except Exception as exc:  # noqa: BLE001
                logs.append("失败：%s（%s）" % (os.path.basename(src), exc))
            continue

        target_name = unique_target(folder, safe_cn, ext, taken)
        dst = os.path.join(folder, target_name)

        if dry_run:
            logs.append("[预览] %s  ->  %s" % (os.path.basename(src), target_name))
            continue

        try:
            if backup_dir:
                shutil.copy2(src, os.path.join(backup_dir, os.path.basename(src)))
            ok_header = rewrite_step_name(src, os.path.splitext(target_name)[0]) if rewrite_header else False
            os.replace(src, dst)
            logs.append("完成：%s  ->  %s%s" % (
                os.path.basename(src), target_name,
                "" if ok_header or not rewrite_header else "（警告：未找到 FILE_NAME 头，仅改了文件名）"))
        except Exception as exc:  # noqa: BLE001
            logs.append("失败：%s（%s）" % (os.path.basename(src), exc))

    if backup_dir:
        logs.append("备份目录：%s" % backup_dir)
    return logs


# ---------------------------------------------------------------- GUI

def run_gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    root = tk.Tk()
    root.title("Creo STEP 中文名修复工具")
    root.geometry("880x560")
    root.minsize(760, 480)

    state = {"folder": "", "rows": []}

    top = ttk.Frame(root, padding=10)
    top.pack(fill="x")

    folder_var = tk.StringVar(value="（未选择目录）")
    ttk.Label(top, text="STEP 目录：").pack(side="left")
    ttk.Label(top, textvariable=folder_var, foreground="#555").pack(side="left", padx=(0, 10))

    backup_var = tk.BooleanVar(value=True)
    header_var = tk.BooleanVar(value=True)
    dry_var = tk.BooleanVar(value=False)

    list_wrap = ttk.Frame(root, padding=(10, 0))
    list_wrap.pack(fill="both", expand=True)
    canvas = tk.Canvas(list_wrap, highlightthickness=0)
    scroll = ttk.Scrollbar(list_wrap, orient="vertical", command=canvas.yview)
    inner = ttk.Frame(canvas)
    inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
    canvas.create_window((0, 0), window=inner, anchor="nw")
    canvas.configure(yscrollcommand=scroll.set)
    canvas.pack(side="left", fill="both", expand=True)
    scroll.pack(side="right", fill="y")

    log_box = tk.Text(root, height=8, wrap="word")
    log_box.pack(fill="x", padx=10, pady=(8, 4))

    def log(lines):
        for line in lines:
            log_box.insert("end", line + "\n")
        log_box.see("end")

    def clear_rows():
        for child in inner.winfo_children():
            child.destroy()
        state["rows"] = []

    def do_scan():
        folder = filedialog.askdirectory(title="选择 STEP 文件所在目录")
        if not folder:
            return
        state["folder"] = folder
        folder_var.set(folder)
        clear_rows()
        files = scan(folder)
        mapping = load_mapping(folder)
        if not files:
            log(["该目录下没有 .stp/.step 文件"])
            return
        ttk.Label(inner, text="原文件", width=34, anchor="w").grid(row=0, column=0, padx=6, pady=4)
        ttk.Label(inner, text="导出后的中文名（可改）", width=40, anchor="w").grid(row=0, column=1, padx=6, pady=4)
        ttk.Label(inner, text="现有头部名", width=22, anchor="w").grid(row=0, column=2, padx=6, pady=4)
        for idx, full in enumerate(files, start=1):
            base = os.path.basename(full)
            stem = os.path.splitext(base)[0]
            suggestion = mapping.get(base) or mapping.get(stem) or ""
            ttk.Label(inner, text=base, width=34, anchor="w").grid(row=idx, column=0, padx=6, pady=2)
            var = tk.StringVar(value=suggestion)
            ent = ttk.Entry(inner, textvariable=var, width=40)
            ent.grid(row=idx, column=1, padx=6, pady=2, sticky="w")
            head_name = get_step_name(full) or "-"
            ttk.Label(inner, text=head_name[:24], width=22, anchor="w").grid(row=idx, column=2, padx=6, pady=2)
            state["rows"].append((full, var))
        log(["扫描到 %d 个 STEP 文件；对照表%s" % (
            len(files), "已载入" if mapping else "未找到（可手动填）")])

    def do_fill_from_csv():
        if not state["folder"]:
            messagebox.showinfo("提示", "先选择目录")
            return
        mapping = load_mapping(state["folder"])
        if not mapping:
            messagebox.showinfo("提示", "目录下没有 %s（两列：英文名,中文名）" % MAPPING_NAME)
            return
        hit = 0
        for full, var in state["rows"]:
            base = os.path.basename(full)
            stem = os.path.splitext(base)[0]
            val = mapping.get(base) or mapping.get(stem) or ""
            if val:
                var.set(val)
                hit += 1
        log(["按对照表自动填入 %d 条" % hit])

    def do_run():
        if not state["rows"]:
            messagebox.showinfo("提示", "先扫描目录")
            return
        plan = [(full, var.get()) for full, var in state["rows"] if var.get().strip()]
        if not plan:
            messagebox.showinfo("提示", "还没有填任何中文名")
            return
        if not dry_var.get() and not messagebox.askokcancel("确认", "将处理 %d 个文件，继续？" % len(plan)):
            return
        logs = apply_changes(state["folder"], plan, backup=backup_var.get(),
                             rewrite_header=header_var.get(), dry_run=dry_var.get())
        log(logs)
        if not dry_var.get():
            do_scan_quiet()

    def do_scan_quiet():
        folder = state["folder"]
        if not folder:
            return
        clear_rows()
        files = scan(folder)
        mapping = load_mapping(folder)
        ttk.Label(inner, text="原文件", width=34, anchor="w").grid(row=0, column=0, padx=6, pady=4)
        ttk.Label(inner, text="导出后的中文名（可改）", width=40, anchor="w").grid(row=0, column=1, padx=6, pady=4)
        ttk.Label(inner, text="现有头部名", width=22, anchor="w").grid(row=0, column=2, padx=6, pady=4)
        for idx, full in enumerate(files, start=1):
            base = os.path.basename(full)
            stem = os.path.splitext(base)[0]
            ttk.Label(inner, text=base, width=34, anchor="w").grid(row=idx, column=0, padx=6, pady=2)
            var = tk.StringVar(value=mapping.get(base) or mapping.get(stem) or "")
            ttk.Entry(inner, textvariable=var, width=40).grid(row=idx, column=1, padx=6, pady=2, sticky="w")
            ttk.Label(inner, text=(get_step_name(full) or "-")[:24], width=22, anchor="w").grid(
                row=idx, column=2, padx=6, pady=2)
            state["rows"].append((full, var))

    btns = ttk.Frame(root, padding=(10, 6))
    btns.pack(fill="x")
    ttk.Button(btns, text="选择目录并扫描", command=do_scan).pack(side="left")
    ttk.Button(btns, text="按对照表填写", command=do_fill_from_csv).pack(side="left", padx=8)
    ttk.Checkbutton(btns, text="先备份", variable=backup_var).pack(side="left", padx=(16, 4))
    ttk.Checkbutton(btns, text="同时改写 STEP 头 FILE_NAME", variable=header_var).pack(side="left", padx=4)
    ttk.Checkbutton(btns, text="仅预览", variable=dry_var).pack(side="left", padx=4)
    ttk.Button(btns, text="执行", command=do_run).pack(side="right")

    if os.environ.get("STEPCN_SMOKE"):
        root.after(600, root.destroy)
    root.mainloop()


# ---------------------------------------------------------------- 自检

def _self_test():
    import tempfile
    tmp = tempfile.mkdtemp(prefix="stepcn_")
    sample = (
        "ISO-10303-21;\nHEADER;\n"
        "FILE_DESCRIPTION(('test'),'2;1');\n"
        "FILE_NAME('P001','2026-10-20T12:00:00',('a'),('o'),'pre','sys','');\n"
        "FILE_SCHEMA(('AP242'));\nENDSEC;\nDATA;\n#1=PRODUCT('P001','P001','',(#2));\nENDSEC;\nEND-ISO-10303-21;\n"
    )
    for name in ("P001.stp", "P002.stp"):
        with open(os.path.join(tmp, name), "w", encoding="latin-1", newline="") as f:
            f.write(sample)
    mapping_path = os.path.join(tmp, MAPPING_NAME)
    with open(mapping_path, "w", encoding="utf-8-sig", newline="") as f:
        f.write("P001,轴承座\nP002,端盖\n")

    assert encode_step_string("轴承座") == "\\X2\\8F74627F5EA7\\X0\\", encode_step_string("轴承座")
    assert decode_step_string(encode_step_string("端盖 A1")) == "端盖 A1"

    files = scan(tmp)
    assert len(files) == 2, files
    mapping = load_mapping(tmp)
    plan = [(f, mapping[os.path.splitext(os.path.basename(f))[0]]) for f in files]
    logs = apply_changes(tmp, plan, backup=True, rewrite_header=True, dry_run=False)
    assert all("完成" in x for x in logs if x.startswith(("完成", "失败"))), logs

    result = sorted(os.listdir(tmp))
    assert "轴承座.stp" in result, result
    assert "端盖.stp" in result, result
    head = read_head(os.path.join(tmp, "轴承座.stp"))
    assert "\\X2\\8F74627F5EA7\\X0\\" in head, head[:300]
    assert get_step_name(os.path.join(tmp, "轴承座.stp")) == "轴承座"
    print("SELF-TEST PASSED |", len(result), "entries |", [x for x in result if not x.startswith("_")])
    print("log:", logs[:2])


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        _self_test()
    else:
        run_gui()
