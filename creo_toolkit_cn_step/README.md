# cn_step —— Creo 12 / 13 通用的一键导出中文名 STEP

Creo 自带的 STEP 导出器不支持中文输出路径/文件名，本工程用 Pro/TOOLKIT 绕过这个限制：
在 Creo 里加一个菜单项，点一下就把当前模型导出成中文文件名，并把 STEP 头里的 `FILE_NAME`
也一并改成中文。

**中文名有两个完全独立的位置**，只改一个是不够的：

| 位置 | 决定什么 | 只改这个的后果 |
|---|---|---|
| `FILE_NAME('...')` 头 + 磁盘文件名 | 文件叫什么 | SolidWorks 里文件名是中文，但装配树里组件还是英文 |
| `PRODUCT('id','name',...)` 实体 | 装配树里组件显示什么 | ← 这才是打开后真正看到的东西 |

本工程两处都改。详见第「六」节。

---

## 更新记录（2026-10-22 第二轮）

**Creo 2.0 已剔除**——SDK 兼容成本太高（见下一节），只保证 **Creo 8.0 ~ 13.0**。
部署脚本遇到 2.0 会 `[skip]`，不再往里面拷文件。

这一轮修了四个实际问题：

1. **「关于落落」闪退/无反应** —— 旧实现是内存对话框模板
   （`DialogBoxIndirectParamW` + 手工拼 `DLGTEMPLATE`），模板里任何一处
   DWORD 对齐算错，Creo 就闪退或干脆不弹。改成**自绘窗口**（`CreateWindowExW`
   + 自己的消息循环 + `WM_PAINT` 里画全部内容），彻底绕开模板；
   窗口建不出来时退化到 `MessageBoxW`，内容一样看得到。

2. **选项卡只在起始页出现** —— 第一版把页挂在 `main_adb_cat`
   （对应"还没打开模型"的起始页）。零件模式其实是 `prt_main_adb_cat`，
   装配是 `asm_main_adb_cat`。现在 `tools\make_rbn.py` 一次生成 4 个模式
   （零件 / 钣金零件 / 装配 / 激活装配）的页，实测 Creo 13 零件模式
   选项卡正常出现并显示「落落工具」。

3. **导出命名规则** —— 模型名 + 6 位日期（YYMMDD）：
   - Creo 用：`大歼36配件_261022.stp`
   - 其它软件用：`大歼36配件_other_261022.stp`
   `_other` 自动插在日期前面，不是接在末尾。

4. **导出前先选路径** —— 点「导出中文 STEP」会弹系统保存对话框，
   默认名已填好、默认目录取 `cn_step.cfg` 的 `outdir`，可以改目录改名字，
   点「保存」才真正开始导；点取消则安静退出。

---

## 更新记录（2026-10-22 第一轮）

这一轮做了五件事，其中 **Creo 2.0 后来被剔除**（保留记录备查）：

1. **Creo 2.0 支持（SDK 是 VS2010 编的）**
   - SDK 在 `Common Files\M250\protoolkit`（M 码子目录），库是单一 `protk_dll.lib`，
     里面带 `/FAILIFMISMATCH:"_MSC_VER=1600"` 和 `/DEFAULTLIB:"libcpmt"`。
   - 直接混链会报 LNK2038（`_MSC_VER=1900` vs `1600`）；就算链过去，加载即崩
     （`ERROR_DLL_INIT_FAILED 1114`）。
   - **根因**：`protk_dll.lib` 自带 `/DEFAULTLIB:"libcpmt"`，于是 VS2022 的
     libcpmt 被拉进来 528 个符号，其中的 C++ 静态初始化器（`??__Eclassic_locale`、
     `??__Ecout`、`??__Einitlocks`……）在 DLL 加载时对 VS2010 的 STL 数据乱来，
     DllMain 返回 FALSE。
   - **解法**：链接时 `/NODEFAULTLIB:libcpmt.lib` + 把 `msvcp100.lib`（从
     msvcp100.dll 用 lib.exe /def 生成的导入库，1676 个导出）放在 `protk_dll.lib`
     **之后**——LINK 从左往右解析，这样 STL 符号全部落在 msvcp100.dll（VS2010
     C++ 运行库，Creo 2 本来就要加载它），ABI 完全一致。剩下的 5 个符号
     （`?_open`/`?_sopen`/`std::exception` ctor/`_Facet_Register`/`bad_cast` ctor）
     由 `src\c2_shim.cpp` + `/alternatename:` 补齐。
   - 源码兼容层（`CN_STEP_OLD_SDK`）：Creo 2 没有 `ProMdlMdlnameGet`/`ProMdlName`
     → 用 `ProMdlNameGet` + `ProName`；没有 `ProIntf3DFileWriteWithDefaultProfile`
     → 用 8 参数版 `ProIntf3DFileWrite`（`ProOutputAssemblyConfiguration` 是枚举，
     按值传 `PRO_OUTPUT_ASSEMBLY_SINGLE_FILE`）。
   - 实测：Creo 2.0 加载成功，`menu=0 cmd=0 btn=0 about=0 dsg=0/0 rbn=0`。

