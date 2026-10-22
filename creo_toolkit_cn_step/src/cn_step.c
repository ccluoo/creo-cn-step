/*
 * cn_step.c  -  Creo Parametric TOOLKIT 应用：一键导出中文名 STEP
 * 目标版本：Creo 13.4（API 已对照本机 protoolkit\includes 核实并编译通过）
 *
 * 为什么不用"让翻译器认中文"
 * ------------------------------------------------------------------
 * intf3d 翻译器拿中文输出路径会失败，TOOLKIT 层也绕不过。所以本应用接管流程：
 *   1) 用纯 ASCII 基名调用 ProIntf3DFileWriteWithDefaultProfile 导出
 *      （官方文档要求 output_file 不带扩展名，Creo 自己补）
 *   2) 在内存里把 STEP 头 FILE_NAME 改写成中文（ISO 10303-21 \X2\ 编码）
 *   3) 用 Win32 MoveFileExW 改成中文文件名
 * 第 2、3 步全走自己的 UTF-16 代码，不经过 Creo 的 ANSI 层。
 *
 * 关键类型（Creo 13 起是宽字符，容易踩坑）
 *   ProPath         = wchar_t[]   路径
 *   ProMdlName      = wchar_t[]   模型名
 *   ProFileName     = wchar_t[]   消息文件名
 *   ProMenuItemName = char[]      菜单标识（仍是窄字符！）
 *
 * 自检：定义 CN_STEP_SELFTEST 编译可直接跑算法单测，不依赖 Creo。
 */

/* 这三个宏同时可能由命令行 /D 提供，用 #ifndef 防止 C4005 重定义告警 */
#ifndef PRO_USE_VAR_ARGS
#define PRO_USE_VAR_ARGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#if !defined(CN_STEP_SELFTEST) && !defined(CN_STEP_TOOL)
#include <ProToolkit.h>
#include <ProMdl.h>
#include <ProMenuBar.h>
#include <ProUICmd.h>
#include <ProMessage.h>
#include <ProUtil.h>
#include <ProIntf3Dexport.h>
#include <ProRibbon.h>
#endif

#define MSGFILE    "msg_cn_step.txt"
#define CFG_NAME   L"cn_step.cfg"
#define TMP_PREFIX L"_tmp_cn_step_"

/* ==================================================================
 * 第一部分：纯 Win32 的底层 IO（自检和正式运行都要用）
 * ================================================================== */

static BYTE *read_all_bytes(const wchar_t *path, DWORD *out_len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return NULL; }
    BYTE *buf = (BYTE *)malloc((size_t)size + 2);
    if (!buf) { CloseHandle(h); return NULL; }
    DWORD got = 0;
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) {
        free(buf); CloseHandle(h); return NULL;
    }
    buf[size] = 0;
    buf[size + 1] = 0;
    *out_len = size;
    CloseHandle(h);
    return buf;
}

static BOOL write_all_bytes(const wchar_t *path, const BYTE *data, DWORD len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data, len, &wrote, NULL) && wrote == len;
    CloseHandle(h);
    return ok;
}

/* ------------------------------------------------------- part 21 字符串编码
 * ISO 10303-21 规则：
 *   单引号  -> ''
 *   反斜杠  -> \\
 *   可打印 ASCII 原样
 *   其余字符连续成段，包成 \X2\XXXXXXXX...\X0\（UTF-16BE 十六进制）
 * ------------------------------------------------------------------------ */

static void p21_encode(const wchar_t *src, char *out, size_t cap)
{
    size_t o = 0, i = 0;
    if (cap == 0) return;
    out[0] = 0;
    while (src[i] && o + 16 < cap) {
        wchar_t c = src[i];
        if (c == L'\'') {
            out[o++] = '\''; out[o++] = '\'';
            i++;
        } else if (c == L'\\') {
            out[o++] = '\\'; out[o++] = '\\';
            i++;
        } else if (c >= 0x20 && c <= 0x7E) {
            out[o++] = (char)c;
            i++;
        } else {
            int n = snprintf(out + o, cap - o, "\\X2\\");
            if (n < 0) break;
            o += (size_t)n;
            while (src[i] && !(src[i] >= 0x20 && src[i] <= 0x7E) && o + 8 < cap) {
                n = snprintf(out + o, cap - o, "%04X",
                             (unsigned)(unsigned short)src[i]);
                if (n < 0) break;
                o += (size_t)n;
                i++;
            }
            n = snprintf(out + o, cap - o, "\\X0\\");
            if (n < 0) break;
            o += (size_t)n;
        }
    }
    out[o] = 0;
}

/* 定位 FILE_NAME(...) 第一个单引号字符串的位置，[start, end) 为其内容 */
static BOOL find_file_name_span(const BYTE *buf, DWORD len,
                                DWORD *start, DWORD *end)
{
    const char *needle = "FILE_NAME(";
    size_t nlen = strlen(needle);
    DWORD i;
    for (i = 0; i + nlen < len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) break;
    }
    if (i + nlen >= len) return FALSE;

    DWORD j = i + (DWORD)nlen;
    while (j < len && (buf[j] == ' ' || buf[j] == '\r' ||
                       buf[j] == '\n' || buf[j] == '\t')) j++;
    if (j >= len || buf[j] != '\'') return FALSE;

    DWORD s = j + 1;
    DWORD k = s;
    while (k < len) {
        if (buf[k] == '\'') {
            if (k + 1 < len && buf[k + 1] == '\'') { k += 2; continue; }
            break;
        }
        k++;
    }
    if (k >= len) return FALSE;
    *start = s;
    *end = k;
    return TRUE;
}

static BOOL rewrite_step_header(const wchar_t *path, const wchar_t *cn_name)
{
    DWORD len = 0;
    BYTE *buf = read_all_bytes(path, &len);
    if (!buf) return FALSE;

    DWORD s = 0, e = 0;
    if (!find_file_name_span(buf, len, &s, &e)) { free(buf); return FALSE; }

    char enc[1024];
    p21_encode(cn_name, enc, sizeof(enc));

    DWORD elen = (DWORD)strlen(enc);
    DWORD newlen = len - (e - s) + elen;
    BYTE *out = (BYTE *)malloc((size_t)newlen + 1);
    if (!out) { free(buf); return FALSE; }

    memcpy(out, buf, s);
    memcpy(out + s, enc, elen);
    memcpy(out + s + elen, buf + e, len - e);
    out[newlen] = 0;

    BOOL ok = write_all_bytes(path, out, newlen);
    free(out);
    free(buf);
    return ok;
}

/* ==================================================================
 * 第一部分补充：装配体内部组件名改写
 *
 * 为什么光改 FILE_NAME 不够（用真实 Creo 导出件验证过）：
 *   FILE_NAME 只决定「这个文件叫什么」。STEP 内部的装配树由 PRODUCT 实体
 *   决定，两者毫无关系。Creo 导出装配体时写出来长这样：
 *
 *     #21=PRODUCT('000','000','NOT SPECIFIED',(#20));
 *     #22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#21,.MADE.);
 *     #23=PRODUCT_DEFINITION('design','',#22,#19);
 *     #29=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship',
 *                     '\X2\58EB5927592B\X0\',#119,#23,$);
 *
 *   看 #29 的第三个参数：Creo 把装配里设的中文组件名正确写成了 \X2\ 转义。
 *   中文其实已经在文件里，只是落在 NAUO 的 description 上，而 PRODUCT 的
 *   id / name 仍是 ASCII 模型名。SolidWorks 之类的软件读装配树取的是
 *   PRODUCT 的名字，所以看到的永远是 000。
 *
 * 本模块因此做两件事：
 *   1) 顺着 NAUO.description -> PD -> FORMATION -> PRODUCT 的引用链，
 *      把文件里已有的中文名回填到 PRODUCT 的 id 和 name；
 *   2) NAUO 里没有中文时，退回 cn_step.cfg 指定的名称对照表。
 * 两者都拿不到就保持原样，不做猜测。
 * ================================================================== */

typedef struct {                 /* 一处待替换区间 */
    DWORD s, e;                  /* 原文 [s,e) 内容 */
    char *rep;                   /* 新内容（p21 已编码，不含引号） */
    DWORD rlen;
} P21Edit;

typedef struct { int pd, formation; }  P21PdForm;
typedef struct { int formation, product; } P21FormProd;
typedef struct { int pd; int parent; char *desc; DWORD dlen; } P21Nauo;
typedef struct { int product; DWORD ids, idlen, nms, nmlen; } P21Prod;
typedef struct { DWORD ds, de; int pd; } P21Shape;   /* PRODUCT_DEFINITION_SHAPE 的描述串 */

typedef struct {
    P21PdForm   *pdf;   size_t npdf,  cpdf;
    P21FormProd *fp;    size_t nfp,   cfp;
    P21Nauo     *nauo;  size_t nnauo, cnauo;
    P21Prod     *prod;  size_t nprod, cprod;
    P21Shape    *shp;   size_t nshp,  cshp;
} P21Index;

/* 简单的按需扩容。elem 为单元素字节数，返回新指针；失败返回 NULL。 */
static void *arr_grow(void *p, size_t *cap, size_t need, size_t elem)
{
    if (need < *cap) return p;
    size_t nc = *cap ? *cap * 2 : 64;
    while (nc <= need) nc *= 2;
    void *q = realloc(p, nc * elem);
    if (!q) return NULL;
    *cap = nc;
    return q;
}

static const BYTE *skip_ws(const BYTE *p, const BYTE *end)
{
    while (p < end) {
        BYTE c = *p;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') p++;
        else break;
    }
    return p;
}

/* p 指在开引号上；成功后 *ps/*pe 为内容区间，*pp 停在闭引号之后。 */
static BOOL p21_str(const BYTE **pp, const BYTE *end,
                    const BYTE **ps, const BYTE **pe)
{
    const BYTE *p = *pp;
    if (p >= end || *p != '\'') return FALSE;
    p++;
    const BYTE *s = p;
    while (p < end) {
        if (*p == '\'') {
            if (p + 1 < end && p[1] == '\'') { p += 2; continue; }
            *ps = s; *pe = p; *pp = p + 1;
            return TRUE;
        }
        p++;
    }
    return FALSE;
}

/* p 指在 '#' 上；读一个实体引用号。 */
static BOOL p21_ref(const BYTE **pp, const BYTE *end, int *out)
{
    const BYTE *p = *pp;
    if (p >= end || *p != '#') return FALSE;
    p++;
    long v = 0;
    int any = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
        any = 1;
        if (v > 100000000L) return FALSE;
    }
    if (!any) return FALSE;
    *out = (int)v;
    *pp = p;
    return TRUE;
}

