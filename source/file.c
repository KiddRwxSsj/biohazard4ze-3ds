// IFileMgr / IFile. game archives get pulled into ram once, the game reopens
// common.h2z a couple hundred times and sd opens are not cheap
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aee.h"

#define OFM_READ      1
#define OFM_READWRITE 2
#define OFM_CREATE    4
#define OFM_APPEND    8
#define SEEK_START_   0
#define SEEK_END_     1
#define SEEK_CUR_     2

typedef struct {
    char name[64];
    u8 *data;
    u32 size;
} cached_t;

static cached_t cache[24];
static int ncache;

typedef struct {
    void *const *vt;
    const u8 *mem;
    u32 size, pos;
    FILE *fp;
} file_t;

static void *vt_file[12];
static void *vt_fmgr[21];
static aee_obj fmgr = { vt_fmgr };

static void host_path(const char *g, char *out, size_t n)
{
    if (!strncmp(g, "fs:/~/", 6)) g += 6;
    while (*g == '/') g++;
    snprintf(out, n, GAME_DIR "%s", g);
    for (char *p = out + sizeof(GAME_DIR) - 1; *p; p++) if (*p == '\\') *p = '/';
}

// read-only stuff that ships with the game, everything else is save data
static bool is_game_file(const char *g)
{
    const char *e = strrchr(g, '.');
    return e && (!strcasecmp(e, ".h2z") || !strcasecmp(e, ".bin") || !strcasecmp(e, ".mod"));
}

// the game reopens these constantly, the stage archives only on stage loads. keeping all
// of them in ram cost 8MB of heap the soundfont needs
static bool is_hot(const char *g)
{
    return !strncasecmp(g, "common", 6) || !strncasecmp(g, "3d_base", 7);
}

