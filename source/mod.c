// loads the brew .mod straight into an rwx block and lets its own stub relocate it
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "aee.h"

// header words past the BREW magic, offsets from file start
#define H_CODE_OFF  0x18
#define H_CODE_SZ   0x1c
#define H_DATA_SZ   0x20
#define H_BSS_SZ    0x24

static u8 *block;

static u32 rd32(const u8 *p, int o) { return p[o] | p[o+1] << 8 | p[o+2] << 16 | (u32)p[o+3] << 24; }

int mod_load(const char *path, void *shell, void *helpers, void **pmod)
{
    FILE *f = fopen(path, "rb");
    if (!f) { dbg("mod: cant open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    u8 hdr[0x40];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr + 8, "BREW", 4)) {
        dbg("mod: not a BREW header mod");
        fclose(f);
        return -1;
    }
    // bss overlays the reloc table, so the image can be bigger than the file
    u32 img = rd32(hdr, H_CODE_OFF) + rd32(hdr, H_CODE_SZ) + rd32(hdr, H_DATA_SZ) + rd32(hdr, H_BSS_SZ);
    if (img < (u32)len) img = len;
    img = (img + 0xfff) & ~0xfff;

    // one page in front for the two words the stub copies in (helper ver, helper table)
    block = memalign(0x1000, img + 0x1000);
    if (!block) { fclose(f); return -2; }
    memset(block, 0, img + 0x1000);
    u8 *base = block + 0x1000;
    fseek(f, 0, SEEK_SET);
    fread(base, 1, len, f);
    fclose(f);

    ((u32 *)base)[-2] = 1;
    ((u32 *)base)[-1] = (u32)helpers;

    Handle self;
    Result r = svcDuplicateHandle(&self, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(r))
        r = svcControlProcessMemory(self, (u32)block, 0, img + 0x1000, MEMOP_PROT,
                                    MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    // emulators dont enforce nx, so keep going and let real hw fault if it has to
    if (R_FAILED(r)) dbg("mod: rwx failed %08lx (luma + svc access needed)", r);
    // fresh pages never executed, so a dcache clean is all the icache needs
    svcFlushProcessDataCache(self, (u32)block, img + 0x1000);
    svcCloseHandle(self);

    dbg("mod: %s at %p, %ld bytes, image %lx", path, base, len, img);

    int (*entry)(void *, void *, void **) = (void *)(base + 0x40);
    int rc = entry(shell, helpers, pmod);
    dbg("mod: AEEMod_Load -> %d, IModule %p", rc, *pmod);
    return rc;
}