/* 从 p 起跳过当前参数，停在顶层的 ',' 或 ')' 上。 */
static const BYTE *p21_param_end(const BYTE *p, const BYTE *end)
{
    int depth = 0;
    while (p < end) {
        BYTE c = *p;
        if (c == '\'') {
            p++;
            while (p < end) {
                if (*p == '\'') {
                    if (p + 1 < end && p[1] == '\'') { p += 2; continue; }
                    p++;
                    break;
                }
                p++;
            }
            continue;
        }
        if (c == '(') { depth++; p++; continue; }
        if (c == ')') {
            if (depth == 0) return p;
            depth--;
            p++;
            continue;
        }
        if (c == ',' && depth == 0) return p;
        p++;
    }
    return p;
}

/* 参数起点：idx 从 0 起。失败返回 NULL。 */
static const BYTE *p21_arg(const BYTE *p, const BYTE *end, int idx)
{
    int i = 0;
    while (i < idx) {
        const BYTE *q = p21_param_end(p, end);
        if (q >= end || *q != ',') return NULL;
        p = skip_ws(q + 1, end);
        i++;
    }
    return p;
}

/* p 指在实体的 '(' 上；返回配套的 ')' 之后的 ';' 位置。
 * 注意不能复用 p21_param_end：那个函数从参数内容起算，遇到括号会进出，
 * 从 '(' 起点调用会一直扫到文件末尾，把后面的实体全部跳掉。 */
static const BYTE *p21_ent_end(const BYTE *p, const BYTE *end)
{
    int depth = 0;
    while (p < end) {
        BYTE c = *p;
        if (c == '\'') {
            p++;
            while (p < end) {
                if (*p == '\'') {
                    if (p + 1 < end && p[1] == '\'') { p += 2; continue; }
                    p++;
                    break;
                }
                p++;
            }
            continue;
        }
        if (c == '(') { depth++; p++; continue; }
        if (c == ')') {
            depth--;
            p++;
            if (depth <= 0) break;
            continue;
        }
        p++;
    }
    while (p < end && *p != ';') p++;
    return p;
}

/* 名字是否完全等于 lit，且紧跟 '(' */
static BOOL ent_is(const BYTE *p, const BYTE *end, const char *lit, size_t n)
{
    if ((size_t)(end - p) < n + 1) return FALSE;
    if (memcmp(p, lit, n) != 0) return FALSE;
    return p[n] == '(';
}

/* 扫描全文，收集改 PRODUCT 名所需的四类实体。 */
static void p21_index(const BYTE *buf, DWORD len, P21Index *ix)
{
    const BYTE *p = buf;
    const BYTE *end = buf + len;

    memset(ix, 0, sizeof(*ix));

    while (p < end) {
        /* 找实体起点 "#<数字>=" */
        while (p < end && *p != '#') p++;
        if (p >= end) break;

        const BYTE *q = p;
        int id = 0;
        if (!p21_ref(&q, end, &id)) { p++; continue; }
        if (q >= end || *q != '=') { p++; continue; }
        q++;
        q = skip_ws(q, end);
        if (q >= end) break;

        const BYTE *nm = q;
        while (q < end && ((*q >= 'A' && *q <= 'Z') || *q == '_' ||
                           (*q >= '0' && *q <= '9'))) q++;
        size_t nlen = (size_t)(q - nm);

        if (q < end && *q == '(' && nlen > 0) {
            const BYTE *args = skip_ws(q + 1, end);

            if (ent_is(nm, end, "PRODUCT", 7)) {
                const BYTE *a0 = p21_arg(args, end, 0);
                const BYTE *a1 = p21_arg(args, end, 1);
                const BYTE *s0, *e0, *s1, *e1;
                if (a0 && a1 && p21_str(&a0, end, &s0, &e0) &&
                    p21_str(&a1, end, &s1, &e1)) {
                    P21Prod *np = (P21Prod *)arr_grow(ix->prod, &ix->cprod,
                                                      ix->nprod, sizeof(P21Prod));
                    if (np) {
                        ix->prod = np;
                        np[ix->nprod].product = id;
                        np[ix->nprod].ids = (DWORD)(s0 - buf);
                        np[ix->nprod].idlen = (DWORD)(e0 - s0);
                        np[ix->nprod].nms = (DWORD)(s1 - buf);
                        np[ix->nprod].nmlen = (DWORD)(e1 - s1);
                        ix->nprod++;
                    }
                }
            } else if (ent_is(nm, end, "PRODUCT_DEFINITION", 18)) {
                const BYTE *a2 = p21_arg(args, end, 2);
                int fid = 0;
                if (a2 && p21_ref(&a2, end, &fid)) {
                    P21PdForm *np = (P21PdForm *)arr_grow(ix->pdf, &ix->cpdf,
                                                          ix->npdf, sizeof(P21PdForm));
                    if (np) {
                        ix->pdf = np;
                        np[ix->npdf].pd = id;
                        np[ix->npdf].formation = fid;
                        ix->npdf++;
                    }
                }
            } else if (nlen >= 28 &&
                       memcmp(nm, "PRODUCT_DEFINITION_FORMATION", 28) == 0) {
                /* 名字还有 _WITH_SPECIFIED_SOURCE 之类的后缀，用前缀判断 */
                const BYTE *a2 = p21_arg(args, end, 2);
                int pid = 0;
                if (a2 && p21_ref(&a2, end, &pid)) {
                    P21FormProd *np = (P21FormProd *)arr_grow(ix->fp, &ix->cfp,
                                                              ix->nfp, sizeof(P21FormProd));
                    if (np) {
                        ix->fp = np;
                        np[ix->nfp].formation = id;
                        np[ix->nfp].product = pid;
                        ix->nfp++;
                    }
                }
            } else if (ent_is(nm, end, "PRODUCT_DEFINITION_SHAPE", 24)) {
                const BYTE *a1 = p21_arg(args, end, 1);
                const BYTE *a2 = p21_arg(args, end, 2);
                const BYTE *s1, *e1;
                int pd = 0;
                if (a1 && a2 && p21_str(&a1, end, &s1, &e1) &&
                    p21_ref(&a2, end, &pd)) {
                    P21Shape *np = (P21Shape *)arr_grow(ix->shp, &ix->cshp,
                                                        ix->nshp, sizeof(P21Shape));
                    if (np) {
                        ix->shp = np;
                        np[ix->nshp].ds = (DWORD)(s1 - buf);
                        np[ix->nshp].de = (DWORD)(e1 - s1) + np[ix->nshp].ds;
                        np[ix->nshp].pd = pd;
                        ix->nshp++;
                    }
                }
            } else if (ent_is(nm, end, "NEXT_ASSEMBLY_USAGE_OCCURRENCE", 30)) {
                const BYTE *a2 = p21_arg(args, end, 2);
                const BYTE *a3 = p21_arg(args, end, 3);   /* parent PD */
                const BYTE *a4 = p21_arg(args, end, 4);   /* child  PD */
                const BYTE *s2, *e2;
                int pd = 0, parent = 0;
                if (a2 && a4 && p21_str(&a2, end, &s2, &e2) &&
                    p21_ref(&a4, end, &pd) && p21_ref(&a3, end, &parent)) {
                    P21Nauo *np = (P21Nauo *)arr_grow(ix->nauo, &ix->cnauo,
                                                      ix->nnauo, sizeof(P21Nauo));
                    if (np) {
                        ix->nauo = np;
                        np[ix->nnauo].pd = pd;
                        np[ix->nnauo].parent = parent;
                        np[ix->nnauo].desc = (char *)s2;
                        np[ix->nnauo].dlen = (DWORD)(e2 - s2);
                        ix->nnauo++;
                    }
                }
            }
        }

        /* 跳到本条实体结尾，避免把字符串里的 '#' 误当实体起点 */
        const BYTE *semi = p21_ent_end(q, end);
        p = (semi < end) ? semi + 1 : end;
    }
}

static void p21_free_index(P21Index *ix)
{
    free(ix->pdf); free(ix->fp); free(ix->nauo); free(ix->prod); free(ix->shp);
    memset(ix, 0, sizeof(*ix));
}

/* 该 p21 字符串里是否已经有中文（\X2\ 转义或原始 GBK 字节） */
static int p21_has_cjk(const BYTE *s, DWORD n)
{
    DWORD i;
    for (i = 0; i + 3 < n; i++)
        if (s[i] == '\\' && s[i + 1] == 'X' && s[i + 2] == '2' && s[i + 3] == '\\')
            return 1;
    for (i = 0; i < n; i++)
        if (s[i] > 127) return 1;
    return 0;
}

/* 把一段 p21 字符串规整成合法内容。
 * 关键：只有出现原始高位字节（Creo 按 ANSI 写出的中文）时才重新转义；
 * 已经是 \X2\ 转义的一律原样复制，否则反斜杠会被二次加倍成 \\X2\\，直接写坏文件。 */
static char *p21_normalize(const BYTE *s, DWORD n, DWORD *out_len)
{
    DWORD i;
    int has_hi = 0;
    for (i = 0; i < n; i++) {
        if (s[i] > 127) { has_hi = 1; break; }
    }

    char *out = (char *)malloc((size_t)n * 8 + 32);
    if (!out) return NULL;

    if (!has_hi) {
        memcpy(out, s, n);
        out[n] = 0;
        *out_len = n;
        return out;
    }

    /* 含 ANSI(936) 字节：解码成宽字符再走标准 \X2\ 转义 */
    int need = MultiByteToWideChar(936, 0, (const char *)s, (int)n, NULL, 0);
    if (need <= 0) { free(out); return NULL; }
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(need + 1));
    if (!w) { free(out); return NULL; }
    MultiByteToWideChar(936, 0, (const char *)s, (int)n, w, need);
    w[need] = 0;

    p21_encode(w, out, (size_t)n * 8 + 32);
    free(w);

    *out_len = (DWORD)strlen(out);
    return out;
}

/* 按起始位置排序，保证 p21_apply_edits 的升序前提 */
static int p21_edit_cmp(const void *a, const void *b)
{
    const P21Edit *x = (const P21Edit *)a;
    const P21Edit *y = (const P21Edit *)b;
    if (x->s < y->s) return -1;
    if (x->s > y->s) return 1;
    return 0;
}

