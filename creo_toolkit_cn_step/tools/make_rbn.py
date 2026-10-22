# -*- coding: utf-8 -*-
"""
make_rbn.py - 生成落落工具的功能区定义文件 toolkitribbonui.rbn

背景
----
Creo 没有「用代码往功能区加一个选项卡」的 API。唯一的载体是 .rbn
（ProRibbonDefinitionfileLoad 只吃这个），而 .rbn 是 PTC 的 UGC 容器。
好消息：UGC 在「未压缩」形态下就是纯文本，一行一条记录：

    <层级> <属性名> <值>

数字越大越深，行尾的 "->" 表示后面跟着子节点，"#END_OF_UGC" 收尾。

结构对照 PTC 自带的两个范例（都是明文）：
  Common Files\\otk\\...\\ribbon\\otkAsyncExamples.rbn   （最小示例）
  Common Files\\afx\\text\\ribbon\\afx_ribbon.rbn        （官方插件，多模式）

关键点：
1. 自定义页必须挂在**已存在的 ribbon** 上，页名带 @ribbon 后缀；
2. 一个 .rbn 可以同时给多个模式各挂一页——第一版只挂了 main_adb_cat，
   结果选项卡只出现在起始页。零件/组件模式的 ribbon 名字分别是
   prt_main_adb_cat / asm_main_adb_cat（模式名清单可 grep 安装目录
   下明文 .rbn 里的 "_main_adb_cat" 得到）；
3. modeList / ribbonList / pageList 三处的条目数要一致。
"""

import io
import os

# --------------------------------------------------------------------- 头部
HEADER = (
    "#UGC:2 NEUTRAL 1606 440 800 0 0 15 3300 2011500 00000000 \\\n"
    "#- VERS 0 0                                                          \\\n"
    "#- HOST                                                              \\\n"
    "#- LINK                                                              \\\n"
    "#- DBID                                                              \\\n"
    "#- REVS 0,                                                           \\\n"
    "#- RELL 0,                                                           \\\n"
    "#- UOBJ_ID 0 0 0                                                     \\\n"
    "#- MACH                                                              \\\n"
    "#-END_OF_UGC_HEADER\n"
)

BYLINE = ("#Creo  TM  Version P-10-14  (c) 2012 by Parametric Technology "
          "Corporation  All Rights Reserved. B000\n")

# 零件 / 钣金零件 / 组件 / 组件(激活态) —— 都是 Creo 里真实存在的 ribbon 名
MODES = [
    "prt_main_adb_cat",
    "prt_smt_main_adb_cat",
    "asm_main_adb_cat",
    "act_asm_main_adb_cat",
]

GROUP = "user_custom_group_luoluo1"


def page_name(mode):
    return "user_custom_page_luoluo1@" + mode


