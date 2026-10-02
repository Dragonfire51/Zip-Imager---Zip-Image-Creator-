/*
 * Zip Disk Image Creator
 * Creates raw disk images of Iomega Zip 100 / 250 / 750 media (FAT16).
 * Pure Win32 API, ANSI build (no UNICODE) for Windows 95 .. Windows 11.
 *
 * Build (MinGW):
 *   g++ -O2 -s -mwindows -static -static-libgcc -static-libstdc++ ZipImager.cpp \
 *       -o ZipImager.exe -lcomctl32 -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32
 *
 * For real Windows 95/98/ME support use an old MinGW toolchain (mingw.org, GCC 3.x-4.x).
 * Modern MinGW-w64 binaries need Windows XP/Vista or newer.
 */
#define WINVER 0x0400
#define _WIN32_WINNT 0x0400
#define _WIN32_IE 0x0300
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

typedef unsigned char u8;

#define APP_NAME "Zip Disk Image Creator"
#define APP_VERSION "1.0.0"

/* ---------------------------------------------------------------- media */
struct ZipType { const char* name; DWORD sectors; DWORD partStart; };
/* Sector counts of the real media. Edit here if you need different values. */
static const ZipType ZT[] = {
    { "Zip 100 (100 MB)", 196608UL,  32 },
    { "Zip 250 (250 MB)", 489532UL,  32 },
    { "Zip 750 (750 MB)", 1498176UL, 32 }
};
#define ZT_COUNT 3

struct Geo { DWORD P, total, partSec, spc, fatSec, clusters, rootSec; };

static void MakeGeo(int t, bool mbr, Geo& g) {
    g.P = mbr ? ZT[t].partStart : 0;
    g.total = ZT[t].sectors;
    g.partSec = g.total - g.P;
    g.rootSec = 32; /* 512 root entries */
    for (g.spc = 1; g.spc <= 64; g.spc *= 2) {
        DWORD fat = 1;
        for (int i = 0; i < 6; i++) {
            g.clusters = (g.partSec - 1 - g.rootSec - 2 * fat) / g.spc;
            fat = ((g.clusters + 2) * 2 + 511) / 512;
        }
        g.fatSec = fat;
        g.clusters = (g.partSec - 1 - g.rootSec - 2 * fat) / g.spc;
        if (g.clusters <= 65000UL) break;
    }
}

/* ----------------------------------------------------------- file tree */
struct Node {
    std::string name;
    bool dir;
    std::string src;
    DWORD size;
    WORD dDate, dTime;
    Node* parent;
    std::vector<Node*> kids;
    char sfn[11];
    int lfn;
    DWORD first, nclu;
    bool fromImg;               /* data lives inside the opened image */
    std::vector<DWORD> chain;   /* cluster chain in the opened image  */
    Node() : dir(false), size(0), dDate(0), dTime(0), parent(0), lfn(0), first(0), nclu(0), fromImg(false) {
        memset(sfn, ' ', 11);
    }
};

static void FreeNode(Node* n) {
    for (size_t i = 0; i < n->kids.size(); i++) FreeNode(n->kids[i]);
    delete n;
}

static const char* SPECIAL = "$%'-_@~`!(){}^#&";
static bool ExactChar(unsigned char c) {
    return c < 128 && (isupper(c) || isdigit(c) || (c && strchr(SPECIAL, c)));
}
static char MangleChar(unsigned char c) {
    if (c < 128 && (isalnum(c) || (c && strchr(SPECIAL, c)))) return (char)toupper(c);
    return '_';
}
static int WideLen(const std::string& s) {
    WCHAR w[300];
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w, 256);
    return n > 0 ? n - 1 : (int)s.size();
}

static void NameKids(Node* d) {
    std::vector<std::string> used;
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < d->kids.size(); i++) {
            Node* k = d->kids[i];
            const std::string& n = k->name;
            size_t dot = n.rfind('.');
            std::string b, e;
            if (dot == std::string::npos || dot == 0) b = n; else { b = n.substr(0, dot); e = n.substr(dot + 1); }
            bool exact = !b.empty() && b.size() <= 8 && e.size() <= 3;
            for (size_t j = 0; exact && j < b.size(); j++) if (!ExactChar((u8)b[j])) exact = false;
            for (size_t j = 0; exact && j < e.size(); j++) if (!ExactChar((u8)e[j])) exact = false;
            if (pass == 0) {
                if (!exact) continue;
                memset(k->sfn, ' ', 11);
                memcpy(k->sfn, b.c_str(), b.size());
                memcpy(k->sfn + 8, e.c_str(), e.size());
                k->lfn = 0;
                used.push_back(std::string(k->sfn, 11));
            } else {
                if (exact) continue;
                std::string base, ext;
                for (size_t j = 0; j < b.size(); j++) if (b[j] != ' ' && b[j] != '.') base += MangleChar((u8)b[j]);
                for (size_t j = 0; j < e.size() && ext.size() < 3; j++) if (e[j] != ' ' && e[j] != '.') ext += MangleChar((u8)e[j]);
                if (base.empty()) base = "_";
                for (int t = 1;; t++) {
                    char tail[16]; sprintf(tail, "~%d", t);
                    std::string cand = base.substr(0, 8 - strlen(tail)) + tail;
                    char s[11]; memset(s, ' ', 11);
                    memcpy(s, cand.c_str(), cand.size());
                    memcpy(s + 8, ext.c_str(), ext.size());
                    std::string key(s, 11);
                    if (std::find(used.begin(), used.end(), key) == used.end()) {
                        used.push_back(key); memcpy(k->sfn, s, 11); break;
                    }
                }
                int wl = WideLen(n); if (wl > 255) wl = 255;
                k->lfn = (wl + 12) / 13;
            }
        }
    }
}

static bool PlanDir(Node* d, bool isRoot, DWORD cs, DWORD& next, std::vector<Node*>& ord, std::string& err) {
    NameKids(d);
    DWORD ents = isRoot ? 1 : 2;
    for (size_t i = 0; i < d->kids.size(); i++) ents += 1 + d->kids[i]->lfn;
    if (isRoot) {
        if (ents > 512) { err = "Too many entries in the root folder (max. 511 including long names)."; return false; }
    } else {
        d->nclu = (ents * 32 + cs - 1) / cs;
        d->first = next; next += d->nclu; ord.push_back(d);
    }
    for (size_t i = 0; i < d->kids.size(); i++) {
        Node* k = d->kids[i];
        if (k->dir) { if (!PlanDir(k, false, cs, next, ord, err)) return false; }
        else {
            k->nclu = (k->size + cs - 1) / cs;
            if (next + k->nclu > 70000UL) { err = "Not enough space on the selected Zip disk."; return false; }
            k->first = k->nclu ? next : 0;
            next += k->nclu;
            if (k->nclu) ord.push_back(k);
        }
    }
    return true;
}

