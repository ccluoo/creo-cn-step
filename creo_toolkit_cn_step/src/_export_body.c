    /* 中文名：对照表优先，否则就用模型名 */
    wchar_t wcn[PRO_MDLNAME_SIZE + 8];
    if (!mapping_lookup(mdl_name, wcn, (int)(sizeof(wcn) / sizeof(wchar_t)))) {
        wcscpy_s(wcn, sizeof(wcn) / sizeof(wchar_t), mdl_name);
    }

    /* ---------------- 1) 建议文件名：中文名_YYMMDD.stp ---------------- */
    wchar_t stamp[16];
    cn_date_stamp(stamp, 16);

    wchar_t wdefdir[MAX_PATH];
    if (g_cfg.outdir[0] &&
        GetFileAttributesW(g_cfg.outdir) != INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(wdefdir, MAX_PATH, g_cfg.outdir);
    } else if (!GetCurrentDirectoryW(MAX_PATH, wdefdir)) {
        wcscpy_s(wdefdir, MAX_PATH, L"C:\\");
    }

    wchar_t wsave[MAX_PATH * 2];
    _snwprintf_s(wsave, MAX_PATH * 2, _TRUNCATE, L"%s\\%s_%s.stp",
                 wdefdir, wcn, stamp);

    /* ---------------- 2) 让用户选保存位置 ---------------- */
    if (!cn_pick_save_path(wsave, (DWORD)(MAX_PATH * 2)))
        return 0;                       /* 用户点了取消，安静退出 */

    /* 规范化：扩展名不是 stp/step 就补 .stp；再算出另一份的名字 */
    wchar_t wdir[MAX_PATH], wstem[256], wext[32], wstem2[320];
    wchar_t wother[MAX_PATH * 2];

    cn_split_path(wsave, wdir, MAX_PATH, wstem, 256, wext, 32);
    if (_wcsicmp(wext, L"stp") != 0 && _wcsicmp(wext, L"step") != 0)
        wcscat_s(wsave, MAX_PATH * 2, L".stp");

    cn_other_name(wstem, wstem2, 320);
    _snwprintf_s(wother, MAX_PATH * 2, _TRUNCATE, L"%s\\%s.stp",
                 wdir, wstem2);

    /* ---------------- 3) 先导到临时目录 ----------------
     * ProIntf3DFileWrite 对非 ASCII 路径不稳（用户选的目录很可能带中文），
     * 所以先落到系统临时目录的 ASCII 基名，再搬到目标位置。 */
    wchar_t wtmpdir[MAX_PATH];
    if (!cn_temp_dir(wtmpdir, MAX_PATH)) {
        say_err(L"取系统临时目录失败，无法导出。");
        return 0;
    }

    /* 官方文档明确 output_file 不要带扩展名，Creo 会自己补，
     * 所以这里传基名，导出后再探测真实落盘文件名。 */
    ProPath wbase;
    _snwprintf_s(wbase, PRO_PATH_SIZE, _TRUNCATE, L"%s%s%lu",
                 wtmpdir, TMP_PREFIX, (unsigned long)GetTickCount());

#ifdef CN_STEP_OLD_SDK
    /* Creo 2 只有 8 参数的 ProIntf3DFileWrite，选项全给默认值 */
    err = ProIntf3DFileWrite((ProSolid)model, PRO_INTF_EXPORT_STEP, wbase,
                             PRO_OUTPUT_ASSEMBLY_SINGLE_FILE,
                             NULL, NULL, NULL, NULL);
#else
    err = ProIntf3DFileWriteWithDefaultProfile((ProSolid)model,
                                              PRO_INTF_EXPORT_STEP, wbase);
#endif
    if (err != PRO_TK_NO_ERROR) {
        say_err(L"STEP 导出失败（返回 %d）\n\n"
                L"排查顺序：\n"
                L"  1. 临时目录是否可写（%s）\n"
                L"  2. 该模型是否有实体几何\n"
                L"  3. 装配体是否含无法解析的缺失元件",
                (int)err, wtmpdir);
        return 0;
    }

    /* 探测 Creo 实际加上的扩展名 */
    ProPath wprobe;
    wcscpy_s(wprobe, PRO_PATH_SIZE, wbase);
    if (GetFileAttributesW(wprobe) == INVALID_FILE_ATTRIBUTES) {
        static const wchar_t *kCand[] = { L".stp", L".step",
                                          L".stp.stp", L".STEP" };
        int found = 0;
        size_t ci;
        for (ci = 0; ci < sizeof(kCand) / sizeof(kCand[0]); ci++) {
            ProPath cand;
            _snwprintf_s(cand, PRO_PATH_SIZE, _TRUNCATE,
                         L"%s%s", wbase, kCand[ci]);
            if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) {
                wcscpy_s(wprobe, PRO_PATH_SIZE, cand);
                found = 1;
                break;
            }
        }
        if (!found) {
            say_err(L"导出接口返回成功，但在临时目录里找不到生成的文件。\n\n"
                    L"基名：%s", wbase);
            return 0;
        }
    }

    /* ---------------- 4) 改写 STEP 内容（针对转义版） ---------------- */
    int header_ok = 0;
    if (g_cfg.rewrite_header)
        header_ok = rewrite_step_header(wprobe, wcn);

    int prod_renamed = 0;
    int prod_ok = 1;
    if (g_cfg.rewrite_products)
        prod_ok = rewrite_step_products(wprobe, mdl_name, wcn,
                                        mapping_lookup, &prod_renamed) ? 1 : 0;

    /* ---------------- 5) 落到两份文件 ----------------
     * ① wother：保留 \X2\ 转义 —— SolidWorks 等按 ISO 10303-21 读，中文正常
     * ② wsave ：\X2\ 还原成 UTF-8 —— Creo 自己的导入器只认这个 */
    if (!MoveFileExW(wprobe, wother,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        say_err(L"文件已导出但搬运失败（错误码 %lu）。\n\n"
                L"临时文件：%s\n目标：%s",
                GetLastError(), wprobe, wother);
        return 0;
    }

    if (!write_creo_variant(wother, wsave)) {
        say_err(L"已生成：\n%s\n\n但 UTF-8 副本写入失败：\n%s\n\n"
                L"请检查目标目录是否可写。", wother, wsave);
        return 0;
    }

    /* ---------------- 6) 收尾提示 ---------------- */
    if (!g_cfg.rewrite_header || !header_ok) {
        say(L"导出完成\n\n"
            L"① Creo 用：\n%s\n\n"
            L"② 其它软件用（SolidWorks 等）：\n%s\n\n"
            L"（注意：STEP 里没找到 FILE_NAME 记录，只改了文件名）",
            wsave, wother);
        return 0;
    }

    say(L"导出完成\n\n"
        L"① Creo 用（UTF-8，中文正常）\n%s\n\n"
        L"② 其它软件用（SolidWorks 等）\n%s\n\n"
        L"文件头 FILE_NAME → %s\n"
        L"装配体内部组件名已改写 → %d 个%s",
        wsave, wother, wcn, prod_renamed,
        prod_ok ? L"" : L"\n（警告：PRODUCT 改写过程出错，文件可能不完整）");
    return 0;
}

