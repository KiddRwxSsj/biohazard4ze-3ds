#include <stdio.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>
#include "aee.h"

static FILE *lf;
static int lines;
static bool closed;

void dbg(const char *fmt, ...)
{
    // a teardown thread left running past dbg_close would reopen with "w" and wipe the log
    if (closed) return;
    if (!lf) {
        // keep the previous run's log, the run that broke is usually the one before the report
        remove(GAME_DIR "log_cia_prev.txt");
        rename(GAME_DIR "log_cia.txt", GAME_DIR "log_cia_prev.txt");
        lf = fopen(GAME_DIR "log_cia.txt", "w");
        if (!lf) return;
    }
    va_list v;
    va_start(v, fmt);
    vfprintf(lf, fmt, v);
    va_end(v);
    fputc('\n', lf);
    // logging is rare, flush every line so a crash keeps the tail
    fflush(lf);
    lines++;
}

void dbg_close(void)
{
    if (lf) fclose(lf);
    lf = NULL;
    closed = true;
}

extern u32 __ctru_heap, __ctru_heap_size;

// newlib free list plus whatever sbrk hasn't handed out yet
u32 mem_heap_free(void)
{
    struct mallinfo mi = mallinfo();
    u32 top = (u32)sbrk(0);
    return mi.fordblks + (__ctru_heap + __ctru_heap_size - top);
}
