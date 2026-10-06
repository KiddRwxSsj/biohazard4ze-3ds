// english over the pt text tables. runs on common/3d_base right after file.c caches them.
// same rebuilders as re/lang.py, which also spits out lang_en.h from re/lang/en.txt.
// patch only lands if the original string still hashes the same -> other dumps / second pass
// are no-ops. table that doesn't parse or won't fit its entry stays untouched.
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "aee.h"

typedef struct {
    const char *arc, *entry;
    u16 off, len;   // original string in the entry, length without the \0
    u32 fnv;        // of the original bytes
    u16 enlen;      // glyph strings hold zero bytes, so no strlen
    const char *en;
} lang_patch;

static const lang_patch patches[] = {
#include "lang_en.h"
};
#define NPATCH (sizeof patches / sizeof *patches)

static u32 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static u32 fnv(const u8 *p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

typedef struct {
    const u8 *src;
    u32 size;
    const lang_patch *p;
    int n, used, stale;
    int wide;       // 2 for the u16 glyph strings of the layout file
} tab_t;

// english for the string at off, NULL = keep original
static const char *repl(tab_t *t, u32 off, u32 *len)
{
    for (int i = 0; i < t->n; i++) {
        const lang_patch *p = &t->p[i];
        if (p->off != off) continue;
        u32 end = off + p->len;
        if (end + t->wide > t->size || t->src[end] || (t->wide == 2 && t->src[end + 1]) ||
            fnv(t->src + off, p->len) != p->fnv) {
            t->stale++;
            return NULL;
        }
        t->used++;
        *len = p->enlen;
        return p->en;
    }
    return NULL;
}

// a \0 terminated string at src[p], inside [p, end). length or -1
static int cstr(const u8 *src, u32 p, u32 end)
{
    for (u32 i = p; i < end; i++) if (!src[i]) return i - p;
    return -1;
}

// output cursor with a hard cap
typedef struct { u8 *d; u32 o, cap; bool full; } out_t;

static u8 *emit(out_t *w, const void *s, u32 n)
{
    if (w->o + n > w->cap) { w->full = true; return NULL; }
    u8 *at = w->d + w->o;
    memcpy(at, s, n);
    w->o += n;
    return at;
}

// string at off (replaced or not) + \0. returns len incl \0, 0 on error
static u32 emit_str(out_t *w, tab_t *t, u32 off, u32 end)
{
    int n = cstr(t->src, off, end);
    if (n < 0) return 0;
    u32 len = n;
    const char *s = repl(t, off, &len);
    if (!emit(w, s ? (const u8 *)s : t->src + off, len) || !emit(w, "", 1)) return 0;
    return len + 1;
}

// "SCB" u8 n u8; n x { u16 strbytes, 24 bytes (u16 namelen at +13), name\0, desc\0 }
static bool items(tab_t *t, out_t *w)
{
    const u8 *s = t->src;
    if (t->size < 5 || !emit(w, s, 5)) return false;
    u32 p = 5;
    for (int i = 0; i < s[3]; i++) {
        if (p + 26 > t->size) return false;
        u32 end = p + 26 + rd16(s + p);
        if (end > t->size) return false;
        u8 *rec = emit(w, s + p, 26);
        if (!rec) return false;
        u32 q = p + 26;
        int nl = cstr(s, q, end);
        if (nl < 0) return false;
        u32 a = emit_str(w, t, q, end);
        u32 b = a ? emit_str(w, t, q + nl + 1, end) : 0;
        if (!b) return false;
        wr16(rec, a + b);
        wr16(rec + 2 + 13, a);
        p = end;
    }
    return true;
}

// "SCB" u8 n u8; n x { u16 strbytes, 5 bytes, u16 end of title, u16 end of place,
// title\0 place\0 text\0 }
static bool missions(tab_t *t, out_t *w)
{
    const u8 *s = t->src;
    if (t->size < 5 || !emit(w, s, 5)) return false;
    u32 p = 5;
    for (int i = 0; i < s[3]; i++) {
        if (p + 11 > t->size) return false;
        u32 end = p + 11 + rd16(s + p);
        if (end > t->size) return false;
        u8 *rec = emit(w, s + p, 11);
        if (!rec) return false;
        u32 q = p + 11, len[3];
        for (int k = 0; k < 3; k++) {
            int n = cstr(s, q, end);
            if (n < 0 || !(len[k] = emit_str(w, t, q, end))) return false;
            q += n + 1;
        }
        wr16(rec, len[0] + len[1] + len[2]);
        wr16(rec + 7, len[0]);
        wr16(rec + 9, len[0] + len[1]);
        p = end;
    }
    return true;
}

// a text section in [sec, end): u32 count, u32 strbytes, count x { u16 off, u16 len }, strings.
// written to w->d at the same place, zero filled to end
static bool section(tab_t *t, out_t *w, u32 sec, u32 end)
{
    const u8 *s = t->src;
    if (sec + 8 > end || end > t->size) return false;
    u32 cnt = rd32(s + sec), st = sec + 8 + 4 * cnt;
    if (st > end) return false;
    out_t sw = { w->d + sec, 0, end - sec, false };
    u8 *hdr = emit(&sw, s + sec, 8 + 4 * cnt);
    if (!hdr) return false;
    u32 body = 0;
    for (u32 k = 0; k < cnt; k++) {
        u32 o = rd16(s + sec + 8 + 4 * k);
        u32 n = emit_str(&sw, t, st + o, end);
        if (!n) return false;
        wr16(hdr + 8 + 4 * k, body);
        wr16(hdr + 10 + 4 * k, n);
        body += n;
    }
    wr32(hdr + 4, body);
    memset(w->d + sec + sw.o, 0, sw.cap - sw.o);
    return true;
}

// u32 1, u32 base, u32 end; at base u32 n, u32 off[n + 1] from base, n text sections
static bool sysparam(tab_t *t, out_t *w)
{
    const u8 *s = t->src;
    memcpy(w->d, s, t->size);
    w->o = t->size;
    if (t->size < 12) return false;
    u32 base = rd32(s + 4);
    if (base + 4 > t->size) return false;
    u32 n = rd32(s + base);
    if (base + 8 + 4 * n > t->size) return false;
    for (u32 i = 0; i < n; i++)
        if (!section(t, w, base + rd32(s + base + 4 + 4 * i), base + rd32(s + base + 8 + 4 * i))) return false;
    return true;
}

// u32 6, u32 off[7]; subtable 5 is the text section
static bool misdata(tab_t *t, out_t *w)
{
    const u8 *s = t->src;
    memcpy(w->d, s, t->size);
    w->o = t->size;
    if (t->size < 32 || rd32(s) != 6) return false;
    return section(t, w, rd32(s + 24), rd32(s + 28));
}

// "HFS" u16 n, u16 pool units; n x 20 byte records { u16 string (units into the pool), ... };
// pool of u16 glyph strings, 0 terminated. relay strings in order, repoint records,
// keep unit count, zero the tail.
static bool hfs(tab_t *t, out_t *w)
{
    const u8 *s = t->src;
    memcpy(w->d, s, t->size);
    w->o = t->size;
    if (t->size < 7) return false;
    u32 n = rd16(s + 3), units = rd16(s + 5), pool = 7 + 20 * n;
    if (pool + 2 * units > t->size) return false;
    u16 *moved = malloc(2 * units);
    if (!moved) return false;
    memset(moved, 0xff, 2 * units);
    out_t pw = { w->d + pool, 0, 2 * units, false };
    bool ok = true;
    for (u32 o = 0; o < units && ok; ) {
        u32 e = o;
        while (e < units && rd16(s + pool + 2 * e)) e++;
        if (e == units) { ok = false; break; }
        moved[o] = pw.o / 2;
        u32 len = 2 * (e - o);
        const char *r = repl(t, pool + 2 * o, &len);
        ok = emit(&pw, r ? (const u8 *)r : s + pool + 2 * o, len) && emit(&pw, "\0", 2);
        o = e + 1;
    }
    for (u32 i = 0; i < n && ok; i++) {
        u32 o = rd16(s + 7 + 20 * i);
        if (o >= units || moved[o] == 0xffff) ok = false;
        else wr16(w->d + 7 + 20 * i, moved[o]);
    }
    if (ok) memset(w->d + pool + pw.o, 0, 2 * units - pw.o);
    w->full = pw.full;
    free(moved);
    return ok;
}

// same length or shorter, padded with spaces
static bool inplace(tab_t *t, out_t *w)
{
    memcpy(w->d, t->src, t->size);
    w->o = t->size;
    for (int i = 0; i < t->n; i++) {
        u32 len, off = t->p[i].off;
        const char *e = repl(t, off, &len);
        if (!e || len > t->p[i].len) continue;
        memcpy(w->d + off, e, len);
        memset(w->d + off + len, ' ', t->p[i].len - len);
    }
    return true;
}

static bool (*kind(const char *name))(tab_t *, out_t *)
{
    if (!strcmp(name, "item_base_info.bin")) return items;
    if (!strncmp(name, "stgnfo_mission", 14)) return missions;
    if (!strcmp(name, "sys_param.bin")) return sysparam;
    if (!strncmp(name, "mis_data", 8)) return misdata;
    if (!strcmp(name, "Bio4_StringLayout.bin")) return hfs;
    return inplace;
}

static void entry(const char *arc, const char *name, u8 *d, u32 size)
{
    int i = 0;
    while (i < (int)NPATCH && (strcasecmp(patches[i].arc, arc) || strcmp(patches[i].entry, name))) i++;
    if (i == (int)NPATCH) return;
    int j = i;
    while (j < (int)NPATCH && !strcmp(patches[j].entry, name) && !strcasecmp(patches[j].arc, arc)) j++;

    u8 *buf = malloc(size);
    if (!buf) return;
    tab_t t = { d, size, &patches[i], j - i, 0, 0, strcmp(name, "Bio4_StringLayout.bin") ? 1 : 2 };
    out_t w = { buf, 0, size, false };
    bool ok = kind(name)(&t, &w);
    if (ok && t.used) {
        memcpy(d, buf, w.o);
        memset(d + w.o, 0, size - w.o);
    }
    if (!ok || t.stale) dbg("lang: %s %s, %d of %d strings%s", arc, name, ok ? t.used : 0, t.n,
                            ok ? "" : w.full ? ", doesn't fit, left as is" : ", unknown layout, left as is");
    free(buf);
}

void lang_apply(const char *arc, u8 *d, u32 size)
{
    static const u8 key[4] = { 'b', 'i', 'o', '4' };
    if (size < 0x18 || memcmp(d, "H2Z", 4) || memcmp(d + 12, "HIZ", 4)) return;
    u32 n = rd32(d + 16), p = 0x18, k = 0;
    for (u32 e = 0; e < n; e++) {
        u8 h[12];
        char name[64];
        if (p + 12 > size) return;
        for (int i = 0; i < 12; i++) h[i] = d[p + i] ^ key[(k + i) & 3];
        p += 12; k += 12;
        u32 esize = rd32(h + 2), nl = rd16(h + 10);
        if (nl >= sizeof name || p + nl + esize > size) return;
        for (u32 i = 0; i < nl; i++) name[i] = d[p + i] ^ key[(k + i) & 3];
        name[nl] = 0;
        p += nl; k += nl;
        if (!(rd16(h) & 8)) entry(arc, name, d + p, esize);   // flag 8 is deflate, mobile only
        p += esize;
    }
}