static bool Plan(Node* root, const Geo& g, std::vector<Node*>& ord, std::string& err, DWORD* usedClusters) {
    DWORD next = 2;
    if (!PlanDir(root, true, g.spc * 512, next, ord, err)) return false;
    if (usedClusters) *usedClusters = next - 2;
    if (next - 2 > g.clusters) { err = "Not enough space on the selected Zip disk."; return false; }
    return true;
}

/* ------------------------------------------------------ image writing */
static void P16(u8* p, WORD v) { p[0] = (u8)(v & 255); p[1] = (u8)(v >> 8); }
static void P32(u8* p, DWORD v) { P16(p, (WORD)(v & 0xFFFF)); P16(p + 2, (WORD)(v >> 16)); }

static WORD g_nowDate, g_nowTime;

static void ShortEntry(u8* e, const char* sfn, u8 attr, WORD date, WORD time, DWORD first, DWORD size) {
    memset(e, 0, 32);
    memcpy(e, sfn, 11);
    e[11] = attr;
    P16(e + 14, time); P16(e + 16, date); P16(e + 18, date);
    P16(e + 20, (WORD)(first >> 16));
    P16(e + 22, time); P16(e + 24, date);
    P16(e + 26, (WORD)(first & 0xFFFF));
    P32(e + 28, size);
}

static void BuildDir(Node* d, bool isRoot, const char* label, DWORD cs, std::vector<u8>& buf) {
    size_t total = isRoot ? 16384 : (size_t)d->nclu * cs;
    buf.assign(total, 0);
    u8* p = &buf[0];
    if (isRoot) {
        ShortEntry(p, label, 0x08, g_nowDate, g_nowTime, 0, 0); p += 32;
    } else {
        DWORD pc = (d->parent && d->parent->parent) ? d->parent->first : 0;
        ShortEntry(p, ".          ", 0x10, d->dDate, d->dTime, d->first, 0); p += 32;
        ShortEntry(p, "..         ", 0x10, d->dDate, d->dTime, pc, 0); p += 32;
    }
    static const int off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (size_t i = 0; i < d->kids.size(); i++) {
        Node* k = d->kids[i];
        if (k->lfn) {
            WCHAR w[300];
            int wl = MultiByteToWideChar(CP_ACP, 0, k->name.c_str(), -1, w, 256) - 1;
            if (wl < 0) wl = 0; if (wl > 255) wl = 255;
            u8 chk = 0;
            for (int c = 0; c < 11; c++) chk = (u8)(((chk & 1) ? 0x80 : 0) + (chk >> 1) + (u8)k->sfn[c]);
            for (int n = k->lfn; n >= 1; n--) {
                memset(p, 0, 32);
                p[0] = (u8)(n | (n == k->lfn ? 0x40 : 0));
                p[11] = 0x0F; p[13] = chk;
                for (int c = 0; c < 13; c++) {
                    int j = (n - 1) * 13 + c;
                    WORD ch = j < wl ? (WORD)w[j] : (j == wl ? 0 : 0xFFFF);
                    P16(p + off[c], ch);
                }
                p += 32;
            }
        }
        ShortEntry(p, k->sfn, k->dir ? 0x10 : 0x20, k->dDate, k->dTime, k->first, k->dir ? 0 : k->size);
        p += 32;
    }
}

/* progress / UI hooks */
static HWND hMain, hList, hCombo, hLabel, hProg, hStatus, hPathLbl;
static unsigned long long g_done, g_total, g_lastUI;

