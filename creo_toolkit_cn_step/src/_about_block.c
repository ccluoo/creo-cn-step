/* ------------------------------------------------------------------ 关于落落
 * 自绘弹出窗口：不使用对话框模板，也不创建子控件。
 *
 * 旧实现用内存对话框模板 + DialogBoxIndirectParamW。模板必须手工按 DWORD
 * 逐字节对齐拼装，只要有一处偏移算错，Creo 就闪退或者干脆不显示——这正是
 * 「点了没反应 / 一闪就没了」的来源。改成「一个自绘窗口 + 自己的消息循环」
 * 后完全绕开模板；万一窗口都建不出来，还会退化成 MessageBoxW，
 * 保证任何情况下点了都有内容出来。
 *
 * 二维码从 DLL 同目录的 luoluo_qr.bmp 读取（LoadImageW，零依赖）。
 * -------------------------------------------------------------------- */

#define ABT_CW      380          /* 96 dpi 下的客户区宽 */
#define ABT_CH      520          /* 96 dpi 下的客户区高 */
#define ABT_QRS     180          /* 二维码显示边长 */

#define ABT_C_TITLE RGB(0x1a, 0x1f, 0x26)
#define ABT_C_HEAD  RGB(0x53, 0x5b, 0x66)
#define ABT_C_BODY  RGB(0x33, 0x39, 0x40)
#define ABT_C_RED   RGB(0xc0, 0x00, 0x00)
#define ABT_C_LINE  RGB(0xd8, 0xdc, 0xe2)
#define ABT_C_BTN   RGB(0x2f, 0x6b, 0xd8)

static HFONT   g_abt_font, g_abt_font_b, g_abt_font_big;
static HBITMAP g_abt_qr;
static HWND    g_abt_hwnd;
static int     g_abt_k = 100;    /* DPI 缩放百分比（96 dpi = 100） */
static RECT    g_abt_btn;        /* 确定按钮矩形，客户区坐标 */

static int abt_s(int v) { return MulDiv(v, g_abt_k, 100); }

static HINSTANCE cn_hinstance(void)
{
    HMODULE hm = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&cn_hinstance, &hm))
        return NULL;
    return (HINSTANCE)hm;
}

/* 读 DLL 同目录的 luoluo_qr.bmp；找不到返回 NULL，二维码区域留空 */
static HBITMAP cn_load_qr(void)
{
    wchar_t mod[MAX_PATH * 2], path[MAX_PATH * 2], *slash;
    HMODULE hm = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&cn_load_qr, &hm))
        return NULL;
    if (!GetModuleFileNameW(hm, mod, MAX_PATH * 2))
        return NULL;
    wcscpy_s(path, MAX_PATH * 2, mod);
    slash = wcsrchr(path, L'\\');
    if (!slash) return NULL;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH * 2 - (slash + 1 - path)),
             L"luoluo_qr.bmp");
    return (HBITMAP)LoadImageW(NULL, path, IMAGE_BITMAP, 0, 0,
                               LR_LOADFROMFILE | LR_CREATEDIBSECTION);
}

