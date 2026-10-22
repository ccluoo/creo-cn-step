# Creo STEP 导出中文 工具箱（落落工具）

一键把当前模型导出成**中文文件名**的 STEP，
并且把装配树里的组件名也改成中文。

Creo 自带的 STEP 导出器不支持中文路径/文件名，本工程用 Pro/TOOLKIT 绕过这个限制,全程使用workbuddy。

- 适用版本：**Creo 8.0 ~ 13.0**（Creo 2.0 因 SDK 兼容成本过高已剔除，部署脚本会 `[skip]`）
- 授权：不修改 Creo 安装目录下的可执行文件，只拷 DLL + 注册 `config.pro` 与 ribbon 配置

## 目录

| 路径 | 说明 |
|---|---|
| `creo_toolkit_cn_step/` | **主工程**：C 源码、构建脚本、部署脚本、Python 工具、详细文档 |
| `creo_step_cn/` | 早期 Python 版工具（不依赖 Pro/TOOLKIT，导出后批量改名改头） |

主工程文档见 [`creo_toolkit_cn_step/README.md`](creo_toolkit_cn_step/README.md)，里面有：

- 中文名两个位置（`FILE_NAME` 头 / `PRODUCT` 实体）为什么必须都改
- Creo 2.0 混链失败的根因（`_MSC_VER=1600` / libcpmt 静态初始化器）
- ribbon 页必须挂到对应模式（`prt_main_adb_cat` / `asm_main_adb_cat` …）的对照表
- 「关于落落」为什么放弃 `DLGTEMPLATE` 改自绘窗口
- 逐版本构建、部署、回归测试的完整步骤

## 快速开始

1. 构建：`creo_toolkit_cn_step\build.bat`（需 VS2022 + 对应版本 Pro/TOOLKIT SDK）
2. 部署：以管理员身份运行 `creo_toolkit_cn_step\release\一键部署.bat`
3. 启动 Creo，在零件/装配模式下点「落落工具」选项卡

部署细节见 `creo_toolkit_cn_step/release/安装说明.txt`。

## 导出命名规则

| 用途 | 文件名 |
|---|---|
| Creo 自己用 | `xxx_261022.stp`（模型名 + __YYMMDD`） |
| 其它软件用 | `xxx_other_261022.stp`（`_other_YYMMDD） |

点「导出中文 STEP」会弹出系统保存对话框，默认名已填好、默认目录取 `cn_step.cfg` 的 `outdir`，
点「保存」才真正开始导出。

## 构建产物不入库

`build/`、`dist*/`、`*.dll`、`*.zip`、测试截图均被 `.gitignore` 排除，仓库只保留源码、
配置模板与文档。