static void Pump() {
    MSG m;
    while (PeekMessage(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessage(&m); }
}
static void UpdateProg() {
    int pct = g_total ? (int)(g_done * 100 / g_total) : 100;
    SendMessage(hProg, PBM_SETPOS, pct, 0);
    char t[64]; sprintf(t, " Saving image... %d%%", pct);
    SendMessage(hStatus, SB_SETTEXT, 0, (LPARAM)t);
    Pump();
}
static bool Wr(HANDLE h, const void* p, DWORD n) {
    DWORD w;
    if (!WriteFile(h, p, n, &w, 0) || w != n) return false;
    g_done += n;
    if (g_done - g_lastUI >= (1UL << 20)) { g_lastUI = g_done; UpdateProg(); }
    return true;
}
static bool Zeros(HANDLE h, unsigned long long n) {
    static u8 z[65536];
    while (n) {
        DWORD c = n > sizeof(z) ? (DWORD)sizeof(z) : (DWORD)n;
        if (!Wr(h, z, c)) return false;
        n -= c;
    }
    return true;
}

static Node* root = 0;
static Node* cur = 0;
static std::string g_imgPath;          /* image the 'fromImg' nodes read from */
static DWORD g_srcDataOff = 0, g_srcCs = 0;

static bool RdAt(HANDLE h, DWORD off, void* buf, DWORD n) {
    if (SetFilePointer(h, (LONG)off, 0, FILE_BEGIN) == 0xFFFFFFFFUL && GetLastError() != NO_ERROR) return false;
    DWORD r;
    return ReadFile(h, buf, n, &r, 0) && r == n;
}

static void NowDos() {
    SYSTEMTIME st; FILETIME ft;
    GetLocalTime(&st); SystemTimeToFileTime(&st, &ft);
    FileTimeToDosDateTime(&ft, &g_nowDate, &g_nowTime);
}

static bool WriteImage(const char* path, int t, bool mbr, std::string lbl, std::string& err) {
    Geo g; MakeGeo(t, mbr, g);
    DWORD cs = g.spc * 512;
    std::vector<Node*> ord;
    if (!Plan(root, g, ord, err, 0)) return false;
    NowDos();

    char label[11]; memset(label, ' ', 11);
    std::string L;
    for (size_t i = 0; i < lbl.size() && L.size() < 11; i++) L += MangleChar((u8)lbl[i]) == '_' && lbl[i] != '_' ? '_' : (char)toupper((u8)lbl[i]);
    if (L.empty()) L = "ZIPDISK";
    memcpy(label, L.c_str(), L.size());

    HANDLE h = CreateFile(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { err = "Cannot create the output file."; return false; }
    g_done = 0; g_lastUI = 0; g_total = (unsigned long long)g.total * 512;
    bool ok = true;

    if (mbr) {
        u8 m[512]; memset(m, 0, 512);
        u8* pe = m + 0x1BE + 16 * 3; /* Zip disks use partition #4 */
        pe[0] = 0x80; pe[1] = 0xFE; pe[2] = 0xFF; pe[3] = 0xFF; pe[4] = 0x06;
        pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
        P32(pe + 8, g.P); P32(pe + 12, g.partSec);
        m[510] = 0x55; m[511] = 0xAA;
        ok = Wr(h, m, 512) && Zeros(h, (unsigned long long)(g.P - 1) * 512);
    }
    if (ok) {
        u8 b[512]; memset(b, 0, 512);
        b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
        memcpy(b + 3, "MSDOS5.0", 8);
        P16(b + 11, 512); b[13] = (u8)g.spc; P16(b + 14, 1); b[16] = 2; P16(b + 17, 512);
        P16(b + 19, g.partSec < 65536UL ? (WORD)g.partSec : 0);
        b[21] = 0xF8; P16(b + 22, (WORD)g.fatSec); P16(b + 24, 32); P16(b + 26, 64);
        P32(b + 28, g.P);
        P32(b + 32, g.partSec >= 65536UL ? g.partSec : 0);
        b[36] = 0x80; b[38] = 0x29; P32(b + 39, GetTickCount());
        memcpy(b + 43, label, 11); memcpy(b + 54, "FAT16   ", 8);
        b[510] = 0x55; b[511] = 0xAA;
        ok = Wr(h, b, 512);
    }
    if (ok) {
        std::vector<WORD> fat((size_t)g.fatSec * 256, 0);
        fat[0] = 0xFFF8; fat[1] = 0xFFFF;
        for (size_t i = 0; i < ord.size(); i++) {
            Node* n = ord[i];
            for (DWORD c = 0; c < n->nclu; c++)
                fat[n->first + c] = (c == n->nclu - 1) ? 0xFFFF : (WORD)(n->first + c + 1);
        }
        ok = Wr(h, &fat[0], g.fatSec * 512) && Wr(h, &fat[0], g.fatSec * 512);
    }
    if (ok) {
        std::vector<u8> buf;
        BuildDir(root, true, label, cs, buf);
        ok = Wr(h, &buf[0], (DWORD)buf.size());
        for (size_t i = 0; ok && i < ord.size(); i++) {
            Node* n = ord[i];
            if (n->dir) {
                BuildDir(n, false, label, cs, buf);
                ok = Wr(h, &buf[0], (DWORD)buf.size());
            } else {
                const std::string& rp = n->fromImg ? g_imgPath : n->src;
                HANDLE f = CreateFile(rp.c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
                if (f == INVALID_HANDLE_VALUE) { err = "Cannot read: " + rp; ok = false; break; }
                static u8 chunk[65536];
                DWORD left = n->size; size_t ci = 0;
                while (left && ok) {
                    DWORD want = left > sizeof(chunk) ? (DWORD)sizeof(chunk) : left, got = 0;
                    if (n->fromImg) {
                        if (want > g_srcCs) want = g_srcCs;
                        if (ci >= n->chain.size() || !RdAt(f, g_srcDataOff + (n->chain[ci] - 2) * g_srcCs, chunk, want))
                            memset(chunk, 0, want);
                        ci++; got = want;
                    } else if (!ReadFile(f, chunk, want, &got, 0) || got == 0) { memset(chunk, 0, want); got = want; }
                    ok = Wr(h, chunk, got);
                    left -= got;
                }
                CloseHandle(f);
                if (ok) ok = Zeros(h, (unsigned long long)n->nclu * cs - n->size);
            }
        }
    }
    if (ok) {
        unsigned long long rest = g_total > g_done ? g_total - g_done : 0;
        ok = Zeros(h, rest);
    }
    CloseHandle(h);
    if (!ok) {
        if (err.empty()) err = "Write error (disk full or file locked?).";
        DeleteFile(path);
    }
    return ok;
}

/* ------------------------------------------------------------- GUI */
#define ID_NEW     100
#define ID_SAVE    101
#define ID_SAVEAS  102
#define ID_EXIT    103
#define ID_OPEN    104
#define ID_ADDF    110
#define ID_ADDD    111
#define ID_DEL     112
#define ID_UP      113
#define ID_SELALL  114
#define ID_NEWF    115
#define ID_RENAME  116
#define ID_MBR     120
#define ID_ABOUT   130
#define ID_COMBO   200
#define ID_LABEL   201
#define ID_LIST    202

static char curFile[MAX_PATH] = "";
static bool dirty = false;
static bool useMbr = true;
static int g_skipped = 0;

static std::string FmtSize(double b) {
    char t[64];
    if (b >= 1048576.0) sprintf(t, "%.2f MB", b / 1048576.0);
    else if (b >= 1024.0) sprintf(t, "%.1f KB", b / 1024.0);
    else sprintf(t, "%.0f bytes", b);
    return t;
}

static void SetTitle() {
    std::string t = APP_NAME " - ";
    const char* n = curFile[0] ? strrchr(curFile, '\\') : 0;
    t += curFile[0] ? (n ? n + 1 : curFile) : "Untitled";
    if (dirty) t += " *";
    SetWindowText(hMain, t.c_str());
}

static bool NameLess(Node* a, Node* b) {
    if (a->dir != b->dir) return a->dir;
    return _stricmp(a->name.c_str(), b->name.c_str()) < 0;
}

static void UpdateCapacity() {
    int t = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0); if (t < 0) t = 0;
    Geo g; MakeGeo(t, useMbr, g);
    std::vector<Node*> ord; std::string err; DWORD used = 0;
    bool ok = Plan(root, g, ord, err, &used);
    char txt[200];
    DWORD cs = g.spc * 512;
    if (ok) {
        sprintf(txt, " Used: %s of %s   (cluster size %lu bytes)", FmtSize((double)used * cs).c_str(),
                FmtSize((double)g.clusters * cs).c_str(), (unsigned long)cs);
        SendMessage(hProg, PBM_SETPOS, (WPARAM)(g.clusters ? (unsigned long long)used * 100 / g.clusters : 0), 0);
    } else {
        sprintf(txt, " %s", err.c_str());
        SendMessage(hProg, PBM_SETPOS, 100, 0);
    }
    SendMessage(hStatus, SB_SETTEXT, 0, (LPARAM)txt);
}

static void Refresh() {
    ListView_DeleteAllItems(hList);
    std::sort(cur->kids.begin(), cur->kids.end(), NameLess);
    int row = 0;
    LVITEM it;
    if (cur != root) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row; it.pszText = (char*)".."; it.lParam = 0;
        ListView_InsertItem(hList, &it);
        ListView_SetItemText(hList, row, 2, (char*)"Parent folder");
        row++;
    }
    for (size_t i = 0; i < cur->kids.size(); i++, row++) {
        Node* k = cur->kids[i];
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = row;
        it.pszText = (char*)k->name.c_str(); it.lParam = (LPARAM)k;
        ListView_InsertItem(hList, &it);
        if (!k->dir) ListView_SetItemText(hList, row, 1, (char*)FmtSize((double)k->size).c_str());
        ListView_SetItemText(hList, row, 2, (char*)(k->dir ? "Folder" : "File"));
    }
    std::string p = "";
    for (Node* n = cur; n && n != root; n = n->parent) p = "\\" + n->name + p;
    p = " Zip disk:" + (p.empty() ? std::string("\\") : p);
    SetWindowText(hPathLbl, p.c_str());
    UpdateCapacity();
    SetTitle();
}

static void NewImage() {
    if (root) FreeNode(root);
    root = new Node(); root->dir = true; cur = root;
    curFile[0] = 0; dirty = false; g_imgPath = "";
}

/* add a file/folder from disk into 'parent' */
static void AddItem(Node* parent, const std::string& full) {
    WIN32_FIND_DATA fd;
    HANDLE h = FindFirstFile(full.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { g_skipped++; return; }
    FindClose(h);
    std::string nm = fd.cFileName;
    for (size_t i = 0; i < parent->kids.size(); i++)
        if (_stricmp(parent->kids[i]->name.c_str(), nm.c_str()) == 0) { g_skipped++; return; }
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.nFileSizeHigh) { g_skipped++; return; }
    Node* n = new Node();
    n->name = nm; n->parent = parent; n->src = full;
    n->dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    n->size = n->dir ? 0 : fd.nFileSizeLow;
    FILETIME lf; FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lf);
    if (!FileTimeToDosDateTime(&lf, &n->dDate, &n->dTime)) { n->dDate = 0x21; n->dTime = 0; }
    parent->kids.push_back(n);
    if (n->dir) {
        WIN32_FIND_DATA f2;
        HANDLE hf = FindFirstFile((full + "\\*").c_str(), &f2);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(f2.cFileName, ".") || !strcmp(f2.cFileName, "..")) continue;
                AddItem(n, full + "\\" + f2.cFileName);
            } while (FindNextFile(hf, &f2));
            FindClose(hf);
        }
    }
}