2. **一键部署改造：文件进各 Creo 根目录的 `cc_luoluoo\`**
   - `release\一键部署.bat`（内部 `deploy.ps1`）扫描所有 Creo，在
     `<Creo 根目录>\cc_luoluoo\` 放全套插件文件，DLL 按版本自动挑
     （Creo 2→c2，8→c8……13→c13，统一改名叫 `cn_step.dll`），
     注册行指向该目录。旧注册行先清掉再写，幂等；改前自动备份。

3. **菜单改名**：一级菜单「落落工具」，二级「导出中文 STEP（当前模型）」
   和「关于落落」。中英文消息文件都在 `text\` 下。

4. **「关于落落」对话框**：原生 Win32 内存对话框（不依赖 ProUIDialog），
   含参考文献、WorkBuddy 制作、红色免责声明、联系方式二维码
   （DLL 同目录 `luoluo_qr.bmp`）。

5. **功能区选项卡「落落工具」**：`.rbn` 是 PTC 的 UGC 容器，未压缩形态是
   **纯文本**，可以程序生成（`tools\make_rbn.py`）。自定义页必须挂在
   **已存在的 `main_adb_cat`** 上（不能自己发明 ribbon 名）才会显示。
   实测 Creo 8.0.12.0 选项卡出现。技术细节见 `release\功能区选项卡.txt`。

7 个版本（2/8/9/10/11/12/13）各自编译通过；部署方式见 `release\安装说明.txt`。

---

## 零、Creo 8 ~ 13：真实兼容性（6 个版本全部实测）

本机装了 6 个 Creo：`8.0.12.0`、`9.0.10.0`、`10.0.8.0`、`11.0.4.0`、`12.4.1.0`、`13.4.1.0`，
六套 SDK 各编了一份 DLL，六个 Creo 各实际启动加载过。结论和常见直觉**相反**——

### 0.1 关键规则：DLL 只能在 **≥ 编译版本** 的 Creo 上跑

| DLL 用哪个 SDK 编 | Creo 8 | Creo 9 | Creo 10 | Creo 11 | Creo 12 | Creo 13 |
|---|---|---|---|---|---|---|
| **Creo 8** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Creo 12 | ❌ | — | — | — | ✅ | ✅ |
| Creo 13 | — | — | — | — | ❌ | ✅ |

版本不够时，Creo 会直接拒绝加载，trail 文件里留下这段：

```
!Application (cn_step): running DLL-mode user_initialize
!Application (cn_step): returned from DLL-mode user_initialize; status is -98
!               : Application "cn_step" requires Creo 12; the current version is Creo 8.0
!               : Startup of application "cn_step" failed.
```

`status = -98` 就是版本不匹配。Pro/TOOLKIT 客户端库把"我要求的最低 Creo 版本"写死在
DLL 里（等于编译时的 SDK 版本），Creo 启动时比对，达不到就拒。

**所以：用最老的 SDK（Creo 8）编一份，就能通吃 8/9/10/11/12/13。**
实测六个版本 `user_initialize` 全部 `menu=0 cmd=0 btn=0`。

> 上一版文档说"用 12 编的兼容面最大"，方向对但没验证到底——真正的最优点是**8**。

### 0.2 静态对照：6 个版本内部几乎一致

| 检查项 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|
| `protk.dat` 的 `REVISION` | 18 | 18 | 18 | 18 | 18 | 18 |
| 头文件数量 | 413 | 414 | 423 | 429 | 430 | 433 |
| `protk_dll_NU.lib` | 50.3 MB | 51.4 MB | 58.7 MB | 61.6 MB | 64.0 MB | 66.7 MB |
| 运行时依赖 `ucore68.dll` | 1.70 MB | 同 | 同 | 同 | 同 | 同 |
| `ucore.lib` / `udata.lib` | — | — | — | — | 同 md5 | 同 md5 |
| 导出符号（12 个） | 同 | 同 | 同 | 同 | 同 | 同 |
| `.fptable` 节（512 B） | 同 | 同 | 同 | 同 | 同 | 同 |

`ucore68.dll` 其实是 PTC 打包的 **ICU 68**（国际化字符库），6 个版本同名同尺寸，
9~13 甚至完全同一个文件（md5 一致），8 那版也只是编译差异。

### 0.3 本插件用到的 8 个 API，6 个版本声明逐字符相同

```
ProMenubarMenuAdd                      ProMenuBar.h      ✓
ProMenubarmenuPushbuttonAdd            ProMenuBar.h      ✓
ProCmdActionAdd                        ProPopupmenu.h    ✓
ProMdlCurrentGet                       ProMdl.h          ✓
ProMdlMdlnameGet                       ProMdl.h          ✓
ProMdlTypeGet                          ProMdl.h          ✓
ProStringToWstring                     ProUtil.h         ✓
ProIntf3DFileWriteWithDefaultProfile   ProIntf3Dexport.h ✓
```

> 提示框用的是 Windows 原生 `MessageBoxW`，不是 `ProMessageDisplay`——后者收窄字符，
> 中文要过代码页，容易乱。所以 `ProMessage.h` 虽然各版一致，本插件也没用它。

差异全是**新增**：13 多了 `ProDatumRef.h`/`ProEcadUtils.h`/`ProSubHarness.h`、
`PRO_INTF_EXPORT_3D_PDF`/`PRO_INTF_EXPORT_USD`、`PRO_SPOT_WELD_PNT`/`PRO_T_HARNESS`、
`PROMDLSTYPE_FAMTAB_DB`、`ProCmdIsAdded()` 等，**没有一处改动已有签名**。

### 0.4 Creo 13 的未签名 DLL 拦截

Creo 13 起，加载没有数字签名的 TOOLKIT DLL 会弹一个模态框：

```
This application is from an unknown source. Do you want to run this application, cn_step?
```

不点掉就一直卡着，插件不会加载。配置项是 `open_protk_unsigned_apps`：

```
open_protk_unsigned_apps always     # Prompt（默认）/ Never / Always
```

写进 config.pro 就不再问。**只对 Creo 13 有效**，老版本会忽略这个未知选项。

### 0.5 自己复现这套结论

```bat
build.bat 8      :: -> dist_c8\cn_step_c8.dll     ← 通用版，8~13 都能用
build.bat 13     :: -> dist_c13\cn_step_c13.dll   ← 只能在 13 上用
build.bat        :: -> dist\  （用本机最新 SDK）
```

```powershell
# 拿 c8 编的 DLL 去 Creo 13 上跑
.\test_one.ps1 -Version 13 -Install 13.4.1.0 -DllVersion 8
```

`test_one.ps1` 会无值守启动指定的 Creo、等 `cn_step_load.log` 出现、读完自动退出并清理进程。
`test_matrix.py` 做的是纯 PE 层的 6×6 交叉加载（不受 Creo 版本校验影响，只用来看依赖是否齐）。

---

## 一、根因：为什么 Creo 导出中文名会失败

| 层 | 行为 | 结论 |
|---|---|---|
| `intf3d` 翻译器（`ProIntf3DFileWrite*` 底层） | 按 **ANSI 窄字符**解释路径，用当前代码页转换 | 中文路径在这里就已经错了 |
| `creo_less_restrictive_names yes` | 只放开**模型名**的字符校验 | 对导出文件名无效 |
| `Pro/TOOLKIT` 的 `ProPath` | 是 `wchar_t[]`，中文能正确传入 | 但传进去以后翻译器仍按窄字符处理 |

所以**没有任何配置项能让 Creo 直接吐出中文名 STEP**。唯一可靠的做法是接管流程：

```
   ① 用纯 ASCII 基名导出          ② 内存里改写 FILE_NAME        ③ Win32 改中文名
   ─────────────────────          ─────────────────────         ─────────────────
   ProIntf3DFileWrite             '默认名'      ──►  '\X2\8F74627F5EA7\X0\'
   WithDefaultProfile             ISO 10303-21 的 \X2\ UTF-16BE 编码
   输出 _tmp_cn_step_12345.stp                                    MoveFileExW
                                                                 → 轴承座.stp