/* 把若干编辑一次性套用到文件上。edits 必须按 s 升序且互不重叠。 */
static BOOL p21_apply_edits(const wchar_t *path, const BYTE *buf, DWORD len,
                            const P21Edit *ed, size_t n)
{
    size_t i;
    size_t newlen = len;
    for (i = 0; i < n; i++) newlen += (size_t)ed[i].rlen - (ed[i].e - ed[i].s);

    BYTE *out = (BYTE *)malloc(newlen + 1);
    if (!out) return FALSE;

    DWORD src = 0;
    size_t dst = 0;
    for (i = 0; i < n; i++) {
        if (ed[i].s < src || ed[i].e > len) { free(out); return FALSE; }
        DWORD cp = ed[i].s - src;
        memcpy(out + dst, buf + src, cp);
        dst += cp;
        if (ed[i].rep && ed[i].rlen) {     /* rep==NULL/rlen==0 表示删除 */
            memcpy(out + dst, ed[i].rep, ed[i].rlen);
            dst += ed[i].rlen;
        }
        src = ed[i].e;
    }
    memcpy(out + dst, buf + src, len - src);
    dst += len - src;
    out[dst] = 0;

    BOOL ok = write_all_bytes(path, out, (DWORD)dst);
    free(out);
    return ok;
}

/* 主入口：改写 STEP 内部所有 PRODUCT 的 id / name。
 *   mdl_name  顶层模型名（用于认出顶层 PRODUCT）
 *   main_cn   顶层模型想要的中文名
 *   lookup    名称对照表查询回调（可为 NULL）
 *   out_renamed  实际改掉的名字个数 */
typedef int (*P21LookupFn)(const wchar_t *key, wchar_t *out, int cap);

static BOOL rewrite_step_products(const wchar_t *path,
                                  const wchar_t *mdl_name,
                                  const wchar_t *main_cn,
                                  P21LookupFn lookup,
                                  int *out_renamed)
{
    if (out_renamed) *out_renamed = 0;

    DWORD len = 0;
    BYTE *buf = read_all_bytes(path, &len);
    if (!buf) return FALSE;

    P21Index ix;
    p21_index(buf, len, &ix);
    if (ix.nprod == 0) { p21_free_index(&ix); free(buf); return TRUE; }

    /* product 实体号 -> 中文名（p21 编码后的字节串） */
    char **cname = (char **)calloc(ix.nprod, sizeof(char *));
    DWORD *clen = (DWORD *)calloc(ix.nprod, sizeof(DWORD));
    if (!cname || !clen) {
        free(cname); free(clen); p21_free_index(&ix); free(buf);
        return FALSE;
    }

    size_t k;
    /* 第一优先：NAUO 里已经写好的中文组件名 */
    for (k = 0; k < ix.nnauo; k++) {
        if (!p21_has_cjk((const BYTE *)ix.nauo[k].desc, ix.nauo[k].dlen))
            continue;

        /* pd -> formation -> product */
        size_t d, f;
        int formation = -1, product = -1;
        for (d = 0; d < ix.npdf; d++)
            if (ix.pdf[d].pd == ix.nauo[k].pd) { formation = ix.pdf[d].formation; break; }
        if (formation < 0) continue;
        for (f = 0; f < ix.nfp; f++)
            if (ix.fp[f].formation == formation) { product = ix.fp[f].product; break; }
        if (product < 0) continue;

        size_t pi;
        for (pi = 0; pi < ix.nprod; pi++) {
            if (ix.prod[pi].product != product || cname[pi]) continue;
            DWORD olen = 0;
            char *norm = p21_normalize((const BYTE *)ix.nauo[k].desc,
                                       ix.nauo[k].dlen, &olen);
            if (norm) { cname[pi] = norm; clen[pi] = olen; }
            break;
        }
    }

    /* 第二优先：顶层装配自身，用主模型中文名。
     * 结构化找顶层：NAUO 第 4 参数是 parent PD。被 parent 引用、
     * 却从未作为 child 出现的 PD，就是顶层装配 —— 不依赖模型名字符串
     * 匹配（模型改名未存盘、或 Creo 导出用了内部名时字符串会失配）。 */
    if (main_cn && main_cn[0]) {
        /* product 实体号 -> 中文名（p21 编码后的字节串） */
        char enc[1024];
        p21_encode(main_cn, enc, sizeof(enc));
        size_t pi;
        for (k = 0; k < ix.nnauo; k++) {
            int parent = ix.nauo[k].parent;
            size_t d;
            int is_child = 0, formation = -1, product = -1;
            for (d = 0; d < ix.nnauo; d++)
                if (ix.nauo[d].pd == parent) { is_child = 1; break; }
            if (is_child) continue;              /* 它也是别人的子件，不是顶层 */

            for (d = 0; d < ix.npdf; d++)
                if (ix.pdf[d].pd == parent) { formation = ix.pdf[d].formation; break; }
            if (formation < 0) continue;
            for (d = 0; d < ix.nfp; d++)
                if (ix.fp[d].formation == formation) { product = ix.fp[d].product; break; }
            if (product < 0) continue;

            for (pi = 0; pi < ix.nprod; pi++) {
                if (ix.prod[pi].product != product || cname[pi]) continue;
                cname[pi] = _strdup(enc);
                clen[pi] = (DWORD)strlen(enc);
                break;
            }
        }
    }

    /* 第三优先：名称对照表 */
    if (lookup) {
        for (k = 0; k < ix.nprod; k++) {
            if (cname[k]) continue;
            wchar_t key[512], val[512];
            DWORD kl = ix.prod[k].idlen;
            if (kl >= sizeof(key) / sizeof(wchar_t) - 8) continue;
            MultiByteToWideChar(936, 0, (const char *)buf + ix.prod[k].ids,
                                (int)kl, key, (int)(sizeof(key) / sizeof(wchar_t)) - 1);
            key[kl] = 0;
            if (lookup(key, val, (int)(sizeof(val) / sizeof(wchar_t)))) {
                char enc[1024];
                p21_encode(val, enc, sizeof(enc));
                cname[k] = _strdup(enc);
                clen[k] = (DWORD)strlen(enc);
            }
        }
    }

    /* 去重：两个不同零件撞成同一个中文名，导入端会合并甚至报错 */
    for (k = 0; k < ix.nprod; k++) {
        if (!cname[k]) continue;
        size_t j;
        int dup = 0;
        for (j = 0; j < k; j++) {
            if (cname[j] && clen[j] == clen[k] &&
                memcmp(cname[j], cname[k], clen[k]) == 0) { dup = 1; break; }
        }
        if (!dup) continue;
        char alt[1200];
        int serial;
        for (serial = 2; serial < 100; serial++) {
            _snprintf_s(alt, sizeof(alt), _TRUNCATE, "%s_%d", cname[k], serial);
            size_t q;
            int clash = 0;
            for (q = 0; q < k; q++) {
                if (cname[q] && clen[q] == (DWORD)strlen(alt) &&
                    memcmp(cname[q], alt, clen[q]) == 0) { clash = 1; break; }
            }
            if (!clash) break;
        }
        char *nn = _strdup(alt);
        if (nn) { free(cname[k]); cname[k] = nn; clen[k] = (DWORD)strlen(alt); }
    }

    /* 生成编辑：先 id 后 name，每个 PRODUCT 内部按出现顺序，整体天然升序 */
    P21Edit *ed = NULL;
    size_t ned = 0, ced = 0;
    size_t prod_edits = 0;   /* 只数 PRODUCT 的编辑，弹窗报告要用准确值 */
    for (k = 0; k < ix.nprod; k++) {
        if (!cname[k]) continue;
        size_t need = ned + 2;
        P21Edit *grown = (P21Edit *)arr_grow(ed, &ced, need - 1, sizeof(P21Edit));
        if (!grown) break;
        ed = grown;

        P21Prod *pr = &ix.prod[k];
        if (pr->ids < pr->nms) {
            ed[ned].s = pr->ids;  ed[ned].e = pr->ids + pr->idlen;
            ed[ned].rep = cname[k]; ed[ned].rlen = clen[k];
            ned++;
            ed[ned].s = pr->nms;  ed[ned].e = pr->nms + pr->nmlen;
            ed[ned].rep = cname[k]; ed[ned].rlen = clen[k];
            ned++;
        } else {
            ed[ned].s = pr->nms;  ed[ned].e = pr->nms + pr->nmlen;
            ed[ned].rep = cname[k]; ed[ned].rlen = clen[k];
            ned++;
            ed[ned].s = pr->ids;  ed[ned].e = pr->ids + pr->idlen;
            ed[ned].rep = cname[k]; ed[ned].rlen = clen[k];
            ned++;
        }
        prod_edits += 2;
    }

    /* SHAPE FOR <名>. 里的名也换掉。
     * 这是 PTC 自己的描述约定，有些导入器会拿它当零件名，改了没有副作用。 */
    for (k = 0; k < ix.nshp; k++) {
        const BYTE *ds = buf + ix.shp[k].ds;
        DWORD dl = ix.shp[k].de - ix.shp[k].ds;
        /* 最短合法形式是 "SHAPE FOR x." ，共 12 字节，所以只能用 < 不能 <= */
        if (dl < 12) continue;
        if (memcmp(ds, "SHAPE FOR ", 10) != 0) continue;
        if (ds[dl - 1] != '.') continue;

        char *cn = NULL;
        DWORD cnl = 0;
        size_t d2, f2, p2;
        for (d2 = 0; d2 < ix.npdf; d2++) {
            if (ix.pdf[d2].pd != ix.shp[k].pd) continue;
            for (f2 = 0; f2 < ix.nfp; f2++) {
                if (ix.fp[f2].formation != ix.pdf[d2].formation) continue;
                for (p2 = 0; p2 < ix.nprod; p2++) {
                    if (ix.prod[p2].product != ix.fp[f2].product) continue;
                    cn = cname[p2];
                    cnl = clen[p2];
                    break;
                }
                break;
            }
            break;
        }
        if (!cn) continue;

        P21Edit *grown2 = (P21Edit *)arr_grow(ed, &ced, ned, sizeof(P21Edit));
        if (!grown2) break;
        ed = grown2;
        ed[ned].s = ix.shp[k].ds + 10;
        ed[ned].e = ix.shp[k].de - 1;
        ed[ned].rep = cn;
        ed[ned].rlen = cnl;
        ned++;
    }

    /* PRODUCT 与 SHAPE 两轮收集交错，应用前必须按位置排序 */
    if (ned > 1) qsort(ed, ned, sizeof(P21Edit), p21_edit_cmp);

    BOOL ok = TRUE;
    if (ned > 0) ok = p21_apply_edits(path, buf, len, ed, ned);
    if (ok && out_renamed) *out_renamed = (int)(prod_edits / 2);

    for (k = 0; k < ix.nprod; k++) free(cname[k]);
    free(cname); free(clen); free(ed);
    p21_free_index(&ix);
    free(buf);
    return ok;
}

