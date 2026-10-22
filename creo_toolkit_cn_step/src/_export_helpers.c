/* ------------------------------------------------------- 导出路径小工具 */

/* YYMMDD，例如 2026-10-22 -> 261022 */
static void cn_date_stamp(wchar_t *out, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(out, cap, _TRUNCATE, L"%02d%02d%02d",
                 (int)(st.wYear % 100), (int)st.wMonth, (int)st.wDay);
}

/* 把完整路径拆成 目录 / 主名 / 扩展名（扩展名不含点） */
static void cn_split_path(const wchar_t *path,
                          wchar_t *dir, size_t dcap,
                          wchar_t *stem, size_t scap,
                          wchar_t *ext, size_t ecap)
{
    const wchar_t *slash = wcsrchr(path, L'\\');
    const wchar_t *name = slash ? slash + 1 : path;
    const wchar_t *dot = wcsrchr(name, L'.');
    size_t n;

    if (slash) {
        n = (size_t)(slash - path);
        if (n >= dcap) n = dcap - 1;
        wmemcpy(dir, path, n);
        dir[n] = 0;
    } else {
        dir[0] = 0;
    }

    if (dot && dot != name) {
        n = (size_t)(dot - name);
        if (n >= scap) n = scap - 1;
        wmemcpy(stem, name, n);
        stem[n] = 0;
        wcsncpy_s(ext, ecap, dot + 1, _TRUNCATE);
    } else {
        wcsncpy_s(stem, scap, name, _TRUNCATE);
        ext[0] = 0;
    }
}

/* xxxx_261022 -> xxxx_other_261022
 * _other 要插在 6 位日期前面，而不是直接接在末尾 */
static void cn_other_name(const wchar_t *stem, wchar_t *out, size_t cap)
{
    size_t L = wcslen(stem), cut = L;
    if (L > 7 && stem[L - 7] == L'_') {
        size_t i;
        int digits = 1;
        for (i = L - 6; i < L; i++)
            if (stem[i] < L'0' || stem[i] > L'9') { digits = 0; break; }
        if (digits) cut = L - 7;
    }
    _snwprintf_s(out, cap, _TRUNCATE, L"%.*s_other%s",
                 (int)cut, stem, stem + cut);
}

/* 临时目录：优先系统 TEMP；路径里带中文时退回 C:\Windows\Temp */
static BOOL cn_temp_dir(wchar_t *out, size_t cap)
{
    wchar_t t[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, t);
    const wchar_t *p;
    int ascii = 1;

    if (n == 0 || n >= MAX_PATH) return FALSE;
    for (p = t; *p; p++)
        if ((unsigned)*p > 127) { ascii = 0; break; }
    if (!ascii) {
        wcscpy_s(t, MAX_PATH, L"C:\\Windows\\Temp\\");
        if (GetFileAttributesW(L"C:\\Windows\\Temp") == INVALID_FILE_ATTRIBUTES)
            CreateDirectoryW(L"C:\\Windows\\Temp", NULL);
    }
    if (wcslen(t) >= cap) return FALSE;
    wcscpy_s(out, cap, t);
    return TRUE;
}

/* 保存对话框：进入时 path 里已填好「目录\建议文件名」，用户可改可换目录 */
static BOOL cn_pick_save_path(wchar_t *path, DWORD cap)
{
    OPENFILENAMEW ofn;
    static const wchar_t kFilter[] =
        L"STEP 文件 (*.stp;*.step)\0*.stp;*.step\0"
        L"所有文件 (*.*)\0*.*\0"
        L"\0";

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = GetForegroundWindow();
    ofn.lpstrFilter = kFilter;
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = cap;
    ofn.lpstrTitle  = L"导出中文 STEP — 选择保存位置";
    ofn.lpstrDefExt = L"stp";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    return GetSaveFileNameW(&ofn) ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ 主流程 */
