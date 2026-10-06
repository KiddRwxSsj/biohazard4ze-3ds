// AEEHelperFuncs: the libc-ish table the mod finds at base-4
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>
#include "aee.h"

#define NHELP 117
#define ALLOC_NO_ZMEM 0x80000000u
// brew clock starts 1980-01-06 like gps
#define BREW_EPOCH 315964800

static u64 t0;

uint32_t aee_uptime(void) { return (uint32_t)(osGetTime() - t0); }

void *aee_malloc(uint32_t n)
{
    void *p = malloc((n & ~ALLOC_NO_ZMEM) ? (n & ~ALLOC_NO_ZMEM) : 1);
    if (p && !(n & ALLOC_NO_ZMEM)) memset(p, 0, n & ~ALLOC_NO_ZMEM);
    return p;
}

void aee_free(void *p) { free(p); }

static void *h_realloc(void *p, uint32_t n) { return realloc(p, n & ~ALLOC_NO_ZMEM); }

// ads compiled code hands va_list over as int *[1], ie a pointer to the arg pointer
static int h_vsprintf(char *b, const char *f, void **ap)
{
    va_list v;
    memcpy(&v, ap, sizeof v);
    return vsprintf(b, f, v);
}

static int h_vsnprintf(char *b, size_t n, const char *f, void **ap)
{
    va_list v;
    memcpy(&v, ap, sizeof v);
    return vsnprintf(b, n, f, v);
}

static int h_sprintf(char *b, const char *f, ...)
{
    va_list v;
    va_start(v, f);
    int r = vsprintf(b, f, v);
    va_end(v);
    return r;
}

static int h_snprintf(char *b, size_t n, const char *f, ...)
{
    va_list v;
    va_start(v, f);
    int r = vsnprintf(b, n, f, v);
    va_end(v);
    return r;
}

static void h_dbgprintf(const char *f, ...)
{
    char tmp[256];
    va_list v;
    va_start(v, f);
    vsnprintf(tmp, sizeof tmp, f, v);
    va_end(v);
    dbg("game: %s", tmp);
}

static int wlen(const AECHAR *s) { int n = 0; while (s[n]) n++; return n; }

static AECHAR *h_wstrcpy(AECHAR *d, const AECHAR *s) { memmove(d, s, (wlen(s) + 1) * 2); return d; }
static AECHAR *h_wstrcat(AECHAR *d, const AECHAR *s) { h_wstrcpy(d + wlen(d), s); return d; }
static int h_wstrlen(const AECHAR *s) { return wlen(s); }

static int h_wstrcmp(const AECHAR *a, const AECHAR *b)
{
    while (*a && *a == *b) a++, b++;
    return *a - *b;
}

static AECHAR *h_wstrchr(const AECHAR *s, AECHAR c)
{
    for (;; s++) {
        if (*s == c) return (AECHAR *)s;
        if (!*s) return NULL;
    }
}

// strtowstr(const char *, AECHAR *, int nbytes)
static AECHAR *h_strtowstr(const char *s, AECHAR *d, int nb)
{
    int n = nb / 2 - 1, i;
    if (n < 0) return d;
    for (i = 0; i < n && s[i]; i++) d[i] = (u8)s[i];
    d[i] = 0;
    return d;
}

static char *h_wstrtostr(const AECHAR *s, char *d, int nb)
{
    int i;
    if (nb <= 0) return d;
    for (i = 0; i < nb - 1 && s[i]; i++) d[i] = s[i] < 0x100 ? s[i] : '?';
    d[i] = 0;
    return d;
}