/* ------------------------------------------------------------------
 * 生成 Creo 回读用的副本
 *
 * 实测结论（2026-10-21）：
 *   SolidWorks 只认 ISO 10303-21 的 \X2\...\X0\ 转义；
 *   Creo 的导入器正相反 —— 不解析转义（把反斜杠替换成下划线），
 *   但会按 UTF-8 解释字符串里的原始非 ASCII 字节。
 * 一种编码不可能两头都显示中文，所以出两份：
 *   主文件  <名字>.stp         \X2\ 转义      -> SolidWorks / 标准工具链
 *   副本    <名字>_creo.stp    UTF-8 原始字节 -> Creo 回读
 *
 * 另外实测：Creo 导入会把 NAUO 的 description 也当作名字来源，
 * 与 PRODUCT 撞车就多一个 _1 后缀（id/name/description 三处都写中文
 * 会变成 _1_1）。副本里把带中文的 description 清空，只剩一个 _1
 * —— 那是 Creo 给导入元件的实例编号，文件层面消不掉。
 * ------------------------------------------------------------------ */
static BOOL write_creo_variant(const wchar_t *src, const wchar_t *dst)
{
    DWORD len = 0;
    BYTE *buf = read_all_bytes(src, &len);
    if (!buf) return FALSE;

    /* 1) \X2\HEX\X0\ -> UTF-8 原始字节（每 4 个十六进制 = 1 个 UTF-16 码元） */
    BYTE *out = (BYTE *)malloc((size_t)len * 4 + 64);
    if (!out) { free(buf); return FALSE; }

    size_t o = 0;
    DWORD i = 0;
    while (i < len) {
        if (i + 3 < len && buf[i] == '\\' && buf[i + 1] == 'X' &&
            buf[i + 2] == '2' && buf[i + 3] == '\\') {
            DWORD j = i + 4;
            wchar_t wbuf[512];
            int wn = 0, bad = 0;
            while (j + 3 < len && !(buf[j] == '\\' && buf[j + 1] == 'X' &&
                                    buf[j + 2] == '0' && buf[j + 3] == '\\')) {
                unsigned v = 0;
                int h;
                if (j + 4 > len) { bad = 1; break; }
                for (h = 0; h < 4; h++) {
                    BYTE c = buf[j + (DWORD)h];
                    v <<= 4;
                    if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
                    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
                    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
                    else { bad = 1; break; }
                }
                if (bad) break;
                if (wn < 511) wbuf[wn++] = (wchar_t)v;
                j += 4;
            }
            if (!bad && wn > 0 && j + 3 < len) {
                char u8[2048];
                int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, wn, u8,
                                            (int)sizeof(u8), NULL, NULL);
                if (n > 0) { memcpy(out + o, u8, (size_t)n); o += (size_t)n; }
                i = j + 4;
                continue;
            }
        }
        out[o++] = buf[i++];
    }
    free(buf);

    BOOL ok = write_all_bytes(dst, out, (DWORD)o);
    free(out);
    if (!ok) return FALSE;

    /* 2) 清空带非 ASCII 的 NAUO description，消掉多余的那个 _1 */
    DWORD len2 = 0;
    BYTE *b2 = read_all_bytes(dst, &len2);
    if (!b2) return FALSE;

    P21Index ix;
    p21_index(b2, len2, &ix);

    P21Edit *ed = NULL;
    size_t ned = 0, ced = 0, k;
    for (k = 0; k < ix.nnauo; k++) {
        DWORD t;
        int has_hi = 0;
        for (t = 0; t < ix.nauo[k].dlen; t++)
            if ((BYTE)ix.nauo[k].desc[t] > 127) { has_hi = 1; break; }
        if (!has_hi) continue;

        P21Edit *np = (P21Edit *)arr_grow(ed, &ced, ned, sizeof(P21Edit));
        if (!np) break;
        ed = np;
        ed[ned].s = (DWORD)((const BYTE *)ix.nauo[k].desc - b2);
        ed[ned].e = ed[ned].s + ix.nauo[k].dlen;
        ed[ned].rep = NULL;          /* 置空 */
        ed[ned].rlen = 0;
        ned++;
    }
    p21_free_index(&ix);

    if (ned > 0) ok = p21_apply_edits(dst, b2, len2, ed, ned) ? TRUE : FALSE;
    free(ed);
    free(b2);
    return ok;
}

/* ==================================================================
 * 第二部分：Creo 侧（自检时整段排除）
 * ================================================================== */
#if !defined(CN_STEP_SELFTEST) && !defined(CN_STEP_TOOL)

/* ------------------------------------------------------------------ 配置 */

typedef struct {
    wchar_t outdir[MAX_PATH];
    wchar_t mapping[MAX_PATH];
    int     rewrite_header;
    int     rewrite_products;
    int     creo_copy;      /* 额外出一份 UTF-8 版给 Creo 回读 */
    int     utf8_mapping;
} CnCfg;

static CnCfg g_cfg;

static void bytes_to_wstr(const BYTE *b, DWORD len, int prefer_utf8,
                          wchar_t *out, int cap)
{
    UINT cp = prefer_utf8 ? CP_UTF8 : CP_ACP;
    int need = MultiByteToWideChar(cp, 0, (LPCCH)b, (int)len, NULL, 0);
    if (need <= 0 && prefer_utf8) {
        cp = CP_ACP;
        need = MultiByteToWideChar(cp, 0, (LPCCH)b, (int)len, NULL, 0);
    }
    if (need <= 0) { out[0] = 0; return; }
    if (need >= cap) need = cap - 1;
    MultiByteToWideChar(cp, 0, (LPCCH)b, (int)len, out, need);
    out[need] = 0;
}

static void cfg_defaults(void)
{
    if (GetCurrentDirectoryW(MAX_PATH, g_cfg.outdir) == 0)
        wcscpy_s(g_cfg.outdir, MAX_PATH, L"C:\\Temp");
    wcscpy_s(g_cfg.mapping, MAX_PATH, L"");
    g_cfg.rewrite_header = 1;
    g_cfg.rewrite_products = 1;
    g_cfg.creo_copy = 1;
    g_cfg.utf8_mapping = 1;
}

/* 配置文件从"当前工作目录"读 —— 也就是启动 Creo 时所在的目录 */
static void cfg_load(void)
{
    cfg_defaults();

    wchar_t cfg_path[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH - 32, cfg_path) == 0) return;
    size_t L = wcslen(cfg_path);
    if (L && cfg_path[L - 1] != L'\\') wcscat_s(cfg_path, MAX_PATH, L"\\");
    wcscat_s(cfg_path, MAX_PATH, CFG_NAME);

    DWORD len = 0;
    BYTE *raw = read_all_bytes(cfg_path, &len);
    if (!raw) return;

    size_t wcap = (size_t)len * 2 + 8;
    wchar_t *text = (wchar_t *)malloc(sizeof(wchar_t) * wcap);
    if (text) {
        bytes_to_wstr(raw, len, 1, text, (int)wcap);
        wchar_t *line = wcstok(text, L"\r\n", NULL);
        while (line) {
            if (line[0] == 0xFEFF) line++;
            wchar_t *eq = wcschr(line, L'=');
            if (eq) {
                *eq = 0;
                wchar_t *k = line;
                wchar_t *v = eq + 1;
                while (*v == L' ' || *v == L'\t') v++;
                if (_wcsicmp(k, L"outdir") == 0) {
                    wcscpy_s(g_cfg.outdir, MAX_PATH, v);
                } else if (_wcsicmp(k, L"mapping") == 0) {
                    wcscpy_s(g_cfg.mapping, MAX_PATH, v);
                } else if (_wcsicmp(k, L"rewrite_header") == 0) {
                    g_cfg.rewrite_header = (_wtoi(v) != 0);
                } else if (_wcsicmp(k, L"rewrite_products") == 0) {
                    g_cfg.rewrite_products = (_wtoi(v) != 0);
                } else if (_wcsicmp(k, L"creo_copy") == 0) {
                    g_cfg.creo_copy = (_wtoi(v) != 0);
                } else if (_wcsicmp(k, L"encoding") == 0) {
                    g_cfg.utf8_mapping = (_wcsicmp(v, L"ansi") != 0);
                }
            }
            line = wcstok(NULL, L"\r\n", NULL);
        }
        free(text);
    }
    free(raw);
}

/* -------------------------------------------------------------- 对照表
 * 两列 CSV：导出名,中文名
 * ---------------------------------------------------------------- */

static int mapping_lookup(const wchar_t *mdl_name, wchar_t *out, int cap)
{
    if (!g_cfg.mapping[0]) return 0;

    DWORD len = 0;
    BYTE *raw = read_all_bytes(g_cfg.mapping, &len);
    if (!raw) return 0;

    size_t wcap = (size_t)len * 2 + 8;
    wchar_t *text = (wchar_t *)malloc(sizeof(wchar_t) * wcap);
    int found = 0;
    if (text) {
        bytes_to_wstr(raw, len, g_cfg.utf8_mapping, text, (int)wcap);
        wchar_t *line = wcstok(text, L"\r\n", NULL);
        while (line) {
            if (line[0] == 0xFEFF) line++;
            wchar_t *sep = wcschr(line, L',');
            if (sep) {
                *sep = 0;
                wchar_t *k = line;
                wchar_t *v = sep + 1;
                while (*k == L' ' || *k == L'\t') k++;
                while (*v == L' ' || *v == L'\t') v++;
                if (_wcsicmp(k, mdl_name) == 0) {
                    wcscpy_s(out, cap, v);
                    size_t L = wcslen(out);
                    while (L && (out[L - 1] == L' ' || out[L - 1] == L'\t'))
                        out[--L] = 0;
                    if (out[0]) { found = 1; break; }
                }
            }
            line = wcstok(NULL, L"\r\n", NULL);
        }
        free(text);
    }
    free(raw);
    return found;
}

/* ------------------------------------------------------------------ 提示
 * 用原生 MessageBoxW 而不是 ProMessageDisplay：
 *   ProMessageDisplay 收窄字符，中文要经代码页转换，容易变乱码；
 *   MessageBoxW 走 UTF-16，Creo 界面语言是什么都显示正确。
 * 命令回调运行在 UI 线程上，弹窗是安全的。
 * ---------------------------------------------------------------- */

static void msgbox(UINT icon, const wchar_t *fmt, va_list ap)
{
    wchar_t wbuf[2048];
    _vsnwprintf_s(wbuf, 2048, _TRUNCATE, fmt, ap);
    MessageBoxW(NULL, wbuf, L"导出中文 STEP",
                MB_OK | icon | MB_SETFOREGROUND);
}

static void say(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    msgbox(MB_ICONINFORMATION, fmt, ap);
    va_end(ap);
}

static void say_err(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    msgbox(MB_ICONERROR, fmt, ap);
    va_end(ap);
}

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
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
        else
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
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
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
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