```

第 ② ③ 步完全走本工程自己的 UTF-16 代码，不经过 Creo 的 ANSI 层，所以不会乱码、不会失败。

---

## 二、当前状态（诚实版）

| 项目 | 状态 |
|---|---|
| 源码按本机 Creo 13.4 SDK 真实头文件逐个 API 核对 | ✅ 已完成 |
| 编译 + 链接（`cl` / `link`，`/W3` 下零 warning 零 error） | ✅ 已实测 |
| 导出符号 `user_initialize` / `user_terminate`，x64 | ✅ `dumpbin` 已确认 |
| STEP 头改写算法（中文转义、引号/反斜杠、边界情况） | ✅ 单测全通过，见 `test.bat` |
| 装配体内部 `PRODUCT` 名改写（含 GBK 规整、去重、幂等） | ✅ 单测全通过 |
| 真实 Creo 导出件实测（10KB 小装配 + 50MB 大装配） | ✅ 见下 |
| **在 Creo 里实际点菜单跑通** | ⚠️ **未做** —— 需要你在 Creo 里注册后试一次 |

真实文件实测数据（`test.bat` 的 `[4]` 段，用你桌面上的 `11.stp` 副本）：

```
renamed 4 products
50MB 装配体：renamed 1 products，耗时 140 ms
reverse-patch equals original: True   ← 把中文换回原名后与原文件逐字节一致
```

最后一行是关键：证明改动精确到字节，几何数据一个字节都没动。
最后一项（真在 Creo 里点）必须由你确认，因为那要真的启动 Creo 加载 DLL。
第一次跑如果报错，按第八节的表对照，基本都能定位。

---

## 三、环境（本机已装好，无需再装）

| 组件 | 版本 / 路径 |
|---|---|
| Creo Parametric | `C:\Program Files\PTC\Creo 12.4.1.0` 或 `...\Creo 13.4.1.0`（两版都验证过） |
| Pro/TOOLKIT SDK | 同目录 `Common Files\protoolkit`（430 / 433 个头文件，`x86e_win64\obj` 库齐全） |
| Visual Studio BuildTools 2022 | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`，MSVC `14.44.35207` |
| Windows SDK | `C:\Program Files (x86)\Windows Kits\10`，`10.0.26100.0` |