def make(tab_label, group_label, cmd_export, cmd_about):
    """返回 .rbn 全文（str）。"""

    L = []
    n = len(MODES)

    def add(level, name, value=None, arrow=False):
        s = "%d %s" % (level, name)
        if value is not None:
            s += " " + value
        else:
            s += " "            # 值的占位空格，PTC 自己也是这么写的
        if arrow:
            s += "->"
        L.append(s)

    def command(name):
        add(6, "containerChildrenArray")
        add(7, "ChildUnion(RbnDBCommand)", arrow=True)
        add(8, "mName", name)               # ProCmdActionAdd 注册的命令名
        add(8, "mType", "1")                # 1 = RbnDBCommand
        add(8, "Attributes")
        add(8, "mComponentClass", name)
        add(8, "mColumns", "0")

    # ---- 根 ----
    add(0, "RbnDBData", arrow=True)
    add(1, "onCreation")
    add(2, "mNumComments", "0")
    add(2, "mDate")

    # ---- 组：两个命令放一组（所有模式共用这一个组） ----
    add(1, "groupList [1]")
    add(2, "groupList")
    add(3, "ChildUnion(RbnDBGroup)", arrow=True)
    add(4, "RbnDBInheritContainer")
    add(5, "mName", GROUP)
    add(5, "mType", "12")                   # 12 = 自定义 group
    add(5, "mLabel", group_label)           # 组标题（显示在按钮下方）
    add(5, "mAncestor")
    add(5, "mKeyTip")
    add(5, "mCustom", "1")
    add(5, "Attributes")
    add(5, "containerChildrenArray [2]")
    command(cmd_export)
    command(cmd_about)
    add(4, "mToolLauncher")
    add(4, "mBitmap")
    add(4, "mPlugin")

    # ---- 页：每个模式一页，页名带 @模式 ribbon 名 ----
    add(1, "pageList [%d]" % n)
    for mode in MODES:
        add(2, "pageList")
        add(3, "ChildUnion(RbnDBPage)", arrow=True)
        add(4, "RbnDBInheritContainer")
        add(5, "mName", page_name(mode))
        add(5, "mType", "11")               # 11 = 自定义 page
        add(5, "mLabel", tab_label)         # 选项卡标题
        add(5, "mAncestor")
        add(5, "mKeyTip")
        add(5, "mCustom", "1")
        add(5, "Attributes")
        add(5, "containerChildrenArray [1]")
        add(6, "containerChildrenArray")
        add(7, "ChildUnion(RbnDBGroupRef)", arrow=True)
        add(8, "mName", GROUP)
        add(8, "mType", "15")               # 15 = group 引用
        add(8, "Attributes")
        add(8, "mRuntimeRemoved", "0")
        add(4, "mCloseButton", "__RBN_DB_NO_CHANGE__")
        add(4, "mPlugin")

    # ---- ribbon：把页挂到每个已存在的模式 ribbon 上 ----
    add(1, "ribbonList [%d]" % n)
    for mode in MODES:
        add(2, "ribbonList")
        add(3, "ChildUnion(RbnDBRibbon)", arrow=True)
        add(4, "RbnDBContainer")
        add(5, "mName", mode)               # 引用，不是新建
        add(5, "mType", "10")
        add(5, "Attributes")
        add(5, "containerChildrenArray [1]")
        add(6, "containerChildrenArray")
        add(7, "ChildUnion(RbnDBPageRef)", arrow=True)
        add(8, "mName", page_name(mode))
        add(8, "mType", "16")               # 16 = page 引用
        add(8, "Attributes")
        add(8, "mRuntimeRemoved", "0")
        add(4, "mLabel", "__RBN_DB_NO_CHANGE__")
        add(4, "mDescription", "__RBN_DB_NO_CHANGE__")
        add(4, "mPlugin")
        add(4, "mCustom", "0")

    # ---- 模式 ----
    add(1, "modeList [%d]" % n)
    for mode in MODES:
        add(2, "modeList")
        add(3, "ChildUnion(RbnDBMode)", arrow=True)
        add(4, "RbnDBContainer")
        add(5, "mName", mode)
        add(5, "mType", "9")
        add(5, "Attributes")
        add(4, "mRibbon", mode)
        add(4, "mLabel", "__RBN_DB_NO_CHANGE__")
        add(4, "mDescription", "__RBN_DB_NO_CHANGE__")

    return HEADER + BYLINE + "\n".join(L) + "\n#END_OF_UGC\n"


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(os.path.dirname(here), "release", "text")

    variants = [
        # 语言目录, 选项卡名, 组名, 命令1, 命令2
        ("chinese_cn", "落落工具", "落落工具",
         "CnStepExportCmd", "CnStepAboutCmd"),
        # 英文界面下也显示中文名——需求就是「选项卡名称为落落工具」，
        # 不随界面语言变。.rbn 里的字符串按 UTF-8 存，Creo 直接按 Unicode 显示。
        ("usascii", "落落工具", "落落工具",
         "CnStepExportCmd", "CnStepAboutCmd"),
    ]

    for lang, tab, grp, c1, c2 in variants:
        text = make(tab, grp, c1, c2)
        d = os.path.join(out, lang, "ribbon")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, "toolkitribbonui.rbn")
        with io.open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s (%d bytes)" % (p, len(text.encode("utf-8"))))


if __name__ == "__main__":
    main()