static int cn_step_action(uiCmdCmdId command, uiCmdValue *p_value, void *p_data)
{
    (void)command; (void)p_value; (void)p_data;

    cfg_load();

    ProMdl model;
    ProError err = ProMdlCurrentGet(&model);
    if (err != PRO_TK_NO_ERROR) {
        say_err(L"没有打开的模型，请先打开零件或装配。");
        return 0;
    }

    /* 模型名本身就是宽字符，中文名可直接拿到。
     * Creo 2 没有 ProMdlMdlnameGet/ProMdlName，用旧的
     * ProMdlNameGet + ProName（32 字符，够用）；Creo 3+ 用新接口。 */
#ifdef CN_STEP_OLD_SDK
    ProName mdl_name;
    err = ProMdlNameGet(model, mdl_name);
#else
    ProMdlName mdl_name;
    err = ProMdlMdlnameGet(model, mdl_name);
#endif
    if (err != PRO_TK_NO_ERROR || !mdl_name[0]) {
        say_err(L"取模型名失败（ProMdlMdlnameGet 返回 %d）。", (int)err);
        return 0;
    }

    /* 模型类型校验：
     * ProSolid / ProAssembly / ProPart 在 ProObjects.h 里是同一个类型
     *   typedef struct sld_part* ProSolid;
     *   typedef struct sld_part* ProAssembly;
     * 所以零件和装配体都能直接传给导出接口。
     * 工程图是 struct drawing*，类型不同，必须挡掉，否则 handle 被错误解释。 */
    ProMdlType mtype = PRO_MDL_UNUSED;
    err = ProMdlTypeGet(model, &mtype);
    if (err != PRO_TK_NO_ERROR) {
        say_err(L"取模型类型失败（ProMdlTypeGet 返回 %d）。", (int)err);
        return 0;
    }
    if (mtype != PRO_MDL_PART && mtype != PRO_MDL_ASSEMBLY) {
        say_err(L"当前模型不是零件或装配体（类型代码 %d），无法导出 STEP。\n\n"
                L"如果现在开的是工程图，请先切换到它引用的零件/装配体。",
                (int)mtype);
        return 0;
    }

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

/* ------------------------------------------------------------- 注册入口
 * 签名对照官方示例 protk_appls/pt_autoaxis/pt_autoaxis_src/AutoAxis.c
 * ---------------------------------------------------------------- */

static uiCmdAccessState access_always(uiCmdAccessMode mode)
{
    (void)mode;
    return ACCESS_AVAILABLE;
}

/* --------------------------------------------------------------
 * 启动日志：把所有"插件到底有没有被 Creo 加载"的事实写进一行文本，
 * 落盘到 DLL 同目录的 cn_step_load.log。
 *
 * 为什么需要它：TOOLKIT 加载失败时 Creo 只是静默地不显示菜单，用户
 * 完全看不出是路径错了、REVISION 不对，还是 DLL 位数不对。有了这行
 * 日志，"DLL 被调用了没有 / Creo 报的版本号是多少 / 菜单注册返回码"
 * 立刻可判——换台电脑、换个 Creo 版本排查时尤其有用。
 *
 * 用 GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 反查自身模块，因为
 * user_initialize 的签名里拿不到 HINSTANCE。
 * -------------------------------------------------------------- */
/* 功能区定义文件（toolkitribbonui.rbn）的加载返回码。
 * -999 表示这一轮没走到那段代码，方便跟真实返回码区分。
 * 用全局变量而不是函数参数，是为了不去动 cn_log_startup 的每一处调用。 */
static int g_rbn_err = -999;

static void cn_log_startup(const char *version, const char *build,
                           int menu_err, int cmd_err, int btn_err,
                           int about_err, int dsg1, int dsg2)
{
    wchar_t   mod[MAX_PATH * 2];
    wchar_t   path[MAX_PATH * 2];
    wchar_t  *slash;
    HMODULE   hm = NULL;
    SYSTEMTIME st;
    FILE     *f = NULL;
    char      v[128];
    char      b[160];

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(void *)&cn_log_startup, &hm))
        return;
    if (!GetModuleFileNameW(hm, mod, MAX_PATH * 2))
        return;

    wcscpy_s(path, MAX_PATH * 2, mod);
    slash = wcsrchr(path, L'\\');
    if (!slash)
        return;
    wcscpy_s(slash + 1, (size_t)(MAX_PATH * 2 - (slash + 1 - path)),
             L"cn_step_load.log");

    v[0] = 0;
    b[0] = 0;
    if (version) strncpy_s(v, sizeof(v), version, _TRUNCATE);
    if (build)   strncpy_s(b, sizeof(b), build,   _TRUNCATE);

    GetLocalTime(&st);
    if (_wfopen_s(&f, path, L"a, ccs=UTF-8") != 0 || !f)
        return;
    fwprintf(f,
             L"[%04d-%02d-%02d %02d:%02d:%02d] pid=%lu creo=%hs build=%hs "
             L"menu=%d cmd=%d btn=%d about=%d dsg=%d/%d rbn=%d\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long)GetCurrentProcessId(), v, b,
             menu_err, cmd_err, btn_err, about_err, dsg1, dsg2, g_rbn_err);
    fclose(f);
}

int user_initialize(int argc, char *argv[], char *version, char *build,
                    wchar_t errbuf[80])
{
    (void)argc; (void)argv;
    if (errbuf) errbuf[0] = 0;

    ProFileName msgf;
    ProStringToWstring(msgf, (char *)MSGFILE);

    ProError err_menu = ProMenubarMenuAdd((char *)"CnStepMenu",
                                          (char *)"CnStepMenuLabel",
                                          (char *)"Utilities", PRO_B_TRUE, msgf);
    if (err_menu != PRO_TK_NO_ERROR && err_menu != PRO_TK_E_FOUND) {
        if (errbuf) swprintf_s(errbuf, 80, L"ProMenubarMenuAdd: %d", (int)err_menu);
        cn_log_startup(version, build, (int)err_menu, -1, -1, -1, -1, -1);
        return err_menu;
    }

    uiCmdCmdId cmd_id = 0;
    ProError err_cmd = ProCmdActionAdd((char *)"CnStepExportCmd", (uiCmdCmdActFn)cn_step_action,
                                       uiCmdPrioDefault, (uiCmdAccessFn)access_always,
                                       PRO_B_TRUE, PRO_B_TRUE, &cmd_id);
    if (err_cmd != PRO_TK_NO_ERROR) {
        if (errbuf) swprintf_s(errbuf, 80, L"ProCmdActionAdd: %d", (int)err_cmd);
        cn_log_startup(version, build, (int)err_menu, (int)err_cmd, -1, -1, -1, -1);
        return err_cmd;
    }

    ProError err_btn = ProMenubarmenuPushbuttonAdd((char *)"CnStepMenu",
                                                   (char *)"CnStepExportBtn",
                                                   (char *)"CnStepExportLabel",
                                                   (char *)"CnStepExportHelp",
                                                   NULL, PRO_B_TRUE, cmd_id, msgf);

    /* ---- 关于落落：第二个命令 + 菜单项 ---- */
    uiCmdCmdId about_id = 0;
    ProError err_about = ProCmdActionAdd((char *)"CnStepAboutCmd",
                                         (uiCmdCmdActFn)cn_about_action,
                                         uiCmdPrioDefault,
                                         (uiCmdAccessFn)access_always,
                                         PRO_B_TRUE, PRO_B_TRUE, &about_id);
    ProError err_btn2 = PRO_TK_GENERAL_ERROR;
    if (err_about == PRO_TK_NO_ERROR) {
        err_btn2 = ProMenubarmenuPushbuttonAdd((char *)"CnStepMenu",
                                               (char *)"CnStepAboutBtn",
                                               (char *)"CnStepAboutLabel",
                                               (char *)"CnStepAboutHelp",
                                               NULL, PRO_B_TRUE, about_id,
                                               msgf);
    }

    /* ---- Designate：把两个命令放进「自定义功能区 → ToolKit 命令」列表，
     * 用户在 Creo 里做一次性的选项卡定制后，工具条布局存进 text 目录的
     * toolkitribbonui.rbn，之后每次启动自动出现「落落工具」选项卡 ---- */
    /* ProCmdDesignate 的三个文案参数是 char[PRO_LINE_SIZE] 数组类型，传首地址即可
     * （是「数组」不是「数组指针」，加 * 反而是 char(*)[] 会告警） */
    ProError err_dsg1 = ProCmdDesignate(cmd_id,
                                        (char *)"CnStepExportLabel",
                                        (char *)"CnStepExportHelp",
                                        (char *)"CnStepExportDesc",
                                        msgf);
    ProError err_dsg2 = PRO_TK_GENERAL_ERROR;
    if (err_about == PRO_TK_NO_ERROR)
        err_dsg2 = ProCmdDesignate(about_id,
                                        (char *)"CnStepAboutLabel",
                                        (char *)"CnStepAboutHelp",
                                        (char *)"CnStepAboutDesc",
                                        msgf);

    /* ---- 功能区选项卡 ----
     * Creo 只认 .rbn（UGC 文本格式）里的选项卡定义，没有「用代码往功能区
     * 加一个选项卡」的 API。所以我们自带一份 toolkitribbonui.rbn，放在
     * text\chinese_cn\ribbon\ 与 text\usascii\ribbon\。
     * 多数情况下 Creo 自己就会加载它，这里再显式叫一次做兜底：
     *   PRO_TK_NO_ERROR  加载成功
     *   PRO_TK_IN_USE    Creo 已经自动加载过了（正常情况，同样是好的）
     *   PRO_TK_E_NOT_FOUND / PRO_TK_INVALID_FILE  文件没找到或格式不对
     * 返回码进日志，出问题一眼能看出来。 */
    g_rbn_err = (int)ProRibbonDefinitionfileLoad(L"toolkitribbonui.rbn");

    cn_log_startup(version, build, (int)err_menu, (int)err_cmd, (int)err_btn,
                   (int)err_about, (int)err_dsg1, (int)err_dsg2);
    return err_btn;
}

void user_terminate(void)
{
}

#endif /* !CN_STEP_SELFTEST && !CN_STEP_TOOL */

/* ==================================================================
 * 第三部分：不依赖 Creo 的算法自检
 *   cl /DCN_STEP_SELFTEST 编译后可直接运行，用来验证 STEP 头改写
 * ================================================================== */
#ifdef CN_STEP_SELFTEST

static int g_fail = 0;

static void check(int cond, const char *what)
{
    wprintf(L"  %s  %hs\n", cond ? L"PASS" : L"FAIL", what);
    if (!cond) g_fail++;
}

static void check_enc(const wchar_t *in, const char *want, const char *what)
{
    char got[256];
    p21_encode(in, got, sizeof(got));
    if (strcmp(got, want) != 0) {
        wprintf(L"  FAIL  %hs\n        want \"%hs\"\n        got  \"%hs\"\n",
                what, want, got);
        g_fail++;
    } else {
        wprintf(L"  PASS  %hs  ->  \"%hs\"\n", what, got);
    }
}