static cached_t *cache_get(const char *g, const char *hp)
{
    for (int i = 0; i < ncache; i++) if (!strcasecmp(cache[i].name, g)) return &cache[i];
    if (ncache == 24) return NULL;
    FILE *f = fopen(hp, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    u32 sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *d = malloc(sz ? sz : 1);
    if (!d || fread(d, 1, sz, f) != sz) { free(d); fclose(f); return NULL; }
    fclose(f);
    lang_apply(g, d, sz);
    cached_t *c = &cache[ncache++];
    strlcpy(c->name, g, sizeof c->name);
    c->data = d;
    c->size = sz;
    dbg("file: cached %s (%lu)", g, sz);
    return c;
}

static void mkparents(char *hp)
{
    for (char *p = hp + sizeof(GAME_DIR); *p; p++)
        if (*p == '/') { *p = 0; mkdir(hp, 0777); *p = '/'; }
}

static file_t *fm_open_(void *m, const char *g, int mode, char *hp);
static char where_buf[80];

static file_t *fm_open(void *m, const char *g, int mode)
{
    char hp[256];
    snprintf(where_buf, sizeof where_buf, "file %s", g);
    const char *was = (const char *)g_where;
    WHERE(where_buf);
    g_beat++;
    file_t *r = fm_open_(m, g, mode, hp);
    g_beat++;
    WHERE(was);
    return r;
}

static file_t *fm_open_(void *m, const char *g, int mode, char *hp)
{
    host_path(g, hp, 256);
    file_t *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->vt = vt_file;

    if (mode == OFM_READ && is_game_file(g) && is_hot(g)) {
        cached_t *c = cache_get(g, hp);
        if (!c) { free(f); return NULL; }
        f->mem = c->data;
        f->size = c->size;
        return f;
    }
    const char *how = "rb";
    if (mode & OFM_CREATE) { mkparents(hp); how = "w+b"; }
    else if (mode & OFM_APPEND) how = "a+b";
    else if (mode & OFM_READWRITE) how = "r+b";
    f->fp = fopen(hp, how);
    if (!f->fp) { free(f); return NULL; }
    if (mode == OFM_READ) setvbuf(f->fp, NULL, _IOFBF, 32 * 1024);
    return f;
}

static int fm_test(void *m, const char *g)
{
    char hp[256];
    struct stat st;
    host_path(g, hp, sizeof hp);
    return stat(hp, &st) ? EFAILED : SUCCESS;
}

static void fill_info(u8 *info, const char *g, u32 size, bool dir)
{
    info[0] = dir ? 0x10 : 0;
    *(u32 *)(info + 4) = 0;
    *(u32 *)(info + 8) = size;
    strlcpy((char *)info + 12, g, 64);
}

static int fm_info(void *m, const char *g, u8 *info)
{
    char hp[256];
    struct stat st;
    host_path(g, hp, sizeof hp);
    if (stat(hp, &st)) return EFAILED;
    fill_info(info, g, st.st_size, S_ISDIR(st.st_mode));
    return SUCCESS;
}

static int fm_remove(void *m, const char *g)
{
    char hp[256];
    host_path(g, hp, sizeof hp);
    return remove(hp) ? EFAILED : SUCCESS;
}

static int fm_mkdir(void *m, const char *g)
{
    char hp[256];
    host_path(g, hp, sizeof hp);
    return mkdir(hp, 0777) ? EFAILED : SUCCESS;
}

static u32 fm_free(void *m, u32 *total) { if (total) *total = 64 << 20; return 32 << 20; }
static int fm_err(void *m) { return EFAILED; }
static int fm_enum(void) { return EFAILED; }

static int f_release(file_t *f)
{
    if (f->fp) fclose(f->fp);
    free(f);
    return 0;
}

static int32_t f_read(file_t *f, void *buf, u32 n)
{
    if (f->fp) return fread(buf, 1, n, f->fp);
    if (f->pos >= f->size) return 0;
    if (n > f->size - f->pos) n = f->size - f->pos;
    memcpy(buf, f->mem + f->pos, n);
    f->pos += n;
    return n;
}

static u32 f_write(file_t *f, const void *buf, u32 n)
{
    return f->fp ? fwrite(buf, 1, n, f->fp) : 0;
}

static int f_seek(file_t *f, int how, int32_t off)
{
    if (f->fp) {
        if (how == SEEK_CUR_ && off == 0) return ftell(f->fp);
        return fseek(f->fp, off, how == SEEK_END_ ? SEEK_END : how == SEEK_CUR_ ? SEEK_CUR : SEEK_SET)
               ? EFAILED : SUCCESS;
    }
    // brew quirk: SEEK_CURRENT,0 is a tell
    if (how == SEEK_CUR_ && off == 0) return f->pos;
    int32_t p = how == SEEK_END_ ? (int32_t)f->size + off : how == SEEK_CUR_ ? (int32_t)f->pos + off : off;
    if (p < 0 || p > (int32_t)f->size) return EFAILED;
    f->pos = p;
    return SUCCESS;
}

static int f_info(file_t *f, u8 *info)
{
    u32 sz = f->size;
    if (f->fp) {
        long cur = ftell(f->fp);
        fseek(f->fp, 0, SEEK_END);
        sz = ftell(f->fp);
        fseek(f->fp, cur, SEEK_SET);
    }
    fill_info(info, "", sz, false);
    return SUCCESS;
}

static int f_truncate(file_t *f, u32 len)
{
    if (!f->fp) return EFAILED;
    fflush(f->fp);
    return ftruncate(fileno(f->fp), len) ? EFAILED : SUCCESS;
}

static int one(void) { return 1; }
static int zero(void) { return 0; }

void files_init(void)
{
    void **v = vt_fmgr;
    vt_fill(v, 21);
    v[0] = one; v[1] = zero; v[2] = fm_open; v[3] = fm_info; v[4] = fm_remove; v[5] = fm_mkdir;
    v[6] = fm_remove; v[7] = fm_test; v[8] = fm_free; v[9] = fm_err; v[10] = fm_enum; v[11] = fm_enum;
    v[13] = fm_enum; v[15] = fm_info;

    v = vt_file;
    vt_fill(v, 12);
    v[0] = one; v[1] = f_release; v[2] = zero; v[3] = f_read; v[4] = zero; v[5] = f_write;
    v[6] = f_info; v[7] = f_seek; v[8] = f_truncate; v[9] = f_info; v[10] = zero; v[11] = zero;
    mkdir(GAME_DIR "udata", 0777);
}

void *filemgr_new(void) { return &fmgr; }