static void AfterAdd() {
    dirty = true;
    Refresh();
    if (g_skipped) {
        char t[128];
        sprintf(t, "%d item(s) were skipped (duplicate name, unreadable or larger than 4 GB).", g_skipped);
        MessageBox(hMain, t, APP_NAME, MB_OK | MB_ICONINFORMATION);
        g_skipped = 0;
    }
}

static void CmdAddFiles() {
    static char buf[65536]; buf[0] = 0;
    OPENFILENAME o; memset(&o, 0, sizeof(o));
#ifdef OPENFILENAME_SIZE_VERSION_400A
    o.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
#else
    o.lStructSize = sizeof(o);
#endif
    o.hwndOwner = hMain; o.lpstrFilter = "All files (*.*)\0*.*\0";
    o.lpstrFile = buf; o.nMaxFile = sizeof(buf); o.lpstrTitle = "Add Files";
    o.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileName(&o)) return;
    std::string dir = buf;
    const char* p = buf + dir.size() + 1;
    if (!*p) AddItem(cur, dir);
    else {
        if (dir[dir.size() - 1] != '\\') dir += "\\";
        while (*p) { AddItem(cur, dir + p); p += strlen(p) + 1; }
    }
    AfterAdd();
}

static void CmdAddFolder() {
    char path[MAX_PATH];
    BROWSEINFO bi; memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = hMain; bi.lpszTitle = "Select a folder to add:"; bi.ulFlags = BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
    if (!pidl) return;
    BOOL ok = SHGetPathFromIDList(pidl, path);
    CoTaskMemFree(pidl);
    if (!ok) return;
    std::string s = path;
    if (s.size() > 3 && s[s.size() - 1] == '\\') s.erase(s.size() - 1);
    AddItem(cur, s);
    AfterAdd();
}

static void CmdDelete() {
    std::vector<Node*> del;
    int i = -1;
    while ((i = ListView_GetNextItem(hList, i, LVNI_SELECTED)) != -1) {
        LVITEM it; memset(&it, 0, sizeof(it));
        it.mask = LVIF_PARAM; it.iItem = i;
        ListView_GetItem(hList, &it);
        if (it.lParam) del.push_back((Node*)it.lParam);
    }
    if (del.empty()) return;
    for (size_t j = 0; j < del.size(); j++) {
        cur->kids.erase(std::find(cur->kids.begin(), cur->kids.end(), del[j]));
        FreeNode(del[j]);
    }
    dirty = true; Refresh();
}

static void CmdUp() { if (cur != root) { cur = cur->parent; Refresh(); } }

/* ------------------------------------------------ opening existing images */
static WORD R16(const u8* p) { return (WORD)(p[0] | (p[1] << 8)); }
static DWORD R32(const u8* p) { return (DWORD)R16(p) | ((DWORD)R16(p + 2) << 16); }

struct Src { HANDLE h; DWORD cs, dataOff, clusters; std::vector<WORD> fat; };

