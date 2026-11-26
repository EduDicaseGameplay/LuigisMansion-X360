// Widescreen support on the game side. LuigisMansion-X360, own code; the technique is the LM PC port's
// (Reference/LuigisMansionPC/src/recomp/widescreen.cpp, docs/ENHANCEMENTS.md "Widescreen"), re-written here.
//
// The GX core (Source/thirdparty/sms_gx) widens the picture: the camera's projection gets a wider horizontal field of
// view (Hor+). The game still culls objects against its 4:3 frustum, so anything in the new side areas would pop in
// and out. fn_80008D58 (USA name; PAL 8000935C) builds the camera's four side culling planes from
// (camera r3, top f1, bottom f2, left f3, right f4, ...); every 3D projection setter ends with it. For a full-screen
// camera, left and right are widened by the same factor as the picture.
#include "../runtime/runtime.h"
#include "../runtime/log.h"

extern "C" float GXPC_GetWidescreen(void);
extern "C" void GXPC_SetShadowPass(int on);  // Source/thirdparty/sms_gx (xe_gx_render.cpp)

// The game's shadow-map pass flag (USA lbl_804D8080, PAL 804C5740 = r13 + 288; LM PC port version_addrs.inc): set by
// fn_8005D984 (PAL 800603F0, the light camera's setup), cleared by fn_8005DBE4 (PAL 80060650, the Z16 copy). While
// it is set the GX core draws without the Hor+ widening (LM PC port hle_gx.cpp drawMode; RENDERING.md "Shadows" fix 2).
static const u32 kShadowPassFlag = 0x804C5740u;
static bool shadow_pass() { return rd32(kShadowPassFlag) != 0; }
// The light's frustum (game design) only covers what the 4:3 picture shows: at 16:9 the receivers at the sides fall
// outside the shadow map. As the LM PC port does (shadows.cpp, RENDERING.md "Shadows" fix 3), the main camera's aspect
// (+0x94 of lbl_803A3820, PAL 80390AF0), which the game turns into the light's camera here, is widened for this
// setup call only: the light projection, the receivers' matrices and the light's culling are then all built by the
// game from the same wider frustum, consistent by construction.
static const u32 kMainCamera = 0x80390AF0u, kCamAspect = 0x94;
HLE_FUNC(fn_8005D984) {
    const float wide = GXPC_GetWidescreen();
    const u32 aspectBits = rd32(kMainCamera + kCamAspect);
    if (wide > 1.0f) {
        float a;
        memcpy(&a, &aspectBits, 4);
        a *= wide;
        u32 w;
        memcpy(&w, &a, 4);
        wr32(kMainCamera + kCamAspect, w);
    }
    hook_call_original(c, "fn_8005D984");
    if (wide > 1.0f) wr32(kMainCamera + kCamAspect, aspectBits);
    if (GXPC_GetWidescreen() > 1.0f && shadow_pass()) {
        GXPC_SetShadowPass(1);
        lm_log_once(0x5AD00001u, "[ws] shadow-map pass seen (flag %08X): drawn without Hor+", kShadowPassFlag);
    }
}
HLE_FUNC(fn_8005DBE4) {
    hook_call_original(c, "fn_8005DBE4");
    if (GXPC_GetWidescreen() > 1.0f) GXPC_SetShadowPass(shadow_pass() ? 1 : 0);
}
void lm_shadow_pass_frame_end() {  // hle_gx.cpp, at the game's display copy: never left on across frames
    if (GXPC_GetWidescreen() > 1.0f && !shadow_pass()) GXPC_SetShadowPass(0);
}

static float rdf(u32 a) {
    const u32 v = rd32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

HLE_FUNC(fn_80008D58) {
    const float wide = GXPC_GetWidescreen();
    if (wide > 1.0f) {
        const u32 cam = c.r[3];
        // European camera: viewport x at +0x1F8, width at +0x200 (USA +0x1EC/+0x1F4; LM PC port VOFF)
        const float vpX = rdf(cam + 0x1F8), vpW = rdf(cam + 0x200);
        if (vpX == 0.0f && vpW >= 639.0f && !shadow_pass()) {  // not the light camera (LM PC port: sShadowLightSetup)
            c.f[3] = (double)(float)(c.f[3] * wide);
            c.f[4] = (double)(float)(c.f[4] * wide);
            c.ps1[3] = c.f[3];
            c.ps1[4] = c.f[4];
        }
    }
    hook_call_original(c, "fn_80008D58");
}