int main(int argc, char **argv)
{
    /* 命令行模式：cn_step_selftest.exe <file.stp> [模型名] [中文名]
     * 不依赖 Creo，可当批量后处理器用（参数按系统 ANSI 代码页解释）。 */
    if (argc >= 2) {
        wchar_t wp[MAX_PATH * 2], wm[512] = L"", wc[512] = L"";
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wp, MAX_PATH * 2);
        if (argc >= 3) MultiByteToWideChar(CP_ACP, 0, argv[2], -1, wm, 512);
        if (argc >= 4) MultiByteToWideChar(CP_ACP, 0, argv[3], -1, wc, 512);

        DWORD t0 = GetTickCount();
        int n = 0;
        BOOL ok = rewrite_step_products(wp, wm, wc, NULL, &n);

        /* 同时生成 Creo 回读副本：原名_creo.stp（UTF-8 原始字节） */
        wchar_t wcreo[MAX_PATH * 2];
        wcscpy_s(wcreo, MAX_PATH * 2, wp);
        wchar_t *dot = wcsrchr(wcreo, L'.');
        if (dot) *dot = 0;
        wcscat_s(wcreo, MAX_PATH * 2, L"_creo.stp");
        BOOL cok = ok ? write_creo_variant(wp, wcreo) : FALSE;
        DWORD ms = GetTickCount() - t0;

        printf("file   : %s\nresult : %s\nrenamed: %d products\nelapsed: %lu ms\n"
               "creo   : %s (%s)\n",
               argv[1], ok ? "OK" : "FAILED", n, (unsigned long)ms,
               cok ? "written" : "not written", cok ? "_creo.stp for Creo import" : "skipped");
        return ok ? 0 : 1;
    }

    wprintf(L"[1] p21_encode (ISO 10303-21 escaping)\n");
    check_enc(L"ABC", "ABC", "plain ascii untouched");
    check_enc(L"\x8F74\x627F\x5EA7", "\\X2\\8F74627F5EA7\\X0\\", "CJK -> X2 escape");
    check_enc(L"A'B", "A''B", "single quote doubled");
    check_enc(L"A\\B", "A\\\\B", "backslash doubled");
    check_enc(L"Part \x7AEF\x76D6", "Part \\X2\\7AEF76D6\\X0\\",
              "ascii prefix + CJK suffix");

    wprintf(L"\n[2] rewrite_step_header\n");

    const wchar_t *fx = L"build\\selftest_fixture.stp";
    const char *src =
        "ISO-10303-21;\r\n"
        "HEADER;\r\n"
        "FILE_DESCRIPTION((''),'2;1');\r\n"
        "FILE_NAME('_tmp_cn_step_12345','2026-10-20T16:00:00',(''),(''),"
        "'','','');\r\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\r\n"
        "ENDSEC;\r\n"
        "DATA;\r\n"
        "#1=CARTESIAN_POINT('',(0.,0.,0.));\r\n"
        "ENDSEC;\r\n"
        "END-ISO-10303-21;\r\n";

    CreateDirectoryW(L"build", NULL);
    write_all_bytes(fx, (const BYTE *)src, (DWORD)strlen(src));

    BOOL ok = rewrite_step_header(fx, L"\x8F74\x627F\x5EA7");
    check(ok, "rewrite returned TRUE");

    DWORD len = 0;
    BYTE *got = read_all_bytes(fx, &len);
    check(got != NULL, "file re-read");
    if (got) {
        check(strstr((char *)got,
                     "FILE_NAME('\\X2\\8F74627F5EA7\\X0\\',") != NULL,
              "FILE_NAME now escaped CJK");
        check(strstr((char *)got, "_tmp_cn_step_12345") == NULL,
              "old ascii name gone");
        check(strstr((char *)got, "ENDSEC;") != NULL &&
              strstr((char *)got, "#1=CARTESIAN_POINT") != NULL &&
              strstr((char *)got, "END-ISO-10303-21;") != NULL,
              "rest of file intact");
        check(strstr((char *)got, "AUTOMOTIVE_DESIGN") != NULL,
              "FILE_SCHEMA preserved");
        free(got);
    }

    /* 没有 FILE_NAME 的文件应当安全地返回 FALSE，且不改动内容 */
    const wchar_t *fx2 = L"build\\selftest_nofn.stp";
    const char *src2 = "ISO-10303-21;\r\nHEADER;\r\nENDSEC;\r\n";
    write_all_bytes(fx2, (const BYTE *)src2, (DWORD)strlen(src2));
    check(!rewrite_step_header(fx2, L"\x8F74\x627F\x5EA7"),
          "no FILE_NAME -> returns FALSE");

    DWORD len2 = 0;
    BYTE *got2 = read_all_bytes(fx2, &len2);
    check(got2 && len2 == strlen(src2) && memcmp(got2, src2, len2) == 0,
          "no FILE_NAME -> file untouched");
    free(got2);

    /* ---------------- 装配体内部组件名 ---------------- */
    wprintf(L"\n[3] rewrite_step_products (assembly component names)\n");

    /* 结构与真实 Creo 导出件一致：PD -> FORMATION -> PRODUCT，
     * 中文名挂在 NAUO 的第三个参数上。 */
    const wchar_t *fx3 = L"build\\selftest_asm.stp";
    const char *src3 =
        "ISO-10303-21;\r\n"
        "HEADER;\r\n"
        "FILE_NAME('_tmp_cn_step_777','2026-10-20T16:00:00',(''),(''),'','','');\r\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\r\n"
        "ENDSEC;\r\n"
        "DATA;\r\n"
        "#20=MECHANICAL_CONTEXT('',#17,'mechanical');\r\n"
        "#21=PRODUCT('000','000','NOT SPECIFIED',(#20));\r\n"
        "#22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#21,.MADE.);\r\n"
        "#23=PRODUCT_DEFINITION('design','',#22,#19);\r\n"
        "#24=PRODUCT_DEFINITION_SHAPE('','SHAPE FOR 000.',#23);\r\n"
        "#54=MECHANICAL_CONTEXT('',#17,'mechanical');\r\n"
        "#55=PRODUCT('0','0','NOT SPECIFIED',(#54));\r\n"
        "#56=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#55,.MADE.);\r\n"
        "#57=PRODUCT_DEFINITION('design','',#56,#19);\r\n"
        "#116=MECHANICAL_CONTEXT('',#17,'mechanical');\r\n"
        "#117=PRODUCT('ASM_TOP','ASM_TOP','NOT SPECIFIED',(#116));\r\n"
        "#118=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#117,.MADE.);\r\n"
        "#119=PRODUCT_DEFINITION('design','',#118,#85);\r\n"
        "#29=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship',\r\n"
        "'\\X2\\58EB5927592B\\X0\\',#119,#23,$);\r\n"
        /* 第二个组件的名字是原始 GBK 字节（"装" = D7 B0），模拟 Creo 按 ANSI 写出的情况 */
        "#60=NEXT_ASSEMBLY_USAGE_OCCURRENCE('1','Next assembly relationship',"
        "'\xD7\xB0',#119,#57,$);\r\n"
        "#1=CARTESIAN_POINT('',(0.,0.,0.));\r\n"
        "#2=DIRECTION('',(0.,0.,1.));\r\n"
        "ENDSEC;\r\n"
        "END-ISO-10303-21;\r\n";

    write_all_bytes(fx3, (const BYTE *)src3, (DWORD)strlen(src3));

    {
        DWORD dl = 0;
        BYTE *db = read_all_bytes(fx3, &dl);
        P21Index dix;
        p21_index(db, dl, &dix);
        wprintf(L"        idx: product=%llu pd=%llu formation=%llu nauo=%llu\n",
                (unsigned long long)dix.nprod, (unsigned long long)dix.npdf,
                (unsigned long long)dix.nfp, (unsigned long long)dix.nnauo);
        p21_free_index(&dix);
        free(db);
    }

    int renamed = 0;
    BOOL ok3 = rewrite_step_products(fx3, L"ASM_TOP",
                                     L"\x88C5\x914D\x4F53", NULL, &renamed);
    check(ok3, "assembly rewrite returned TRUE");
    check(renamed == 3, "3 products renamed");

    DWORD len3 = 0;
    BYTE *g3 = read_all_bytes(fx3, &len3);
    check(g3 != NULL, "assembly file re-read");
    if (g3) {
        check(strstr((char *)g3,
                     "#21=PRODUCT('\\X2\\58EB5927592B\\X0\\','\\X2\\58EB5927592B\\X0\\','NOT SPECIFIED'") != NULL,
              "child product id AND name replaced (from \\X2\\ desc)");

        /* GBK 原始字节必须被规范化成 \X2\，不能原样留着 */
        check(strstr((char *)g3, "PRODUCT('\\X2\\88C5\\X0\\','\\X2\\88C5\\X0\\','NOT SPECIFIED'") != NULL,
              "GBK bytes normalized to \\X2\\ escape");

        check(strstr((char *)g3,
                     "#117=PRODUCT('\\X2\\88C5914D4F53\\X0\\','\\X2\\88C5914D4F53\\X0\\'") != NULL,
              "top assembly uses main model chinese name");

        /* SHAPE FOR <名>. 也要跟着换，有些导入器拿它当零件名 */
        check(strstr((char *)g3,
                     "SHAPE FOR \\X2\\58EB5927592B\\X0\\.") != NULL,
              "SHAPE FOR description follows the rename");

        check(strstr((char *)g3, "CARTESIAN_POINT") != NULL &&
              strstr((char *)g3, "END-ISO-10303-21;") != NULL &&
              strstr((char *)g3, "NEXT_ASSEMBLY_USAGE_OCCURRENCE") != NULL,
              "geometry + nauo entities intact");

        /* 二次执行必须幂等：把 \X2\ 再转一次就会坏成 \\X2\\ */
        check(strstr((char *)g3, "\\\\X2\\\\") == NULL,
              "no double-escaping of \\X2\\");
        free(g3);
    }

    int renamed2 = 0;
    rewrite_step_products(fx3, L"ASM_TOP", L"\x88C5\x914D\x4F53", NULL, &renamed2);
    DWORD len4 = 0;
    BYTE *g4 = read_all_bytes(fx3, &len4);
    check(g4 && strstr((char *)g4, "\\\\X2\\\\") == NULL,
          "second pass does not double-escape");
    free(g4);

    /* 没有中文来源时必须保持原样，不做任何猜测 */
    const wchar_t *fx4 = L"build\\selftest_asm_plain.stp";
    const char *src4 =
        "ISO-10303-21;\r\nDATA;\r\n"
        "#21=PRODUCT('000','000','NOT SPECIFIED',(#20));\r\n"
        "#22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#21,.MADE.);\r\n"
        "#23=PRODUCT_DEFINITION('design','',#22,#19);\r\n"
        "#29=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship','',#19,#23,$);\r\n"
        "ENDSEC;\r\n";
    write_all_bytes(fx4, (const BYTE *)src4, (DWORD)strlen(src4));
    int renamed4 = 99;
    rewrite_step_products(fx4, L"OTHER", L"", NULL, &renamed4);
    check(renamed4 == 0, "no chinese source -> nothing renamed");

    DWORD len5 = 0;
    BYTE *g5 = read_all_bytes(fx4, &len5);
    check(g5 && len5 == strlen(src4) && memcmp(g5, src4, len5) == 0,
          "no chinese source -> file untouched");
    free(g5);

    /* 去重：两个零件撞同名，第二个必须自动加后缀 */
    const wchar_t *fx5 = L"build\\selftest_asm_dup.stp";
    const char *src5 =
        "ISO-10303-21;\r\nDATA;\r\n"
        "#21=PRODUCT('A1','A1','NOT SPECIFIED',(#20));\r\n"
        "#22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','L',#21,.MADE.);\r\n"
        "#23=PRODUCT_DEFINITION('design','',#22,#19);\r\n"
        "#51=PRODUCT('A2','A2','NOT SPECIFIED',(#20));\r\n"
        "#52=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','L',#51,.MADE.);\r\n"
        "#53=PRODUCT_DEFINITION('design','',#52,#19);\r\n"
        "#29=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','n','\\X2\\8F74627F5EA7\\X0\\',#19,#23,$);\r\n"
        "#59=NEXT_ASSEMBLY_USAGE_OCCURRENCE('1','n','\\X2\\8F74627F5EA7\\X0\\',#19,#53,$);\r\n"
        "ENDSEC;\r\n";
    write_all_bytes(fx5, (const BYTE *)src5, (DWORD)strlen(src5));
    int renamed5 = 0;
    rewrite_step_products(fx5, L"", L"", NULL, &renamed5);
    DWORD len6 = 0;
    BYTE *g6 = read_all_bytes(fx5, &len6);
    check(g6 && strstr((char *)g6, "'\\X2\\8F74627F5EA7\\X0\\_2'") != NULL,
          "duplicate names get _2 suffix");
    free(g6);

    /* [5] Creo 回读副本：\X2\ -> UTF-8 原始字节，NAUO 描述清空 */
    wprintf(L"\n[5] write_creo_variant (UTF-8 copy for Creo import)\n");
    {
        const wchar_t *f5 = L"build\\selftest_creo.stp";
        /* "士大夫" 的 UTF-16BE 码点是 58EB 5927 592B */
        const char *src5 =
            "ISO-10303-21;\r\n"
            "HEADER;\r\n"
            "FILE_NAME('\\X2\\58EB5927592B\\X0\\','2026-10-21',(''),(''),'','','');\r\n"
            "ENDSEC;\r\n"
            "DATA;\r\n"
            "#21=PRODUCT('\\X2\\58EB5927592B\\X0\\','\\X2\\58EB5927592B\\X0\\','NOT SPECIFIED',(#20));\r\n"
            "#22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE('1','LAST_VERSION',#21,.MADE.);\r\n"
            "#23=PRODUCT_DEFINITION('design','',#22,#19);\r\n"
            "#29=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship',"
            "'\\X2\\58EB5927592B\\X0\\',#119,#23,$);\r\n"
            "ENDSEC;\r\n"
            "END-ISO-10303-21;\r\n";
        write_all_bytes(f5, (const BYTE *)src5, (DWORD)strlen(src5));

        BOOL vok = write_creo_variant(f5, f5);
        check(vok, "variant written");

        DWORD l5 = 0;
        BYTE *g5 = read_all_bytes(f5, &l5);
        check(g5 != NULL, "variant readable");
        if (g5) {
            const char *cnt = (const char *)g5;
            const char utf8_name[] = "\xE5\xA3\xAB\xE5\xA4\xA7\xE5\xA4\xAB"; /* 士大夫 */
            check(strstr(cnt, "\\X2\\") == NULL, "no \\X2\\ escapes left");
            check(strstr(cnt, utf8_name) != NULL, "chinese became raw UTF-8 bytes");
            /* NAUO 的描述被清空，但其余字段保留 */
            check(strstr(cnt, "NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship','',") != NULL,
                  "NAUO description blanked");
            check(strstr(cnt, "#22=PRODUCT_DEFINITION_FORMATION_WITH_SPECIFIED_SOURCE") != NULL,
                  "other entities untouched");
            check(strstr(cnt, "END-ISO-10303-21;") != NULL, "trailer intact");
            /* 中文名出现的次数：FILE_NAME 1 次 + PRODUCT 的 id/name 2 次
             * （NAUO 那处已被清空） */
            int occurrences = 0;
            const char *p5 = cnt;
            while ((p5 = strstr(p5, utf8_name)) != NULL) { occurrences++; p5 += sizeof(utf8_name) - 1; }
            check(occurrences == 3, "chinese kept only where it matters");
            free(g5);
        }
    }

    /* [6] 标准写法 PRODUCT_DEFINITION_FORMATION（名字正好 28 字符）
     *     回归守卫：曾用 nlen > 28，把不带 _WITH_SPECIFIED_SOURCE 后缀的
     *     实体全部跳过，导致 formation 索引为 0、顶层永远改不到。
     *     SolidWorks 和多数 CAD 导出都是这种写法。 */
    wprintf(L"\n[6] PRODUCT_DEFINITION_FORMATION (exactly 28 chars)\n");
    {
        const wchar_t *f6 = L"build\\selftest_form.stp";
        const char *src6 =
            "ISO-10303-21;\n"
            "HEADER;\n"
            "FILE_NAME('selftest_form','2026-10-21',(''),(''),'','','');\n"
            "ENDSEC;\n"
            "DATA;\n"
            "#21=PRODUCT('TOP_ASM','TOP_ASM','NOT SPECIFIED',(#20));\n"
            "#22=PRODUCT_DEFINITION_FORMATION('1','LAST_VERSION',#21);\n"
            "#23=PRODUCT_DEFINITION('part definition','',#22,#19);\n"
            "#31=PRODUCT('CHILD','CHILD','NOT SPECIFIED',(#30));\n"
            "#32=PRODUCT_DEFINITION_FORMATION('1','LAST_VERSION',#31);\n"
            "#33=PRODUCT_DEFINITION('part definition','',#32,#29);\n"
            "#40=NEXT_ASSEMBLY_USAGE_OCCURRENCE('0','Next assembly relationship','',#23,#33,$);\n"
            "ENDSEC;\n"
            "END-ISO-10303-21;\n";
        write_all_bytes(f6, (const BYTE *)src6, (DWORD)strlen(src6));

        int r6 = 0;
        BOOL ok6 = rewrite_step_products(f6, L"TOP_ASM", L"\x603B\x88C5", NULL, &r6);
        check(ok6 && r6 == 1, "suffixless FORMATION still links PD -> PRODUCT");

        DWORD l6 = 0;
        BYTE *g6 = read_all_bytes(f6, &l6);
        check(g6 && strstr((char *)g6, "#21=PRODUCT('\\X2\\603B88C5\\X0\\'") != NULL,
              "top product renamed via suffixless FORMATION");
        check(g6 && strstr((char *)g6, "#31=PRODUCT('CHILD'") != NULL,
              "sibling product left untouched");
        free(g6);
    }

    /* 真实 Creo 导出件的冒烟测试（由 test.bat 决定是否提供） */
    wprintf(L"\n[4] real Creo assembly fixture\n");
    const wchar_t *real = L"build\\selftest_real.stp";
    if (GetFileAttributesW(real) == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"  SKIP  build\\selftest_real.stp not present\n");
    } else {
        int rr = 0;
        BOOL rok = rewrite_step_products(real, L"0005_ASM", L"\x603B\x88C5\x4F53",
                                         NULL, &rr);
        check(rok && rr > 0, "real file rewritten");
        wprintf(L"        renamed %d products\n", rr);

        DWORD lr = 0;
        BYTE *gr = read_all_bytes(real, &lr);
        check(gr != NULL, "real file still readable");
        if (gr) {
            check(strstr((char *)gr, "H20-20-0010001_101") == NULL ||
                  strstr((char *)gr, "ISO-10303-21;") != NULL,
                  "real file structure preserved");
            check(strstr((char *)gr, "END-ISO-10303-21;") != NULL,
                  "real file trailer intact");
            free(gr);
        }
    }

    wprintf(L"\n%s  (%d failure%hs)\n",
            g_fail ? L"*** SELFTEST FAILED ***" : L"SELFTEST PASSED",
            g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}