static bool IsBoot(const u8* b) {
    return b[510] == 0x55 && b[511] == 0xAA && R16(b + 11) == 512 && b[13] && (b[13] & (b[13] - 1)) == 0 && b[16] > 0;
}
static void ChainOf(const Src& s, DWORD first, std::vector<DWORD>& ch) {
    ch.clear();
    DWORD c = first;
    while (c >= 2 && c < 0xFFF0UL && c - 2 < s.clusters && c < s.fat.size() && ch.size() <= s.clusters) {
        ch.push_back(c); c = s.fat[c];
    }
}
static void ReadChain(const Src& s, const std::vector<DWORD>& ch, std::vector<u8>& data) {
    data.assign(ch.size() * (size_t)s.cs, 0);
    for (size_t i = 0; i < ch.size(); i++) RdAt(s.h, s.dataOff + (ch[i] - 2) * s.cs, &data[i * s.cs], s.cs);
}

static void ParseDir(const Src& s, Node* d, const std::vector<u8>& data, int depth) {
    static const int LO[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    WCHAR lw[300]; bool haveL = false; u8 lchk = 0;
    for (size_t off = 0; off + 32 <= data.size(); off += 32) {
        const u8* e = &data[off];
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { haveL = false; continue; }
        u8 attr = e[11];
        if (attr == 0x0F) {
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) { haveL = true; lchk = e[13]; memset(lw, 0, sizeof(lw)); }
            if (haveL && ord >= 1 && ord <= 20)
                for (int c = 0; c < 13; c++) lw[(ord - 1) * 13 + c] = R16(e + LO[c]);
            continue;
        }
        if (attr & 0x08) { haveL = false; continue; }
        if (e[0] == '.') { haveL = false; continue; }
        std::string nm;
        u8 chk = 0;
        for (int c = 0; c < 11; c++) chk = (u8)(((chk & 1) ? 0x80 : 0) + (chk >> 1) + e[c]);
        if (haveL && chk == lchk) {
            int len = 0;
            while (len < 260 && lw[len] != 0 && lw[len] != 0xFFFF) len++;
            char out[600];
            int n = WideCharToMultiByte(CP_ACP, 0, lw, len, out, sizeof(out), 0, 0);
            if (n > 0) nm.assign(out, n);
        }
        if (nm.empty()) {
            char b[9], x[4];
            memcpy(b, e, 8); b[8] = 0; memcpy(x, e + 8, 3); x[3] = 0;
            if ((u8)b[0] == 0x05) b[0] = (char)0xE5;
            for (int i = 7; i >= 0 && b[i] == ' '; i--) b[i] = 0;
            for (int i = 2; i >= 0 && x[i] == ' '; i--) x[i] = 0;
            if (e[12] & 0x08) for (int i = 0; b[i]; i++) b[i] = (char)tolower((u8)b[i]);
            if (e[12] & 0x10) for (int i = 0; x[i]; i++) x[i] = (char)tolower((u8)x[i]);
            nm = b;
            if (x[0]) { nm += "."; nm += x; }
        }
        haveL = false;
        if (nm.empty()) continue;
        Node* k = new Node();
        k->name = nm; k->parent = d; k->fromImg = true;
        k->dir = (attr & 0x10) != 0;
        k->dTime = R16(e + 22); k->dDate = R16(e + 24);
        DWORD first = R16(e + 26) | ((DWORD)R16(e + 20) << 16);
        if (k->dir) {
            if (depth < 24 && first >= 2) {
                std::vector<DWORD> ch; std::vector<u8> sub;
                ChainOf(s, first, ch); ReadChain(s, ch, sub);
                ParseDir(s, k, sub, depth + 1);
            }
        } else {
            k->size = R32(e + 28);
            ChainOf(s, first, k->chain);
            DWORD cap = (DWORD)k->chain.size() * s.cs;
            if (k->size > cap) k->size = cap;
        }
        d->kids.push_back(k);
    }
}

#define FAILX(m) { CloseHandle(h); err = m; return false; }

static bool LoadImage(const char* path, std::string& err) {
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { err = "Cannot open the image file."; return false; }
    DWORD fsz = GetFileSize(h, 0);
    u8 b[512]; DWORD P = 0; bool mbr = false, ok = true;
    if (!RdAt(h, 0, b, 512)) ok = false;
    else if (!IsBoot(b)) {
        ok = false;
        if (b[510] == 0x55 && b[511] == 0xAA) {
            u8 m[512]; memcpy(m, b, 512);
            for (int i = 0; i < 4 && !ok; i++) {
                const u8* pe = m + 0x1BE + 16 * i;
                u8 ty = pe[4]; DWORD st = R32(pe + 8);
                if ((ty == 0x04 || ty == 0x06 || ty == 0x0E || ty == 0x14 || ty == 0x16) && st && st < fsz / 512 &&
                    RdAt(h, st * 512, b, 512) && IsBoot(b)) { P = st; mbr = true; ok = true; }
            }
        }
    }
    if (!ok) FAILX("Unrecognized image: no FAT16 boot sector or partition found.");
    Src s; s.h = h;
    DWORD spc = b[13], res = R16(b + 14), nf = b[16], rents = R16(b + 17), fatsz = R16(b + 22);
    DWORD tot = R16(b + 19) ? R16(b + 19) : R32(b + 32);
    if (!fatsz) FAILX("FAT32 images are not supported.");
    if (!rents) FAILX("Invalid boot sector.");
    DWORD rootSec = (rents * 32 + 511) / 512, dstart = res + nf * fatsz + rootSec;
    if (tot <= dstart) FAILX("Invalid boot sector.");
    s.clusters = (tot - dstart) / spc; s.cs = spc * 512;
    if (s.clusters < 4085 || s.clusters >= 65525UL) FAILX("Only FAT16 images are supported (FAT12 is not).");
    s.dataOff = (P + dstart) * 512;
    s.fat.assign((size_t)fatsz * 256, 0);
    if (!RdAt(h, (P + res) * 512, &s.fat[0], fatsz * 512)) FAILX("Cannot read the FAT (truncated image?).");
    std::vector<u8> rd((size_t)rents * 32, 0);
    if (!RdAt(h, (P + res + nf * fatsz) * 512, &rd[0], rents * 32)) FAILX("Cannot read the root directory.");

    char label[12] = "";
    for (size_t off = 0; off + 32 <= rd.size() && rd[off]; off += 32)
        if (rd[off] != 0xE5 && (rd[off + 11] & 0x0F) == 0x08) {
            memcpy(label, &rd[off], 11); label[11] = 0;
            for (int i = 10; i >= 0 && label[i] == ' '; i--) label[i] = 0;
            break;
        }
    Node* nr = new Node(); nr->dir = true;
    ParseDir(s, nr, rd, 0);
    CloseHandle(h);

    if (root) FreeNode(root);
    root = nr; cur = root;
    g_imgPath = path; g_srcDataOff = s.dataOff; g_srcCs = s.cs;
    DWORD ts = fsz / 512; int sel = ZT_COUNT - 1;
    for (int i = 0; i < ZT_COUNT; i++) if (ZT[i].sectors >= ts) { sel = i; break; }
    SendMessage(hCombo, CB_SETCURSEL, sel, 0);
    SetWindowText(hLabel, label);
    useMbr = mbr;
    CheckMenuItem(GetMenu(hMain), ID_MBR, MF_BYCOMMAND | (useMbr ? MF_CHECKED : MF_UNCHECKED));
    strcpy(curFile, path); dirty = false;
    return true;
}