链接用的库（官方 `make_basic` 同款）：

```
protk_dll_NU.lib   ucore.lib   udata.lib
```

编译宏也照抄官方 makefile，**缺一不可**：

```
/DPRO_MACHINE=36 /DPRO_OS=4 /D_WSTDIO_DEFINED /DPRO_USE_VAR_ARGS
```

---

## 四、编译

```bat
build.bat        :: 用本机最新 Creo 的 SDK 编译，产物放 dist\
build.bat 12     :: 指定用 Creo 12 的 SDK，产物放 dist_c12\cn_step_c12.dll
build.bat 13     :: 指定用 Creo 13 的 SDK，产物放 dist_c13\cn_step_c13.dll
test.bat         :: 编译并运行算法自检（不依赖 Creo，可随时跑）
build_tool.bat   :: 编译独立命令行工具 build\step_cn.exe
```

`build.bat` 会自己找三样东西，找不到才报错：Creo（含 `ProToolkit.h` 的那个目录）、
MSVC 2022、Windows SDK。也可以用环境变量指定 Creo 根目录：

```bat
set CN_STEP_CREO=D:\PTC\Creo 12.4.1.0
build.bat 12
```

> 探测 Creo 时**不要**写成 `for /d %%D in ("C:\Program Files\PTC\Creo !VER!*")`。
> cmd 在集合参数里遇到含空格的通配符会静默匹配失败（会返回一个当前目录下的假路径）。
> 脚本里改成枚举 `PTC\*` 再自己比名字前 5 个字符，见 `:try_creo`。