#endif /* CN_STEP_SELFTEST */

/* ==================================================================
 * 第三部分：独立命令行工具（CN_STEP_TOOL）
 *
 * 与 Creo 完全无关：不加载 DLL、不调用 Pro/TOOLKIT、静态链接 CRT，
 * 任何一台 Windows 机器拷过去就能跑，也不挑 Creo 版本。
 *
 *   step_cn.exe <输入.stp> [顶层中文名] [选项]
 *
 * 选项：
 *   --map <文件.csv>   名称对照表（两列：导出名,中文名）
 *   --out <目录>       输出目录，默认与输入同目录
 *   --no-creo          不生成 _creo 版（默认生成）
 *   --ansi             对照表按 ANSI 读，默认 UTF-8
 *   --quiet            只输出错误
 * ================================================================== */
#ifdef CN_STEP_TOOL

#define TOOL_MAXMAP 512

static wchar_t g_map_key[TOOL_MAXMAP][128];
static wchar_t g_map_val[TOOL_MAXMAP][128];
static int     g_map_n = 0;

/* 本地版本：工具模式不编译 Creo 侧那套配置代码 */
static void bytes_to_wstr(const BYTE *b, DWORD len, int prefer_utf8,
                          wchar_t *out, int cap)
{
    UINT cp = prefer_utf8 ? CP_UTF8 : CP_ACP;
    int need = MultiByteToWideChar(cp, 0, (LPCCH)b, (int)len, NULL, 0);
    if (need <= 0 && prefer_utf8) {
        cp = CP_ACP;
        need = MultiByteToWideChar(cp, 0, (LPCCH)b, (int)len, NULL, 0);
    }
    if (need <= 0) { out[0] = 0; return; }
    if (need >= cap) need = cap - 1;
    MultiByteToWideChar(cp, 0, (LPCCH)b, (int)len, out, need);
    out[need] = 0;
}