static void abt_fonts_make(void)
{
    int px = abt_s(13);
    g_abt_font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_abt_font_b = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                               DEFAULT_CHARSET, OUT_TT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_abt_font_big = CreateFontW(-abt_s(17), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Microsoft YaHei UI");
}

static void abt_fonts_free(void)
{
    if (g_abt_font)     { DeleteObject(g_abt_font);     g_abt_font = NULL; }
    if (g_abt_font_b)   { DeleteObject(g_abt_font_b);   g_abt_font_b = NULL; }
    if (g_abt_font_big) { DeleteObject(g_abt_font_big); g_abt_font_big = NULL; }
}

static void abt_text(HDC dc, HFONT f, COLORREF col,
                     int x, int y, int w, int h, const wchar_t *s)
{
    HGDIOBJ old;
    RECT r;
    if (!f || !s) return;
    old = SelectObject(dc, f);
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    SetTextColor(dc, col);
    DrawTextW(dc, s, -1, &r,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
              DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

static void abt_hline(HDC dc, int x1, int y, int x2)
{
    RECT r;
    HBRUSH b = CreateSolidBrush(ABT_C_LINE);
    r.left = x1; r.top = y; r.right = x2; r.bottom = y + 1;
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void abt_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    int W, H, m, qx, qy, bw;
    HBRUSH bg;

    GetClientRect(hwnd, &rc);
    W = rc.right;
    H = rc.bottom;
    m = abt_s(20);
    (void)H;

    bg = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    /* 标题 + 分隔线 */
    abt_text(dc, g_abt_font_big, ABT_C_TITLE, m, abt_s(14), W - 2 * m,
             abt_s(28), L"落落工具  ·  Export CN STEP");
    abt_hline(dc, m, abt_s(52), W - m);

    /* 参考文献 */
    abt_text(dc, g_abt_font_b, ABT_C_HEAD, m, abt_s(62), W - 2 * m,
             abt_s(20), L"参考文献");
    abt_text(dc, g_abt_font, ABT_C_BODY, m, abt_s(84), W - 2 * m,
             abt_s(20), L"ISO 10303-21   STEP 产品数据交换标准");
    abt_text(dc, g_abt_font, ABT_C_BODY, m, abt_s(104), W - 2 * m,
             abt_s(20), L"PTC Pro/TOOLKIT User's Guide");
    abt_text(dc, g_abt_font, ABT_C_BODY, m, abt_s(124), W - 2 * m,
             abt_s(20), L"PTC Creo Parametric 8.0 – 13.0 接口文档");

    /* 作者与联系方式 */
    abt_text(dc, g_abt_font_b, ABT_C_TITLE, m, abt_s(154), W - 2 * m,
             abt_s(20), L"本插件由 WorkBuddy 制作");
    abt_text(dc, g_abt_font, ABT_C_BODY, m, abt_s(176), W - 2 * m,
             abt_s(20), L"全为兴趣开发，有想要的功能请扫码联系");

    /* 二维码 */
    qx = (W - abt_s(ABT_QRS)) / 2;
    qy = abt_s(204);
    if (g_abt_qr) {
        BITMAP bm;
        HDC mdc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mdc, g_abt_qr);
        if (GetObjectW(g_abt_qr, sizeof(bm), &bm)) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, qx, qy, abt_s(ABT_QRS), abt_s(ABT_QRS),
                       mdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        }
        SelectObject(mdc, old);
        DeleteDC(mdc);
    } else {
        RECT r;
        HBRUSH b = CreateSolidBrush(RGB(0xf2, 0xf4, 0xf7));
        r.left = qx; r.top = qy;
        r.right = qx + abt_s(ABT_QRS);
        r.bottom = qy + abt_s(ABT_QRS);
        FillRect(dc, &r, b);
        DeleteObject(b);
        abt_text(dc, g_abt_font, ABT_C_HEAD, qx, qy + abt_s(80),
                 abt_s(ABT_QRS), abt_s(20), L"二维码未找到");
    }

    /* 免责声明（红字） */
    abt_text(dc, g_abt_font, ABT_C_RED, m, abt_s(394), W - 2 * m,
             abt_s(20), L"1. 使用前请充分了解本插件功能，并自行备份数据后测试。");
    abt_text(dc, g_abt_font, ABT_C_RED, m, abt_s(414), W - 2 * m,
             abt_s(20), L"2. 因使用本插件造成的数据丢失或损失，作者不承担责任。");

    /* 确定按钮（自绘） */
    bw = abt_s(96);
    SetRect(&g_abt_btn, (W - bw) / 2, abt_s(444),
            (W - bw) / 2 + bw, abt_s(474));
    {
        HBRUSH bb = CreateSolidBrush(ABT_C_BTN);
        HGDIOBJ ob = SelectObject(dc, bb);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, g_abt_btn.left, g_abt_btn.top,
                  g_abt_btn.right, g_abt_btn.bottom, abt_s(10), abt_s(10));
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(bb);
    }
    {
        HGDIOBJ old = SelectObject(dc, g_abt_font_b);
        RECT br = g_abt_btn;
        SetTextColor(dc, RGB(0xff, 0xff, 0xff));
        DrawTextW(dc, L"确  定", -1, &br,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK abt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        abt_paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        if (x >= g_abt_btn.left && x < g_abt_btn.right &&
            y >= g_abt_btn.top  && y < g_abt_btn.bottom)
            DestroyWindow(hwnd);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (PtInRect(&g_abt_btn, pt))
            SetCursor(LoadCursorW(NULL, IDC_HAND));
        else
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return TRUE;
    }

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN || wp == VK_SPACE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        /* 自己销毁后要把消息循环叫醒，否则 GetMessage 会一直挂着 */
        g_abt_hwnd = NULL;
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 建窗口 + 跑模态消息循环。失败返回 0，调用方退回 MessageBox。 */
static int abt_run(void)
{
    static int reg = 0;
    WNDCLASSEXW wc;
    RECT wa, orc, box;
    HDC sdc;
    int dpi, x, y, w, h;
    HWND owner;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD exstyle = WS_EX_TOPMOST | WS_EX_DLGMODALFRAME;
    MSG msg;

    sdc = GetDC(NULL);
    dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSY) : 96;
    if (sdc) ReleaseDC(NULL, sdc);
    if (dpi <= 0) dpi = 96;
    g_abt_k = MulDiv(dpi, 100, 96);
    if (g_abt_k < 100) g_abt_k = 100;

    if (!reg) {
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = abt_proc;
        wc.hInstance = cn_hinstance();
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"CnStepAboutWnd";
        if (!RegisterClassExW(&wc)) return 0;
        reg = 1;
    }

    abt_fonts_make();
    g_abt_qr = cn_load_qr();

    box.left = 0; box.top = 0;
    box.right = abt_s(ABT_CW);
    box.bottom = abt_s(ABT_CH);
    AdjustWindowRectEx(&box, style, FALSE, exstyle);
    w = box.right - box.left;
    h = box.bottom - box.top;

    owner = GetForegroundWindow();
    if (owner) {
        GetWindowRect(owner, &orc);
        x = orc.left + ((orc.right - orc.left) - w) / 2;
        y = orc.top + ((orc.bottom - orc.top) - h) / 2;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + ((wa.right - wa.left) - w) / 2;
        y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    g_abt_hwnd = CreateWindowExW(exstyle, L"CnStepAboutWnd", L"关于落落工具",
                                 style, x, y, w, h, owner, NULL,
                                 cn_hinstance(), NULL);
    if (!g_abt_hwnd) {
        abt_fonts_free();
        if (g_abt_qr) { DeleteObject(g_abt_qr); g_abt_qr = NULL; }
        return 0;
    }

    ShowWindow(g_abt_hwnd, SW_SHOW);
    UpdateWindow(g_abt_hwnd);
    SetForegroundWindow(g_abt_hwnd);
    SetFocus(g_abt_hwnd);

    while (g_abt_hwnd) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0) { PostQuitMessage(0); break; }
        if (r == (BOOL)-1) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    abt_fonts_free();
    if (g_abt_qr) { DeleteObject(g_abt_qr); g_abt_qr = NULL; }
    return 1;
}

static void abt_fallback(void)
{
    MessageBoxW(NULL,
        L"落落工具  ·  Export CN STEP\n\n"
        L"参考文献：\n"
        L"  ISO 10303-21   STEP 产品数据交换标准\n"
        L"  PTC Pro/TOOLKIT User's Guide\n"
        L"  PTC Creo Parametric 8.0 – 13.0 接口文档\n\n"
        L"本插件由 WorkBuddy 制作\n"
        L"全为兴趣开发，有想要的功能请扫码联系"
        L"（二维码见插件目录 luoluo_qr.bmp）\n\n"
        L"免责声明：\n"
        L"1. 使用前请充分了解本插件功能，并自行备份数据后测试。\n"
        L"2. 因使用本插件造成的数据丢失或损失，作者不承担责任。",
        L"关于落落工具", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

static int cn_show_about(void)
{
    if (abt_run()) return 1;
    abt_fallback();
    return 0;
}

static int cn_about_action(uiCmdCmdId command, uiCmdValue *p_value,
                           void *p_data)
{
    (void)command; (void)p_value; (void)p_data;
    cn_show_about();
    return 0;
}

/* 供外部单独验证对话框（不经 Creo，ctypes 直接调） */
int cn_about_test(void)
{
    return cn_show_about();
}