`test.bat` 跑四段，全部不依赖 Creo：

```
[1] p21_encode                ISO 10303-21 转义规则
[2] rewrite_step_header       FILE_NAME 改写与边界
[3] rewrite_step_products     装配体 PRODUCT 名（含 GBK 规整、去重、幂等）
[4] real Creo assembly        真实导出件冒烟（从桌面 11.stp 取副本，没有就 SKIP）
```

自检程序同时是个命令行后处理器，可以单独跑，不需要装进 Creo：

```bat
build\cn_step_selftest.exe <文件.stp> [模型名] [顶层中文名]
```

`build.bat` 有两个刻意的设计，改脚本时别踩回去：

1. **不调用 `vcvars64.bat`**，改为手工拼 `INCLUDE` / `LIB`。
   原因：`vcvars64.bat` 内部要调 `reg.exe` 探测 VS 安装，在受限环境里会被拦；
   而且它路径里带 `(x86)`，在嵌套里容易被 cmd 当成括号结束符。
2. **所有 `( )` 块内只用 `!VAR!`，绝不用 `%VAR%`**。
   因为 `%VS%` 展开后含 `(x86)`，其中的 `)` 会提前闭合 `if (...)` 块，
   cmd 随后在 `\Microsoft` 处报「此时不应有 \Microsoft」。

产物：

```
dist\                （build.bat 12 时是 dist_c12\，DLL 名带 _c12）
├─ cn_step.dll          ← 主程序
├─ protk.dat            ← 构建时按绝对路径生成，直接可用
├─ cn_step.cfg          ← 配置
├─ 名称对照.csv          ← 示例，按需改
└─ text\
   ├─ chinese_cn\msg_cn_step.txt    ← 中文界面文本
   └─ usascii\msg_cn_step.txt       ← 英文界面文本
```

> 消息文件是 PTC 的 4 行格式（`键名` / `显示文本` / `#` / `#`），UTF-8 带 BOM。
> 格式抄自 `protk_appls/pt_autoaxis/text/usascii/usrmsg.txt`，不要改成 `%%` 那种老格式。

---

## 五、注册到 Creo

### 方式 A：config.pro（推荐）

在 `config.pro` 里加一行，指向生成的注册文件：

```
toolkit_registry_file  C:\Users\Administrator\WorkBuddy\2026-10-20-14-05-22\creo_toolkit_cn_step\dist\protk.dat
```

### 方式 B：放到工作目录

把 `dist\protk.dat` 复制到**启动 Creo 时所在的工作目录**，Creo 会自动读取。

`dist\protk.dat` 内容（`build.bat` 已按真实路径生成）：

```
NAME       cn_step
EXEC_FILE  ...\dist\cn_step.dll
TEXT_DIR   ...\dist\text
STARTUP    dll
REVISION   18
ALLOW_STOP TRUE
END
```

`REVISION 18` 抄自 SDK 自带的 `protk.dat` 样例。**Creo 12.4.1.0 和 13.4.1.0 都是 18**，
所以同一个 `protk.dat` 两版都能用。以后再升大版本时若报「revision 不匹配」，回来改这个数字。

重启 Creo 后，菜单栏「**工具**」右侧会出现「**导出中文STEP**」，下面一项
「**导出中文 STEP（当前模型）**」。点它即可。

---

## 六、配置 `cn_step.cfg`

程序从**当前工作目录**读 `cn_step.cfg`；找不到就用默认值（输出到当前目录、文件名用模型名）。

```ini
outdir=D:\cn_step_out                              # 输出目录，不存在会自动建
mapping=D:\cn_step_out\名称对照.csv                  # 可选，不存在就跳过
rewrite_header=1                                   # 1=改写 STEP 头 FILE_NAME
rewrite_products=1                                 # 1=改写装配体内部 PRODUCT 名
creo_copy=1                                        # 1=额外生成 <名字>_creo.stp 给 Creo 回读
encoding=utf8                                      # 对照表编码 utf8 / ansi
```