/* --diag：只打印 STEP 内部索引统计，不改文件，用来定位解析问题 */
static void tool_diag(const wchar_t *path)
{
    DWORD len = 0;
    BYTE *buf = read_all_bytes(path, &len);
    if (!buf) { printf("[diag] 读不到文件\n"); return; }

    P21Index ix;
    p21_index(buf, len, &ix);

    printf("[diag] bytes=%lu  product=%zu  pd=%zu  formation=%zu  nauo=%zu  shape=%zu\n",
           (unsigned long)len, ix.nprod, ix.npdf, ix.nfp, ix.nnauo, ix.nshp);

    size_t k;
    for (k = 0; k < ix.nprod && k < 20; k++) {
        char id[256];
        DWORD n = ix.prod[k].idlen < 255 ? ix.prod[k].idlen : 255;
        memcpy(id, buf + ix.prod[k].ids, n);
        id[n] = 0;
        printf("        product #%d id=%s\n", ix.prod[k].product, id);
    }
    for (k = 0; k < ix.nnauo && k < 10; k++) {
        printf("        nauo child_pd=%d parent_pd=%d desc_len=%lu\n",
               ix.nauo[k].pd, ix.nauo[k].parent,
               (unsigned long)ix.nauo[k].dlen);
    }
    p21_free_index(&ix);
    free(buf);
}

static void tool_load_map(const wchar_t *path, int utf8)
{
    DWORD len = 0;
    BYTE *raw = read_all_bytes(path, &len);
    if (!raw) return;

    size_t wcap = (size_t)len * 2 + 8;
    wchar_t *text = (wchar_t *)malloc(sizeof(wchar_t) * wcap);
    if (!text) { free(raw); return; }

    bytes_to_wstr(raw, len, utf8, text, (int)wcap);
    wchar_t *line = wcstok(text, L"\r\n", NULL);
    while (line && g_map_n < TOOL_MAXMAP) {
        if (line[0] == 0xFEFF) line++;
        wchar_t *sep = wcschr(line, L',');
        if (sep) {
            *sep = 0;
            wchar_t *k = line, *v = sep + 1;
            while (*k == L' ' || *k == L'\t') k++;
            while (*v == L' ' || *v == L'\t') v++;
            if (*k && *v) {
                wcsncpy_s(g_map_key[g_map_n], 128, k, _TRUNCATE);
                wcsncpy_s(g_map_val[g_map_n], 128, v, _TRUNCATE);
                g_map_n++;
            }
        }
        line = wcstok(NULL, L"\r\n", NULL);
    }
    free(text);
    free(raw);
}

static int tool_lookup(const wchar_t *key, wchar_t *out, int cap)
{
    int i;
    for (i = 0; i < g_map_n; i++) {
        if (_wcsicmp(g_map_key[i], key) == 0) {
            wcsncpy_s(out, (size_t)cap, g_map_val[i], _TRUNCATE);
            return 1;
        }
    }
    return 0;
}

/* 控制台输出宽字符串：先转 UTF-8，避免 %ls 在 "C" locale 下输出不全 */
static void pw(const char *label, const wchar_t *v)
{
    char buf[MAX_PATH * 4];
    int n = WideCharToMultiByte(CP_UTF8, 0, v, -1, buf, (int)sizeof(buf), NULL, NULL);
    if (n <= 0) buf[0] = 0;
    printf("%s%s\n", label, buf);
}

static void tool_help(void)
{
    printf(
        "step_cn - 把 STEP 文件的中文名写好，并生成两份：\n"
        "          <名字>.stp        ISO 标准 \\X2\\ 转义，给 SolidWorks 等\n"
        "          <名字>_creo.stp   UTF-8 原始字节，给 Creo 回读\n"
        "\n"
        "用法: step_cn.exe <文件.stp> [顶层中文名] [选项]\n"
        "\n"
        "  不给中文名时：先用 STEP 内部已有的中文；再查对照表；\n"
        "  都没有就保持原名，只做格式转换。\n"
        "\n"
        "选项:\n"
        "  --map <文件.csv>   名称对照表，两列：导出名,中文名\n"
        "  --out <目录>       输出目录，默认与输入文件同目录\n"
        "  --no-creo          不生成 _creo 版\n"
        "  --ansi             对照表按 ANSI(GBK) 读，默认 UTF-8\n"
        "  --quiet            只输出错误\n"
        "\n"
        "把文件直接拖到 step_cn.exe 上也能用。\n");
}

/* 判断路径是否以指定扩展名结尾（不区分大小写） */
static int tool_has_ext(const wchar_t *p, const wchar_t *ext)
{
    size_t lp = wcslen(p), le = wcslen(ext);
    if (lp < le) return 0;
    return _wcsicmp(p + lp - le, ext) == 0;
}

int main(int argc, char **argv)
{
    SetConsoleOutputCP(65001);

    if (argc < 2) { tool_help(); return 2; }

    wchar_t wsrc[MAX_PATH * 2]  = L"";
    wchar_t wtop[MAX_PATH * 2]  = L"";
    wchar_t wmap[MAX_PATH * 2]  = L"";
    wchar_t wout[MAX_PATH * 2]  = L"";
    int map_utf8 = 1, want_creo = 1, quiet = 0, diag = 0;
    int i, got_top = 0;

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        tool_help();
        return 0;
    }
    if (argv[1][0] == '-') {          /* 第一参数必须是文件 */
        tool_help();
        return 2;
    }
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wsrc, MAX_PATH * 2);

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, wmap, MAX_PATH * 2);
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            MultiByteToWideChar(CP_ACP, 0, argv[++i], -1, wout, MAX_PATH * 2);
        } else if (strcmp(argv[i], "--no-creo") == 0) {
            want_creo = 0;
        } else if (strcmp(argv[i], "--ansi") == 0) {
            map_utf8 = 0;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--diag") == 0) {
            diag = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            tool_help();
            return 0;
        } else if (argv[i][0] != '-' && !got_top) {
            MultiByteToWideChar(CP_ACP, 0, argv[i], -1, wtop, MAX_PATH * 2);
            got_top = 1;
        }
    }

    if (GetFileAttributesW(wsrc) == INVALID_FILE_ATTRIBUTES) {
        pw("[ERROR] 找不到文件: ", wsrc);
        return 1;
    }
    if (!tool_has_ext(wsrc, L".stp") && !tool_has_ext(wsrc, L".step")) {
        fprintf(stderr, "[ERROR] 只接受 .stp / .step 文件\n");
        return 1;
    }

    if (diag) { tool_diag(wsrc); return 0; }

    if (wmap[0]) tool_load_map(wmap, map_utf8);

    /* 源文件的目录与基名 */
    wchar_t dir[MAX_PATH * 2];
    wcscpy_s(dir, MAX_PATH * 2, wsrc);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *(slash + 1) = 0; else wcscpy_s(dir, MAX_PATH * 2, L".\\");

    const wchar_t *base = wcsrchr(wsrc, L'\\');
    base = base ? base + 1 : wsrc;
    wchar_t stem[MAX_PATH * 2];
    wcsncpy_s(stem, MAX_PATH * 2, base, _TRUNCATE);
    wchar_t *dot = wcsrchr(stem, L'.');
    if (dot) *dot = 0;

    /* 统一成"以反斜杠结尾"的目录形式，拼文件名时才不会粘在一起 */
    wchar_t outdir[MAX_PATH * 2];
    wcsncpy_s(outdir, MAX_PATH * 2, wout[0] ? wout : dir, _TRUNCATE);
    size_t odl = wcslen(outdir);
    if (odl > 0 && outdir[odl - 1] != L'\\') {
        wcscat_s(outdir, MAX_PATH * 2, L"\\");
    }
    if (CreateDirectoryW(outdir, NULL) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        printf("[ERROR] 无法创建输出目录: %ls\n", outdir);
        return 1;
    }

    /* 输出文件名：给了中文名就用它，否则沿用原名 */
    wchar_t wdst[MAX_PATH * 2];
    _snwprintf_s(wdst, MAX_PATH * 2, _TRUNCATE, L"%s%s.stp",
                 outdir, got_top ? wtop : stem);

    int same_file = (_wcsicmp(wdst, wsrc) == 0);

    if (!same_file) {
        if (GetFileAttributesW(wdst) != INVALID_FILE_ATTRIBUTES) {
            int n;
            for (n = 2; n < 100; n++) {
                wchar_t alt[MAX_PATH * 2];
                _snwprintf_s(alt, MAX_PATH * 2, _TRUNCATE, L"%s%s_%d.stp",
                             outdir, got_top ? wtop : stem, n);
                if (GetFileAttributesW(alt) == INVALID_FILE_ATTRIBUTES) {
                    wcscpy_s(wdst, MAX_PATH * 2, alt);
                    break;
                }
            }
        }
        if (!CopyFileW(wsrc, wdst, FALSE)) {
            printf("[ERROR] 复制失败 (%lu)\n", GetLastError()); pw("        路径: ", wdst);
            return 1;
        }
    }

    DWORD t0 = GetTickCount();

    /* 顶层中文名：命令行给的 > 对照表里的 > 保持原样 */
    wchar_t wtop_cn[MAX_PATH * 2];
    wcscpy_s(wtop_cn, MAX_PATH * 2, got_top ? wtop : L"");
    if (!wtop_cn[0]) {
        wchar_t tmp[128];
        if (tool_lookup(stem, tmp, 128)) wcscpy_s(wtop_cn, MAX_PATH * 2, tmp);
    }

    int renamed = 0;
    BOOL ok = rewrite_step_products(wdst, stem, wtop_cn, tool_lookup, &renamed);

    /* Creo 回读版 */
    wchar_t wcreo[MAX_PATH * 2];
    wcreo[0] = 0;
    int creo_ok = 0;
    if (ok && want_creo) {
        wcsncpy_s(wcreo, MAX_PATH * 2, wdst, _TRUNCATE);
        wchar_t *d2 = wcsrchr(wcreo, L'.');
        if (d2) *d2 = 0;
        wcscat_s(wcreo, MAX_PATH * 2, L"_creo.stp");
        creo_ok = write_creo_variant(wdst, wcreo) ? 1 : 0;
    }

    DWORD ms = GetTickCount() - t0;

    if (!quiet) {
        pw("输入 : ", wsrc);
        pw("输出 : ", wdst);
        printf("改写 : %d 个组件名%s\n", renamed,
               same_file ? "（原地改写）" : "");
        if (wtop_cn[0]) pw("顶层 : ", wtop_cn);
        if (want_creo) pw("Creo : ", creo_ok ? wcreo : L"（未生成）");
        printf("耗时 : %lu ms\n", (unsigned long)ms);
    }
    if (!ok) printf("[ERROR] STEP 改写失败\n");
    return ok ? 0 : 1;
}

#endif /* CN_STEP_TOOL */