static bool DoSave(const char* path) {
    int t = (int)SendMessage(hCombo, CB_GETCURSEL, 0, 0); if (t < 0) t = 0;
    char lb[64]; GetWindowText(hLabel, lb, sizeof(lb));
    std::string err;
    EnableWindow(hMain, FALSE);
    HCURSOR old = SetCursor(LoadCursor(0, IDC_WAIT));
    std::string tmp = std::string(path) + ".tmp";   /* never overwrite the source image directly */
    bool ok = WriteImage(tmp.c_str(), t, useMbr, lb, err);
    if (ok) {
        DeleteFile(path);
        if (!MoveFile(tmp.c_str(), path)) { err = "Could not replace the file. The new image was left as: " + tmp; ok = false; }
    }
    SetCursor(old);
    EnableWindow(hMain, TRUE);
    SetForegroundWindow(hMain);
    if (!ok) MessageBox(hMain, err.c_str(), APP_NAME, MB_OK | MB_ICONERROR);
    else {
        strcpy(curFile, path); dirty = false;
        if (!g_imgPath.empty()) {   /* nodes read from the old image: reload from the new one */
            std::vector<std::string> pn;
            for (Node* n = cur; n && n != root; n = n->parent) pn.insert(pn.begin(), n->name);
            std::string e2;
            if (LoadImage(path, e2)) {
                for (size_t i = 0; i < pn.size(); i++) {
                    Node* nx = 0;
                    for (size_t j = 0; j < cur->kids.size(); j++)
                        if (cur->kids[j]->dir && _stricmp(cur->kids[j]->name.c_str(), pn[i].c_str()) == 0) nx = cur->kids[j];
                    if (!nx) break;
                    cur = nx;
                }
            } else MessageBox(hMain, e2.c_str(), APP_NAME, MB_OK | MB_ICONWARNING);
        }
    }
    Refresh();
    if (ok) SendMessage(hStatus, SB_SETTEXT, 0, (LPARAM)" Image saved successfully.");
    return ok;
}

static bool CmdSaveAs() {
    char buf[MAX_PATH]; strcpy(buf, curFile);
    static const char* exts[] = { "img", "ima", "dsk", "bin" };
    OPENFILENAME o; memset(&o, 0, sizeof(o));
#ifdef OPENFILENAME_SIZE_VERSION_400A
    o.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
#else
    o.lStructSize = sizeof(o);
#endif
    o.hwndOwner = hMain;
    o.lpstrFilter = "Raw Zip image (*.img)\0*.img\0Raw Zip image (*.ima)\0*.ima\0"
                    "Disk image (*.dsk)\0*.dsk\0Binary image (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    o.lpstrFile = buf; o.nMaxFile = MAX_PATH; o.lpstrTitle = "Save As";
    o.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (!GetSaveFileName(&o)) return false;
    const char* base = strrchr(buf, '\\');
    if (!strchr(base ? base : buf, '.') && o.nFilterIndex >= 1 && o.nFilterIndex <= 4) {
        strcat(buf, "."); strcat(buf, exts[o.nFilterIndex - 1]);
    }
    return DoSave(buf);
}

static bool CmdSave() { return curFile[0] ? DoSave(curFile) : CmdSaveAs(); }

static bool Confirm() {
    if (!dirty) return true;
    int r = MessageBox(hMain, "The current image has unsaved changes. Save them now?", APP_NAME,
                       MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return CmdSave();
    return true;
}

static Node* NodeAt(int i) {
    LVITEM it; memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM; it.iItem = i;
    if (!ListView_GetItem(hList, &it)) return 0;
    return (Node*)it.lParam;
}
static int ItemOf(Node* n) {
    int c = ListView_GetItemCount(hList);
    for (int i = 0; i < c; i++) if (NodeAt(i) == n) return i;
    return -1;
}

static void CmdOpen() {
    if (!Confirm()) return;
    char buf[MAX_PATH] = "";
    OPENFILENAME o; memset(&o, 0, sizeof(o));
#ifdef OPENFILENAME_SIZE_VERSION_400A
    o.lStructSize = OPENFILENAME_SIZE_VERSION_400A;
#else
    o.lStructSize = sizeof(o);
#endif
    o.hwndOwner = hMain;
    o.lpstrFilter = "Zip disk images (*.img;*.ima;*.dsk;*.bin)\0*.img;*.ima;*.dsk;*.bin\0All files (*.*)\0*.*\0";
    o.lpstrFile = buf; o.nMaxFile = MAX_PATH; o.lpstrTitle = "Open Image";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileName(&o)) return;
    std::string err;
    HCURSOR old = SetCursor(LoadCursor(0, IDC_WAIT));
    bool ok = LoadImage(buf, err);
    SetCursor(old);
    if (!ok) MessageBox(hMain, err.c_str(), APP_NAME, MB_OK | MB_ICONERROR);
    Refresh();
}

static void CmdNewFolder() {
    std::string nm = "New Folder";
    for (int i = 2;; i++) {
        bool dup = false;
        for (size_t j = 0; j < cur->kids.size(); j++)
            if (_stricmp(cur->kids[j]->name.c_str(), nm.c_str()) == 0) dup = true;
        if (!dup) break;
        char t[48]; sprintf(t, "New Folder (%d)", i); nm = t;
    }
    Node* n = new Node();
    n->name = nm; n->dir = true; n->parent = cur;
    NowDos(); n->dDate = g_nowDate; n->dTime = g_nowTime;
    cur->kids.push_back(n);
    dirty = true; Refresh();
    int i = ItemOf(n);
    if (i >= 0) { SetFocus(hList); ListView_EnsureVisible(hList, i, FALSE); ListView_EditLabel(hList, i); }
}