导出成功后弹窗会告诉你落盘路径、头部改成了什么，以及内部改了几个组件名。

### 6.0 为什么会输出两份文件（重要）

实测结论，2026-10-21 用 SolidWorks 2026 与 Creo 13.4 双向验证：

| 编码方式 | SolidWorks 导入 | Creo 导入 |
|---|---|---|
| `\X2\` 转义（ISO 10303-21 标准） | ✅ 显示中文 | ❌ 变 `_X2_58EB..._X0_`（反斜杠被换成下划线） |
| UTF-8 原始字节 | ❌ 变 `______` | ✅ 显示中文 |

两种软件的 STEP 导入器对中文的处理**正好相反**，而且都不合标准（标准只允许 `\X2\`）。
因此一份文件不可能两边都对：

- `<名字>.stp` —— 标准 `\X2\` 转义，给 **SolidWorks / 其他标准工具链**
- `<名字>_creo.stp` —— UTF-8 原始字节，给 **Creo 回读**

`_creo.stp` 里额外做了两件事：把 `\X2\` 全部转成 UTF-8 字节；把 NAUO 的
description 清空。原因是 Creo 导入时会把 description 也当作名字来源，与 PRODUCT
撞车就多一个 `_1` 后缀（id / name / description 三处都写中文会变成 `_1_1`）。

**注意**：`_creo.stp` 导入 Creo 后组件名仍会带一个 `_1`（如 `士大夫_1.PRT`）。
那是 Creo 给导入元件的实例编号，与文件内容无关，文件层面消不掉 —— 就像 SolidWorks
里元件显示为 `士大夫-1` 一样，是实例命名，不是文件名。想彻底干净，导入后在 Creo 里
用「重命名」改一次即可。

### 6.1 装配体内部组件名的中文从哪来

这一节是本工程最容易被误解的地方，先看真实数据的结构。Creo 导出装配体时写成这样：

```
#21=PRODUCT('000','000','NOT SPECIFIED',(#20));
#22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#21,.MADE.);
#23=PRODUCT_DEFINITION('design','',#22,#19);
#29=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship',
                '\X2\58EB5927592B\X0\',#119,#23,$);
```

注意 `#29` 的第三个参数：`\X2\58EB5927592B\X0\` 是 `宇大夫` 的 UTF-16BE 转义。
也就是说 **Creo 其实能写中文** —— 你在装配里给组件起的「组件名称」，它完整写进了 NAUO。

但装配树里显示的名来自 `PRODUCT`（`#21`），那里面还是 `000`。
中文放错了地方，这就是「只改文件名、组件名改不了」的真正原因。

程序顺着 `NAUO.description → PRODUCT_DEFINITION → FORMATION → PRODUCT`
这条引用链把中文回填到 `PRODUCT` 的 id 和 name 上，所以：

| 场景 | 结果 |
|---|---|
| 在 Creo 里给组件起过中文「组件名称」 | ✅ 自动变中文，**不需要填表** |
| 组件名也是英文型号（如 `H20-20-0010001_101`） | 查 `名称对照.csv`；表里也没有就保持原样 |
| 顶层装配，模型本身是中文名 | ✅ 自动用模型名 |
| 顶层装配，模型是英文名 | 查对照表，或保持原样 |

对第二种情况，`名称对照.csv` 长这样（第一列是 **PRODUCT 的 id**，也就是模型名）：

```csv
导出名,中文名
H20-20-0010001_101,底座
H20-20-0020001_134,立柱
```

程序不会瞎猜：查不到就原样保留，不会拿 `NOT SPECIFIED` 之类的东西凑数。

另外两个细节已处理：

