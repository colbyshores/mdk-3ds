// ============================================================================
// gstest -- a hardware probe: does MDK's lit geometry shader run on THIS
// console, and if not, which of its conditions stops the GPU?
//
// Written 2026-11-18 after the Old 3DS appliance and the owner's New 3DS both
// hung on the first frame drawn with enableLighting (GDB: the main thread in
// C3D_FrameBegin's queue wait; the GX queue stuck on the frame's command list).
// It embeds MDK's OWN shader binary (mdk_3ds/build/mdk_c3d.shbin, all four
// DVLEs) and draws a triangle through it in stages, each logged and flushed to
// sdmc:/3ds/gstest.log BEFORE it starts, so the last line names the first
// stage that hangs. A small .3dsx pushes in seconds; MDK's takes minutes.
//
// Not part of the game build. Build: make -C mdk_3ds/tools/gstest
// (needs mdk_3ds/build/mdk_c3d.shbin, i.e. a built mdk_3ds).
// ============================================================================

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

extern const u8 mdk_shbin[];
extern const u32 mdk_shbin_size;

static FILE* g_log;

static void logf_(const char* fmt, const char* a, int b) {
    if (!g_log) {
        return;
    }
    fprintf(g_log, fmt, a, b);
    fputc('\n', g_log);
    fflush(g_log);
}

// hbmenu-style chain-load back to the netload stub (mdk_3ds harness_3ds.cpp).
static void chainloadStub(void) {
    static char argvBuffer[0x400];
    const char* path = "/3ds/netload_stub.3dsx";
    Handle hbldr = 0;
    if (R_FAILED(svcConnectToPort(&hbldr, "hb:ldr"))) {
        return;
    }
    u32* cmdbuf = getThreadCommandBuffer();
    const u32 pathLength = (u32)strlen(path) + 1;
    cmdbuf[0] = IPC_MakeHeader(2, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(pathLength, 0);
    cmdbuf[2] = (u32)path;
    Result rc = svcSendSyncRequest(hbldr);
    if (R_SUCCEEDED(rc)) {
        rc = (Result)cmdbuf[1];
    }
    if (R_FAILED(rc)) {
        svcCloseHandle(hbldr);
        return;
    }
    ((u32*)argvBuffer)[0] = 1;
    snprintf(argvBuffer + 4, sizeof(argvBuffer) - 4, "sdmc:%s", path);
    cmdbuf[0] = IPC_MakeHeader(3, 0, 2);
    cmdbuf[1] = IPC_Desc_StaticBuffer(sizeof(argvBuffer), 1);
    cmdbuf[2] = (u32)argvBuffer;
    svcSendSyncRequest(hbldr);
    svcCloseHandle(hbldr);
}

// MDK's vertex: position xyz + uv (06_draw.inc BufInfo_Add stride 5 floats).
typedef struct {
    float x, y, z, u, v;
} Vertex;

static const Vertex kTri[3] = {
    {-0.5f, -0.5f, 0.5f, 0.0f, 0.0f},
    {0.5f, -0.5f, 0.5f, 1.0f, 0.0f},
    {0.0f, 0.5f, 0.5f, 0.5f, 1.0f},
};

static shaderProgram_s g_plain, g_lit, g_pass;
static int g_mvp, g_uv, g_tint, g_haze, g_world;
static int g_sun, g_sunCol, g_sky, g_ground, g_camera, g_params;
static void* g_vbo;

static void plainUniforms(void) {
    C3D_Mtx id;
    Mtx_Identity(&id);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_mvp, &id);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uv, 1.0f, 1.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_tint, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_haze, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_world, &id);
}

// The hung frame's own values (decoded from its command list).
static void litUniforms(void) {
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, g_sun, -0.329f, 0.4698f, -0.8192f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, g_sunCol, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, g_sky, 1.293f, 0.9268f, 0.8635f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, g_ground, 0.9048f, 0.6488f, 0.6045f, 0.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, g_camera, 0.0f, -4.0f, 4.5f, 1.0f);
    C3D_FVUnifSet(GPU_GEOMETRY_SHADER, g_params, 1.327f, 0.0f, 0.0f, 0.0f);
}

static void drawPlain(void) {
    C3D_BindProgram(&g_plain);
    plainUniforms();
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
}

static void drawLit(void) {
    C3D_BindProgram(&g_lit);
    plainUniforms();
    litUniforms();
    C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, 3);
}

// The plain-looking draw through the pass-through GS (DVLE[4], gstest only):
// a frame that has used the GS never leaves geometry mode.
static void drawPass(void) {
    C3D_BindProgram(&g_pass);
    plainUniforms();
    C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, 3);
}