static void CmdRename() {
    int i = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (i >= 0 && NodeAt(i)) { SetFocus(hList); ListView_EditLabel(hList, i); }
}

/* returns TRUE if the new name is accepted */
static BOOL EndRename(int item, const char* text) {
    Node* n = NodeAt(item);
    if (!n || !text) return FALSE;
    std::string s = text;
    while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '.')) s.erase(s.size() - 1);
    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
    const char* bad = "\\/:*?\"<>|";
    bool ok = !s.empty() && s.size() <= 255 && s.find_first_of(bad) == std::string::npos;
    if (!ok) { MessageBox(hMain, "Invalid name. It cannot be empty or contain  \\ / : * ? \" < > |", APP_NAME, MB_OK | MB_ICONWARNING); return FALSE; }
    for (size_t j = 0; j < cur->kids.size(); j++)
        if (cur->kids[j] != n && _stricmp(cur->kids[j]->name.c_str(), s.c_str()) == 0) {
            MessageBox(hMain, "An item with that name already exists.", APP_NAME, MB_OK | MB_ICONWARNING);
            return FALSE;
        }
    n->name = s; dirty = true;
    return TRUE;
}

static BOOL CALLBACK FontProc(HWND h, LPARAM f) { SendMessage(h, WM_SETFONT, (WPARAM)f, TRUE); return TRUE; }

static void AddCol(int i, const char* t, int w) {
    LVCOLUMN c; memset(&c, 0, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; c.pszText = (char*)t; c.cx = w; c.iSubItem = i;
    ListView_InsertColumn(hList, i, &c);
}

static void BuildUI(HWND h) {
    hMain = h;
    HMENU mb = CreateMenu(), f = CreatePopupMenu(), a = CreatePopupMenu(), o = CreatePopupMenu(), hp = CreatePopupMenu();
    AppendMenu(f, MF_STRING, ID_NEW, "&New\tCtrl+N");
    AppendMenu(f, MF_STRING, ID_OPEN, "&Open...\tCtrl+O");
    AppendMenu(f, MF_SEPARATOR, 0, 0);
    AppendMenu(f, MF_STRING, ID_SAVE, "&Save\tCtrl+S");
    AppendMenu(f, MF_STRING, ID_SAVEAS, "Save &As...\tCtrl+Shift+S");
    AppendMenu(f, MF_SEPARATOR, 0, 0);
    AppendMenu(f, MF_STRING, ID_EXIT, "E&xit");
    AppendMenu(a, MF_STRING, ID_ADDF, "Add &Files...");
    AppendMenu(a, MF_STRING, ID_ADDD, "Add F&older...");
    AppendMenu(a, MF_STRING, ID_NEWF, "&New Folder");
    AppendMenu(a, MF_STRING, ID_RENAME, "&Rename\tF2");
    AppendMenu(a, MF_STRING, ID_DEL, "&Delete\tDel");
    AppendMenu(a, MF_STRING, ID_UP, "&Up one level\tBackspace");
    AppendMenu(a, MF_STRING, ID_SELALL, "Select &All\tCtrl+A");
    AppendMenu(o, MF_STRING | MF_CHECKED, ID_MBR, "Include &partition table (MBR, like real Zip disks)");
    AppendMenu(hp, MF_STRING, ID_ABOUT, "&About...");
    AppendMenu(mb, MF_POPUP, (UINT_PTR)f, "&File");
    AppendMenu(mb, MF_POPUP, (UINT_PTR)a, "&Action");
    AppendMenu(mb, MF_POPUP, (UINT_PTR)o, "&Options");
    AppendMenu(mb, MF_POPUP, (UINT_PTR)hp, "&Help");
    SetMenu(h, mb);

    HINSTANCE hi = GetModuleHandle(0);
    CreateWindow("BUTTON", "Add Files", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)ID_ADDF, hi, 0);
    CreateWindow("BUTTON", "Add Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)ID_ADDD, hi, 0);
    CreateWindow("BUTTON", "New Folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)ID_NEWF, hi, 0);
    CreateWindow("BUTTON", "Delete", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)ID_DEL, hi, 0);
    CreateWindow("BUTTON", "Up", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, h, (HMENU)ID_UP, hi, 0);
    CreateWindow("STATIC", "Media:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)300, hi, 0);
    hCombo = CreateWindowEx(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, h, (HMENU)ID_COMBO, hi, 0);
    for (int i = 0; i < ZT_COUNT; i++) SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)ZT[i].name);
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
    CreateWindow("STATIC", "Label:", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)301, hi, 0);
    hLabel = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "ZIPDISK", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, h, (HMENU)ID_LABEL, hi, 0);
    SendMessage(hLabel, EM_LIMITTEXT, 11, 0);
    hPathLbl = CreateWindowEx(WS_EX_STATICEDGE, "STATIC", "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)302, hi, 0);
    hList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, "", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
                           0, 0, 0, 0, h, (HMENU)ID_LIST, hi, 0);
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT);
    AddCol(0, "Name", 280); AddCol(1, "Size", 100); AddCol(2, "Type", 110);
    hProg = CreateWindowEx(0, PROGRESS_CLASS, "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)303, hi, 0);
    SendMessage(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    hStatus = CreateWindowEx(0, STATUSCLASSNAME, "", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)304, hi, 0);
    EnumChildWindows(h, FontProc, (LPARAM)GetStockObject(DEFAULT_GUI_FONT));
    NewImage();
    Refresh();
}