- **GBK 字节会规整**。如果 Creo 把中文按 ANSI 写成了原始字节，会解码后重新转成 `\X2\`。
  已经是 `\X2\` 的不会被二次转义（那会坏成 `\\X2\\`，单测里专门盯着这条）。
- **重名会自动加后缀**。两个零件中文名撞了会变成 `轴承座` 和 `轴承座_2`，
  否则导入端会把两个零件合并或直接报错。
- **`SHAPE FOR <名>.` 也跟着换**。这是 PTC 自己的描述约定，有些导入器会拿它当零件名。

改完的实际效果（拿真实导出件跑的）：

```
#21=PRODUCT('\X2\58EB5927592B\X0\','\X2\58EB5927592B\X0\','NOT SPECIFIED',(#20));   宇大夫
#24=PRODUCT_DEFINITION_SHAPE('','SHAPE FOR \X2\58EB5927592B\X0\.',#23);
```

> `Placement of 000 with respect to 0005_ASM` 这种描述性文本保持原样 —— 它只给人看，
> 不参与任何命名，改它没有收益反而增加出错面。

---

## 七、排错

| 现象 | 原因 / 处理 |
|---|---|
| 菜单里看不到「导出中文STEP」 | `protk.dat` 没被读到。检查 `toolkit_registry_file` 路径、`EXEC_FILE` 是否指向真实 dll；看 Creo 启动目录下的 `trail.txt.*` 和「工具 → 服务器」日志 |
| 菜单不出现，trail 里写 `status is -98` + `requires Creo 13; the current version is Creo 12` | **DLL 版本高于 Creo**。换成用本机 Creo 版本（或更老版本）编的 DLL，见第零章。用 `build.bat 8` 编一份可通吃 |
| Creo 13 弹「This application is from an unknown source」卡住 | 未签名 DLL 拦截。config.pro 加 `open_protk_unsigned_apps always`，见 0.4 |
| 启动时提示 revision 不匹配 | 改 `protk.dat` 里的 `REVISION`（Creo 8~13 全是 18，更高版本按 SDK 里 `protk.dat` 样例填） |
| 弹窗「没有打开的模型」 | 先打开一个零件再点 |
| 弹窗「当前模型不是零件或装配体」 | 当前开的是**工程图**（或其他类型）。STEP 导出要的是 3D 模型，切回对应零件/装配体 |
| 文件生成了但文件名是 ASCII | `MoveFileExW` 改名失败，看弹窗错误码：`5`=权限不足，`32`=文件被占用，`3`=路径不存在 |
| 文件名中文了，装配树里组件还是英文 | ①先确认 `dist\cn_step.dll` 的时间戳是最新的（`dist` 和 `build` 里的应字节一致，`build.bat` 会报 `[WARN] ... locked` 就是没替换成功）②组件在 Creo 里没有中文「组件名称」，且对照表里也没有对应项。见 6.1 |
| 弹窗说「内部组件名已改写：0 个」 | 整个装配都没有中文来源。要么去 Creo 里起中文组件名，要么填 `名称对照.csv` |
| 弹窗说「没找到 FILE_NAME 记录」 | 头部被改写过一次或文件非标准 STEP；文件名仍然是对的 |
| `build.bat` 报 `[WARN] ... it is locked` | Creo 正开着并加载了旧 DLL。关掉 Creo 再跑一次 `build.bat`，新 DLL 已在 `build\` 里 |
| 编译报 `C1083: windows.h` | `INCLUDE` 没设对。确认 `SDKI` 指向的是**版本号目录**（如 `...\Include\10.0.26100.0`），不是 `Include` 本身 |
| 编译报 `cannot open ProToolkit.h` | Creo 安装时没勾 Pro/TOOLKIT；或 `CREO` 变量指向空壳目录（本机 `13.4.0.0` 是空的，要用 `13.4.1.0`） |
| 想确认插件到底有没有被加载 | 看 DLL 同目录的 `cn_step_load.log`。每次 `user_initialize` 都会追加一行：时间、PID、Creo 自报版本、三个返回码（0 = 成功） |

---

## 八、支持范围

**零件和装配体都支持**，工程图不支持。

依据在 `ProObjects.h`：

```c
typedef void*            ProMdl;
typedef struct sld_part* ProSolid;      /* 零件和装配体是同一个类型 */
typedef struct sld_part* ProAssembly;   /* 同上 */
typedef struct sld_part* ProPart;       /* 同上 */
typedef struct drawing*  ProDrawing;    /* 这个才是另一种类型 */
```

也就是说 `(ProSolid)model` 对装配体完全合法，不是"类型不对硬塞"。
程序里加了 `ProMdlTypeGet` 校验，只放行 `PRO_MDL_PART` / `PRO_MDL_ASSEMBLY`，
遇到工程图会直接拦下并提示切回模型。

装配体导出成**单文件还是多文件**，由 Creo 里的**默认导出配置文件**决定
（对应 6 参数老接口里的 `ProOutputAssemblyConfiguration`：
`PRO_OUTPUT_ASSEMBLY_SINGLE_FILE` / `MULTI_FILES` / `FLAT_FILE` / `PARTS`）。
本插件用的是 3 参数版本，也就是"按当前默认配置来"，所以要改装配体导出行为，
请先去 Creo 里把 STEP 导出配置文件设好。

> **多文件导出的限制（实话）**：本插件只处理它拿到的那个主文件。
> 如果你把 Creo 的导出配置改成"每个零件单独一个文件"，那些分出去的子文件
> 不会被改名，也不会改内部 PRODUCT 名。默认配置是单文件（实测 50MB 装配体
> 的 14 个 PRODUCT 全在一个 .stp 里），所以一般不会碰到。

## 九、其他说明

- 用的是 `ProIntf3DFileWriteWithDefaultProfile`，**它不是 deprecated**。
  反而 6 参数的 `ProIntf3DFileWrite` 从 Creo 5.0 起就标了 DEPRECATED，
  官方给的替代品正是这个 3 参数接口。所以这里选的是当前推荐写法。
- 结果提示走 `MessageBoxW` 而不是 `ProMessageDisplay`。后者收窄字符，中文要经代码页转换，
  容易出乱码；前者是 UTF-16，什么界面语言都正确。
- 导出用**默认导出 profile**（不弹对话框），想要特定 STEP 协议（AP203/AP214）
  要在 Creo 里先设好默认导出配置。

---

## 十、独立工具 `step_cn.exe`（不装 Creo 也能用，推荐）

插件有个绕不过去的限制：**Pro/TOOLKIT 的 DLL 与 Creo 大版本绑定**，Creo 13 编的
DLL 塞不进 Creo 12。而真正处理中文名的逻辑其实一行 Creo API 都不需要——它只读写
STEP 文本。

所以同一份 `src/cn_step.c` 还能编出一个**完全独立**的命令行工具：

```bat
build_tool.bat            :: 产出 build\step_cn.exe（/MT 静态链接，目标机无需 VC 运行库）
```

```bat
step_cn.exe <文件.stp> [顶层中文名] [选项]

  --map <文件.csv>   名称对照表，两列：导出名,中文名
  --out <目录>       输出目录，默认与输入同目录
  --no-creo          不生成 _creo 版
  --ansi             对照表按 GBK 读，默认 UTF-8
  --diag             只打印 STEP 索引统计，不改文件（排查用）
  --quiet            只输出错误