// strexpand(const byte *src, int n, AECHAR *dst, int dstbytes)
static AECHAR *h_strexpand(const u8 *s, int n, AECHAR *d, int nb)
{
    int m = nb / 2 - 1, i;
    if (m < 0) return d;
    for (i = 0; i < n && i < m; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}

static bool h_utf8towstr(const u8 *s, int n, AECHAR *d, int nb)
{
    int m = nb / 2 - 1, o = 0, i = 0;
    while (i < n && o < m && s[i]) {
        u32 c = s[i++];
        if (c >= 0xe0 && i + 1 < n) { c = (c & 15) << 12 | (s[i] & 63) << 6 | (s[i+1] & 63); i += 2; }
        else if (c >= 0xc0 && i < n) { c = (c & 31) << 6 | (s[i] & 63); i++; }
        d[o++] = c;
    }
    if (m >= 0) d[o] = 0;
    return true;
}

static int h_wstrtoutf8(const AECHAR *s, int n, u8 *d, int nb)
{
    int o = 0;
    for (int i = 0; i < n && s[i] && o < nb - 3; i++) {
        u32 c = s[i];
        if (c < 0x80) d[o++] = c;
        else if (c < 0x800) { d[o++] = 0xc0 | c >> 6; d[o++] = 0x80 | (c & 63); }
        else { d[o++] = 0xe0 | c >> 12; d[o++] = 0x80 | ((c >> 6) & 63); d[o++] = 0x80 | (c & 63); }
    }
    if (nb > 0) d[o] = 0;
    return o;
}

static void *h_app(void) { return shell_app(); }
static uint32_t h_uptime(void) { return aee_uptime(); }
static uint32_t h_seconds(void) { return (uint32_t)(time(NULL) - BREW_EPOCH); }

static uint32_t h_timems(void)
{
    time_t t = time(NULL);
    return (uint32_t)(t % 86400) * 1000 + (aee_uptime() % 1000);
}

typedef struct { u16 year, month, day, hour, minute, second, wday; } julian_t;

static void h_julian(uint32_t secs, julian_t *j)
{
    time_t t = secs ? (time_t)secs + BREW_EPOCH : time(NULL);
    struct tm *tm = gmtime(&t);
    j->year = tm->tm_year + 1900; j->month = tm->tm_mon + 1; j->day = tm->tm_mday;
    j->hour = tm->tm_hour; j->minute = tm->tm_min; j->second = tm->tm_sec;
    // brew weekday 0 = monday
    j->wday = (tm->tm_wday + 6) % 7;
}

static void h_rand(u8 *b, int n)
{
    while (n-- > 0) *b++ = rand() >> 7;
}

static char *h_strdup(const char *s) { return s ? strdup(s) : NULL; }
static char *h_strlower(char *s) { for (char *p = s; *p; p++) *p = tolower((u8)*p); return s; }
static char *h_strupper(char *s) { for (char *p = s; *p; p++) *p = toupper((u8)*p); return s; }
static uint32_t h_ramfree(void) { return 8 << 20; }
static uint32_t h_fsfree(uint32_t *total) { if (total) *total = 64 << 20; return 32 << 20; }
static uint32_t h_version(void) { return 0x04000200; }

static bool h_strbegins(const char *pre, const char *s) { return !strncmp(pre, s, strlen(pre)); }

static bool h_strends(const char *suf, const char *s)
{
    size_t a = strlen(suf), b = strlen(s);
    return a <= b && !strcmp(s + b - a, suf);
}

static char *h_stristr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return (char *)h;
    return NULL;
}

// game is soft-float, doubles come back in r0:r1
__attribute__((pcs("aapcs"))) static double h_strtod(const char *s, char **e) { return strtod(s, e); }

static void h_sleep(uint32_t ms) { svcSleepThread((s64)ms * 1000000); }

// anything we never saw the game call lands here and says which slot
#define STUB(n) static int stub_##n(void) { dbg("helper %d not done, lr %p", n, __builtin_return_address(0)); return 0; }
#define S10(a) STUB(a##0) STUB(a##1) STUB(a##2) STUB(a##3) STUB(a##4) STUB(a##5) STUB(a##6) STUB(a##7) STUB(a##8) STUB(a##9)
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9)
S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9) S10(10)
STUB(110) STUB(111) STUB(112) STUB(113) STUB(114) STUB(115) STUB(116)

#define R10(a) stub_##a##0, stub_##a##1, stub_##a##2, stub_##a##3, stub_##a##4, stub_##a##5, stub_##a##6, stub_##a##7, stub_##a##8, stub_##a##9
static void *tab[NHELP] = {
    stub_0, stub_1, stub_2, stub_3, stub_4, stub_5, stub_6, stub_7, stub_8, stub_9,
    R10(1), R10(2), R10(3), R10(4), R10(5), R10(6), R10(7), R10(8), R10(9), R10(10),
    stub_110, stub_111, stub_112, stub_113, stub_114, stub_115, stub_116,
};

void *helpers_table(void)
{
    t0 = osGetTime();
    srand(t0);
    tab[0] = memmove;      tab[1] = memset;       tab[2] = strcpy;      tab[3] = strcat;
    tab[4] = strcmp;       tab[5] = strlen;       tab[6] = strchr;      tab[7] = strrchr;
    tab[8] = h_sprintf;    tab[9] = h_wstrcpy;    tab[10] = h_wstrcat;  tab[11] = h_wstrcmp;
    tab[12] = h_wstrlen;   tab[13] = h_wstrchr;   tab[16] = h_strtowstr; tab[17] = h_wstrtostr;
    tab[20] = h_utf8towstr; tab[21] = h_wstrtoutf8;
    tab[26] = aee_malloc;  tab[27] = aee_free;    tab[29] = h_realloc;
    tab[35] = h_version;   tab[36] = atoi;        tab[39] = h_dbgprintf;
    tab[42] = h_rand;      tab[43] = h_timems;    tab[44] = h_uptime;   tab[45] = h_seconds;
    tab[46] = h_julian;    tab[47] = aee_free;    tab[48] = h_app;      tab[49] = strtoul;
    tab[50] = strncpy;     tab[51] = strncmp;     tab[52] = strcasecmp; tab[53] = strncasecmp;
    tab[54] = strstr;      tab[55] = memcmp;      tab[56] = memchr;     tab[57] = h_strexpand;
    tab[58] = h_stristr;   tab[61] = h_strdup;    tab[62] = h_strbegins; tab[63] = h_strends;
    tab[69] = h_strlower;  tab[70] = h_strupper;
    tab[77] = h_fsfree;    tab[78] = h_ramfree;   tab[79] = h_vsprintf; tab[80] = h_vsnprintf;
    tab[81] = h_snprintf;  tab[83] = strlcpy;     tab[84] = strlcat;    tab[95] = h_strtod;
    tab[97] = h_sleep;     tab[106] = h_seconds;  tab[109] = qsort;
    return tab;
}