static void Layout() {
    if (!hStatus) return;
    RECT rc; GetClientRect(hMain, &rc);
    int W = rc.right, H = rc.bottom;
    SendMessage(hStatus, WM_SIZE, 0, 0);
    RECT sr; GetWindowRect(hStatus, &sr);
    int sh = sr.bottom - sr.top;
    int x = 6;
    MoveWindow(GetDlgItem(hMain, ID_ADDF), x, 4, 76, 24, TRUE); x += 80;
    MoveWindow(GetDlgItem(hMain, ID_ADDD), x, 4, 82, 24, TRUE); x += 86;
    MoveWindow(GetDlgItem(hMain, ID_NEWF), x, 4, 82, 24, TRUE); x += 86;
    MoveWindow(GetDlgItem(hMain, ID_DEL), x, 4, 60, 24, TRUE); x += 64;
    MoveWindow(GetDlgItem(hMain, ID_UP), x, 4, 40, 24, TRUE); x += 54;
    MoveWindow(GetDlgItem(hMain, 300), x, 9, 44, 16, TRUE); x += 46;
    MoveWindow(hCombo, x, 4, 150, 200, TRUE); x += 158;
    MoveWindow(GetDlgItem(hMain, 301), x, 9, 40, 16, TRUE); x += 42;
    MoveWindow(hLabel, x, 5, 100, 22, TRUE);
    MoveWindow(hPathLbl, 0, 32, W, 18, TRUE);
    int ly = 52, lh = H - ly - sh - 16; if (lh < 10) lh = 10;
    MoveWindow(hList, 0, ly, W, lh, TRUE);
    MoveWindow(hProg, 0, ly + lh, W, 16, TRUE);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: BuildUI(h); return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mi = (MINMAXINFO*)l; mi->ptMinTrackSize.x = 740; mi->ptMinTrackSize.y = 320; return 0; }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_NEW: if (Confirm()) { NewImage(); Refresh(); } break;
        case ID_SAVE: CmdSave(); break;
        case ID_SAVEAS: CmdSaveAs(); break;
        case ID_EXIT: SendMessage(h, WM_CLOSE, 0, 0); break;
        case ID_OPEN: CmdOpen(); break;
        case ID_NEWF: CmdNewFolder(); break;
        case ID_RENAME: CmdRename(); break;
        case ID_ADDF: CmdAddFiles(); break;
        case ID_ADDD: CmdAddFolder(); break;
        case ID_DEL: CmdDelete(); break;
        case ID_UP: CmdUp(); break;
        case ID_SELALL: { int n = ListView_GetItemCount(hList);
            for (int i = 0; i < n; i++) ListView_SetItemState(hList, i, LVIS_SELECTED, LVIS_SELECTED); break; }
        case ID_MBR: {
            useMbr = !useMbr;
            CheckMenuItem(GetMenu(h), ID_MBR, MF_BYCOMMAND | (useMbr ? MF_CHECKED : MF_UNCHECKED));
            dirty = true; Refresh(); break; }
        case ID_COMBO: if (HIWORD(w) == CBN_SELCHANGE) { dirty = true; Refresh(); } break;
        case ID_ABOUT:
            MessageBox(h, APP_NAME "\nVersion " APP_VERSION "\n\nCreates raw FAT16 disk images of Iomega Zip 100/250/750 media.\n"
                          "Drag & drop files or folders into the window to add them.",
                       "About", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)l;
        if (nh->idFrom == ID_LIST) {
            if (nh->code == NM_DBLCLK) {
                int i = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
                if (i >= 0) {
                    LVITEM it; memset(&it, 0, sizeof(it)); it.mask = LVIF_PARAM; it.iItem = i;
                    ListView_GetItem(hList, &it);
                    if (!it.lParam) CmdUp();
                    else if (((Node*)it.lParam)->dir) { cur = (Node*)it.lParam; Refresh(); }
                }
            } else if (nh->code == LVN_BEGINLABELEDIT) {
                return NodeAt(((NMLVDISPINFO*)l)->item.iItem) ? FALSE : TRUE;  /* ".." cannot be renamed */
            } else if (nh->code == LVN_ENDLABELEDIT) {
                NMLVDISPINFO* di = (NMLVDISPINFO*)l;
                BOOL r = di->item.pszText ? EndRename(di->item.iItem, di->item.pszText) : FALSE;
                PostMessage(h, WM_APP + 1, 0, 0);
                return r;
            } else if (nh->code == LVN_KEYDOWN) {
                WORD k = ((NMLVKEYDOWN*)l)->wVKey;
                if (k == VK_DELETE) CmdDelete();
                else if (k == VK_BACK) CmdUp();
            }
        }
        return 0; }
    case WM_DROPFILES: {
        HDROP hd = (HDROP)w;
        UINT n = DragQueryFile(hd, 0xFFFFFFFF, 0, 0);
        for (UINT i = 0; i < n; i++) {
            char p[MAX_PATH];
            if (DragQueryFile(hd, i, p, MAX_PATH)) {
                std::string s = p;
                if (s.size() > 3 && s[s.size() - 1] == '\\') s.erase(s.size() - 1);
                AddItem(cur, s);
            }
        }
        DragFinish(hd);
        AfterAdd();
        return 0; }
    case WM_APP + 1: Refresh(); return 0;
    case WM_CLOSE: if (Confirm()) DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int show) {
    InitCommonControls();
    CoInitialize(0);
    WNDCLASS wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc; wc.hInstance = hi;
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.hIcon = LoadIcon(0, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = "ZipImagerWnd";
    RegisterClass(&wc);
    HWND h = CreateWindowEx(WS_EX_ACCEPTFILES, "ZipImagerWnd", APP_NAME, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 800, 480, 0, 0, hi, 0);
    ShowWindow(h, show); UpdateWindow(h);

    ACCEL ac[6];
    ac[0].fVirt = FVIRTKEY | FCONTROL; ac[0].key = 'N'; ac[0].cmd = ID_NEW;
    ac[1].fVirt = FVIRTKEY | FCONTROL; ac[1].key = 'S'; ac[1].cmd = ID_SAVE;
    ac[2].fVirt = FVIRTKEY | FCONTROL | FSHIFT; ac[2].key = 'S'; ac[2].cmd = ID_SAVEAS;
    ac[3].fVirt = FVIRTKEY | FCONTROL; ac[3].key = 'A'; ac[3].cmd = ID_SELALL;
    ac[4].fVirt = FVIRTKEY | FCONTROL; ac[4].key = 'O'; ac[4].cmd = ID_OPEN;
    ac[5].fVirt = FVIRTKEY; ac[5].key = VK_F2; ac[5].cmd = ID_RENAME;
    HACCEL hAcc = CreateAcceleratorTable(ac, 6);

    MSG msg;
    while (GetMessage(&msg, 0, 0, 0)) {
        if (!TranslateAccelerator(h, hAcc, &msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    }
    DestroyAcceleratorTable(hAcc);
    CoUninitialize();
    return (int)msg.wParam;
}