// THE GLOW'S SHRINK (06_draw.inc drawGlow at edge-smoothing scale 2): a
// 480x800 RGBA8 eye in VRAM, 2x2-averaged into linear memory by the display
// transfer engine. Each variant below is one of the stages 20-23.
static u32* g_vramEye;
static u32* g_scratch;
static u32* g_scratch2;
static C3D_Tex g_glowTex;
static void shrinkTransfer(int stage) {
    const u32 in = GX_BUFFER_DIM(480, 800);
    const u32 half = GX_BUFFER_DIM(240, 400);
    const u32 rgba = GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                     GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8);
    switch (stage) {
    case 20:  // MDK's: tiled->tiled (bit 5), 2x2, output dims = input dims
        GX_DisplayTransfer(g_vramEye, in, g_scratch, in,
                           BIT(5) | rgba | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
        break;
    case 21:  // tiled->tiled, 2x2, output dims halved
        GX_DisplayTransfer(g_vramEye, in, g_scratch, half,
                           BIT(5) | rgba | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
        break;
    case 22:  // tiled->linear, 2x2 (the screen output's form)
        GX_DisplayTransfer(g_vramEye, in, g_scratch, in,
                           rgba | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
        break;
    case 23:  // tiled->tiled, no scaling (output the same size)
        GX_DisplayTransfer(g_vramEye, half, g_scratch, half, BIT(5) | rgba);
        break;
    case 24:  // the replacement, steps 1-2: tiled->linear 2x2, then linear->tiled
        GX_DisplayTransfer(g_vramEye, in, g_scratch, in,
                           rgba | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
        GX_DisplayTransfer(g_scratch, half, g_scratch2, half, rgba | GX_TRANSFER_OUT_TILED(1));
        break;
    case 25: {  // step 3: MDK's tile-row texture copy into the 256x512 glow texture
        const u32 rowBytes = 240u * 8u * 4u;
        const u32 texRowBytes = 256u * 8u * 4u;
        GX_TextureCopy(g_scratch2, GX_BUFFER_DIM(rowBytes >> 4, 0),
                       (u32*)C3D_Tex2DGetImagePtr(&g_glowTex, 0, NULL),
                       GX_BUFFER_DIM(rowBytes >> 4, (texRowBytes - rowBytes) >> 4),
                       rowBytes * 50u, GX_TRANSFER_RAW_COPY(1));
        break;
    }
    case 26:  // the whole replacement chain, as drawGlow will issue it
        shrinkTransfer(24);
        shrinkTransfer(25);
        break;
    case 27:  // tiled->tiled spelled with OUT_TILED as well as bit 5, 2x2
        GX_DisplayTransfer(g_vramEye, in, g_scratch, in,
                           BIT(5) | GX_TRANSFER_OUT_TILED(1) | rgba |
                               GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
        break;
    default:
        break;
    }
}

static void frame(C3D_RenderTarget* target, int stage) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x203040FF, 0);
    C3D_FrameDrawOn(target);
    switch (stage) {
    case 1: drawPlain(); break;
    case 2: drawLit(); break;
    case 3: drawLit(); drawPlain(); break;
    case 4: drawPlain(); drawLit(); drawPlain(); drawLit(); drawPlain(); break;
    // C1: drain -- the command list is split after the GS draw.
    case 5: drawLit(); C3D_FrameSplit(0); drawPlain(); break;
    // C2: full reconfigure of the plain program on the way back.
    case 6:
        drawLit();
        C3D_BindProgram(&g_plain);
        shaderProgramUse(&g_plain);
        plainUniforms();
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
        break;
    // C3: never leave geometry mode -- lit, then the pass-through GS.
    case 7: drawPass(); drawLit(); drawPass(); drawLit(); drawPass(); break;
    // Plain BEFORE any GS in the frame, then GS to the end (the frame begins plain).
    case 8: drawPlain(); drawLit(); drawPass(); break;
    case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27:
        drawPlain();
        C3D_FrameSplit(0);
        shrinkTransfer(stage);
        break;
    default: break;
    }
    C3D_FrameEnd(0);
}

int main(void) {
    gfxInitDefault();
    g_log = fopen("sdmc:/3ds/gstest.log", "w");
    logf_("gstest: start (shbin %s%d bytes)", "", (int)mdk_shbin_size);
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C3D_RenderTarget* target =
        C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                                  GX_TRANSFER_RAW_COPY(0) |
                                  GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                  GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));

    DVLB_s* dvlb = DVLB_ParseFile((u32*)mdk_shbin, mdk_shbin_size);
    logf_("gstest: dvlb numDVLE=%s%d", "", dvlb ? (int)dvlb->numDVLE : -1);
    shaderProgramInit(&g_plain);
    shaderProgramSetVsh(&g_plain, &dvlb->DVLE[0]);
    shaderProgramInit(&g_lit);
    shaderProgramSetVsh(&g_lit, &dvlb->DVLE[0]);
    Result gsh = shaderProgramSetGsh(&g_lit, &dvlb->DVLE[1], 12);
    shaderProgramInit(&g_pass);
    shaderProgramSetVsh(&g_pass, &dvlb->DVLE[0]);
    Result pgsh = shaderProgramSetGsh(&g_pass, &dvlb->DVLE[4], 12);
    logf_("gstest: pass gsh result %s%d", "", (int)pgsh);
    logf_("gstest: lit gsh result %s%d", "", (int)gsh);
    g_mvp = shaderInstanceGetUniformLocation(g_plain.vertexShader, "mvp");
    g_uv = shaderInstanceGetUniformLocation(g_plain.vertexShader, "uvtransform");
    g_tint = shaderInstanceGetUniformLocation(g_plain.vertexShader, "tint");
    g_haze = shaderInstanceGetUniformLocation(g_plain.vertexShader, "haze");
    g_world = shaderInstanceGetUniformLocation(g_plain.vertexShader, "world");
    shaderInstance_s* gs = g_lit.geometryShader;
    g_sun = shaderInstanceGetUniformLocation(gs, "sunDir");
    g_sunCol = shaderInstanceGetUniformLocation(gs, "sunColour");
    g_sky = shaderInstanceGetUniformLocation(gs, "skyColour");
    g_ground = shaderInstanceGetUniformLocation(gs, "groundColour");
    g_camera = shaderInstanceGetUniformLocation(gs, "camera");
    g_params = shaderInstanceGetUniformLocation(gs, "params");
    logf_("gstest: locations ok=%s%d", "",
          g_mvp >= 0 && g_uv >= 0 && g_tint >= 0 && g_haze >= 0 && g_world >= 0 && g_sun >= 0 &&
              g_sunCol >= 0 && g_sky >= 0 && g_ground >= 0 && g_camera >= 0 && g_params >= 0);

    g_vramEye = (u32*)vramAlloc(480 * 800 * 4);
    g_scratch = (u32*)linearAlloc(480 * 800 * 4);
    g_scratch2 = (u32*)linearAlloc(240 * 400 * 4);
    const bool texOk = C3D_TexInitVRAM(&g_glowTex, 256, 512, GPU_RGBA8);
    logf_("gstest: glow texture in VRAM %s%d", texOk ? "ok" : "FAILED", 0);
    logf_("gstest: transfer buffers vram=%s%d", g_vramEye && g_scratch ? "" : "FAILED ",
          (int)(480 * 800 * 4));
    g_vbo = linearAlloc(sizeof(kTri));
    memcpy(g_vbo, kTri, sizeof(kTri));
    GSPGPU_FlushDataCache(g_vbo, sizeof(kTri));

    C3D_BindProgram(&g_plain);
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g_vbo, sizeof(Vertex), 2, 0x10);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

    static const char* const kNames[] = {"", "plain only", "lit GS only",
                                         "lit then plain", "plain/lit x2",
                                         "C1 lit, FrameSplit, plain",
                                         "C2 lit, plain + shaderProgramUse",
                                         "C3 pass/lit/pass/lit/pass (never leave GS)",
                                         "plain first, then lit, then pass",
                                         "", "", "", "", "", "", "", "", "", "", "",
                                         "T20 tiled->tiled 2x2, out dims = in (MDK glow)",
                                         "T21 tiled->tiled 2x2, out dims halved",
                                         "T22 tiled->linear 2x2 (screen output form)",
                                         "T23 tiled->tiled no scaling",
                                         "T24 tiled->linear 2x2 then linear->tiled",
                                         "T25 tile-row texture copy into the glow texture",
                                         "T26 the replacement chain (T24 + T25)",
                                         "T27 bit5 + OUT_TILED, 2x2"};
#ifndef GSTEST_STAGES
#define GSTEST_STAGES 1, 2, 3
#endif
    static const int kStages[] = {GSTEST_STAGES};
    for (unsigned si = 0; si < sizeof(kStages) / sizeof(kStages[0]) && aptMainLoop(); si++) {
        const int stage = kStages[si];
        logf_("gstest: stage %s (%d) START", kNames[stage], stage);
        for (int f = 0; f < 90 && aptMainLoop(); f++) {
            frame(target, stage);
        }
        logf_("gstest: stage %s (%d) survived 90 frames", kNames[stage], stage);
    }
    logf_("gstest: done%s%d", "", 0);
    if (g_log) {
        fclose(g_log);
    }
    linearFree(g_vbo);
    shaderProgramFree(&g_pass);
    shaderProgramFree(&g_lit);
    shaderProgramFree(&g_plain);
    DVLB_Free(dvlb);
    C3D_RenderTargetDelete(target);
    C3D_Fini();
    gfxExit();
    chainloadStub();
    return 0;
}