```

把文件**直接拖到 exe 图标上**也能用（等同只传文件参数）。

行为：

| 情况 | 结果 |
|---|---|
| STEP 里已有中文（NAUO 描述） | 回填到 PRODUCT，输出 `<名字>.stp` + `<名字>_creo.stp` |
| 没有中文，但给了第二参数 | 用它当顶层装配名，组件仍按 NAUO/对照表 |
| 都没有 | 保持原名，只做格式转换（生成 `_creo` 版） |

实测：50 MB、14 个零件的装配 235 ms；改完把中文还原后与原文件**逐字节相同**。

**为什么不改 Creo 底层**：`intf3d` 是编译好的二进制，无源码可改；而且中文名显示
不了不是 Creo 导出的问题（它写的 `\X2\` 完全合标准），是下游导入器的读取问题。
改 Creo 只会破坏标准，解决不了跨软件。真正的解法就是这一层 STEP 后处理。

---

## 十一、不想装插件？还有纯 Python 方案

同目录的 `../creo_step_cn/` 是一个不依赖 Creo 的后处理工具：
先用 Creo 正常导出（ASCII 名也行），再用它批量改成中文名并改写头。

```bat
启动.bat        :: 图形界面，可先 dry-run 预览
```

适合只想偶尔转几个文件、不愿动 Creo 配置的场景。
