// boot time gpu bisect, no game involved.
//   X held: clear + present only (no command list reaches the gpu)
//   Y held: staged draws, from a stock citro3d triangle up to the full gl path.
//           the first stage that never finishes is the one to look at.
#include <stdio.h>
#include "aee.h"
#include "gl.h"

bool gl_selftest_frame(int stage, int f);
void gl_selftest_end(void);
bool gl_selftest_wait(void);

static const char *stage_name[] = {
    "", "stock shader, float attribs", "our shader, all inputs from buffer",
    "our shader, constant attribs", "short positions", "float uv + texture",
    "gl fragment state + scissor", "full gl path (test scene)",
};

#define FX1 65536

static void scene(int f)
{
    static GLuint tex;
    static u8 pix[64 * 64 * 4];
    static const s16 quad[] = { -200, -150, -200, 150, 200, -150, 200, 150 };
    static const s16 small[] = { -100, -75, -100, 75, 100, -75, 100, 75 };
    static const GLfixed uv[] = { 0, FX1, 0, 0, FX1, FX1, FX1, 0 };

    if (!tex) {
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                u8 *p = pix + (y * 64 + x) * 4;
                bool on = ((x >> 3) ^ (y >> 3)) & 1;
                p[0] = on ? 255 : 30; p[1] = on ? 200 : 30; p[2] = on ? 40 : 30; p[3] = 255;
            }
        gl_GenTextures(1, &tex);
        gl_BindTexture(0x0DE1, tex);
        gl_TexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, pix);
    }
    gl_MatrixMode(0x1701);
    gl_LoadIdentity();
    gl_Orthox(-320 * FX1, 320 * FX1, -240 * FX1, 240 * FX1, -FX1, FX1);
    gl_MatrixMode(0x1700);
    gl_LoadIdentity();
    gl_Rotatex((f * 3 % 360) * FX1, 0, 0, FX1);
    gl_EnableClientState(0x8074);
    gl_VertexPointer(2, 0x1402, 0, quad);
    gl_Disable(0x0DE1);
    gl_Color4x(FX1 / 4, FX1 / 2, FX1, FX1);
    gl_DrawArrays(5, 0, 4);
    gl_LoadIdentity();
    gl_Enable(0x0DE1);
    gl_BindTexture(0x0DE1, tex);
    gl_EnableClientState(0x8078);
    gl_TexCoordPointer(2, 0x140C, 0, uv);
    gl_VertexPointer(2, 0x1402, 0, small);
    gl_Color4x(FX1, FX1, FX1, FX1);
    gl_DrawArrays(5, 0, 4);
    gl_DisableClientState(0x8078);
}

static void wait_start(void)
{
    printf("press START\n");
    while (aptMainLoop()) {
        g_beat++;
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        svcSleepThread(16000000);
    }
}

static void fail(int stage, int f)
{
    printf("\x1b[31mSTAGE %d HUNG\x1b[0m at frame %d:\n  %s\n", stage, f, stage_name[stage]);
    dbg("gputest: STAGE %d HUNG at frame %d (%s)", stage, f, stage_name[stage]);
}

void gputest_run(bool draw)
{
    if (!draw) {
        printf("gpu test: clear only, 120 frames\n");
        dbg("gputest: clear only start");
        WHERE("gputest clear only");
        gl_Viewport(0, 0, ZW, ZH);
        u64 worst = 0;
        for (int f = 0; f < 120 && aptMainLoop(); f++) {
            g_beat++;
            u64 t0 = osGetTime();
            gl_ClearColorx(f & 1 ? FX1 / 2 : 0, 0, FX1 / 3, FX1);
            gl_Clear(0x4000 | 0x100);
            if (osGetTime() - t0 > worst) worst = osGetTime() - t0;
            gl_swap();
        }
        printf("clear only passed, worst wait %llu ms\n", worst);
        dbg("gputest: clear only passed, worst wait %llu ms", worst);
        wait_start();
        return;
    }

    printf("gpu test: staged draws\n");
    dbg("gputest: staged start");
    WHERE("gputest staged");
    for (int stage = 1; stage <= 6; stage++) {
        printf("stage %d: %s... ", stage, stage_name[stage]);
        dbg("gputest: stage %d (%s)", stage, stage_name[stage]);
        for (int f = 0; f < 10; f++) {
            g_beat++;
            if (!gl_selftest_frame(stage, f)) { fail(stage, f); wait_start(); return; }
            gl_selftest_end();
        }
        // the last frame of the stage has to finish too
        if (!gl_selftest_wait()) { fail(stage, 10); wait_start(); return; }
        gl_selftest_end();
        printf("ok\n");
        dbg("gputest: stage %d ok", stage);
    }

    printf("stage 7: %s... ", stage_name[7]);
    dbg("gputest: stage 7 (%s)", stage_name[7]);
    gl_Viewport(0, 0, ZW, ZH);
    for (int f = 0; f < 30 && aptMainLoop(); f++) {
        g_beat++;
        u64 t0 = osGetTime();
        gl_ClearColorx(0, 0, FX1 / 3, FX1);
        gl_Clear(0x4000 | 0x100);
        if (osGetTime() - t0 > 2000) { fail(7, f); wait_start(); return; }
        scene(f);
        gl_swap();
    }
    printf("ok\nall stages passed\n");
    dbg("gputest: all stages passed");
    wait_start();
}
