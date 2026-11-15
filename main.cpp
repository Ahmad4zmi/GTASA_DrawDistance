/*
 *  GTA:SA Draw Distance  -  arm64-v8a only
 *
 *  Every offset below belongs to the 64-bit libGTASA.so (found with IDA, the library has symbols).
 *  Nothing is blindly written: GOT slots are checked against the function they must point to and
 *  every code patch checks the original instruction first, so a different game version just logs
 *  an error and skips the feature instead of crashing the game.
 */
#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <math.h>

#include "isautils.h"
ISAUtils* sautils = nullptr;

#ifndef __aarch64__
    #error "This mod is arm64-v8a only (all offsets are for the 64-bit libGTASA.so)"
#endif

MYMODCFG(net.aml.gtasa.streamingfix, GTA:SA Streaming Fix, 0.2, AMDGamerYT)
NEEDGAME(com.rockstargames.gtasa)
BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.0)
END_DEPLIST()

/* ------------------------------------------------------------------------------------------------
 *  Offsets (arm64 libGTASA.so)
 * ---------------------------------------------------------------------------------------------- */
// GOT slots (what HookPLT needs) and the function each of them must point to
#define GOT_RwCameraSetNearClipPlane   0x841838
#define FN_RwCameraSetNearClipPlane    0x26CF8C
#define GOT_RwCameraSetFarClipPlane    0x846D60
#define FN_RwCameraSetFarClipPlane     0x26D034
#define GOT_CCamera_Process            0x8429D8
#define FN_CCamera_Process             0x4BAB78
#define GOT_CStreaming_Update          0x845FD0
#define FN_CStreaming_Update           0x392478

#define VAR_Scene_camera               0xC9B378 // Scene (0xC9B370) + 8
#define VAR_CStreaming_ms_memoryAvailable 0x85EBD8
#define FN_OS_ScreenGetWidth           0x31E0B8
#define FN_OS_ScreenGetHeight          0x31E0C8

// CCamera fields
#define CCAMERA_LOD_DIST_MULT          0xF4
#define CCAMERA_GEN_DIST_MULT          0xF8

// Water
#define GOT_BlocksToBeRenderedX        0x84CFB0 // CWaterLevel::m_BlocksToBeRenderedOutsideWorldX
#define GOT_BlocksToBeRenderedY        0x84F530 // CWaterLevel::m_BlocksToBeRenderedOutsideWorldY
#define INSN_WaterBlockLimit           0x6BCECC // CWaterLevel::BlockHit: CMP W9, #69

/* ------------------------------------------------------------------------------------------------
 *  State
 * ---------------------------------------------------------------------------------------------- */
uintptr_t pGTASA = 0;

// Cached config values. The hooks run every frame, so they must never parse the config.
float fNearClip = 0.1f;          // the near plane is never HIGHER than this (0 = don't touch)
float fDrawDistanceScale = 1.0f; // far plane multiplier, 1.0 = what the game wants
float fStreamingScale = 1.0f;
float fMaxDepthRatio = 6144.0f;  // far / near limit, 0 = no limit
int   nStreamingMemoryOverride = 0; // bytes

float fCurrentFar = 0.0f;        // far plane of the main camera after our scaling

ConfigEntry* pDrawDistanceCfg = nullptr;
ConfigEntry* pStreamingScaleCfg = nullptr;

int* streamingMemoryAvailable = nullptr;
int16_t* pBlocksToBeRenderedOutsideWorldX = nullptr;
int16_t* pBlocksToBeRenderedOutsideWorldY = nullptr;

#define DEFAULT_DRAWDISTANCE_SLIDER 800.0f // slider value that means "x1.00"

typedef unsigned int (*RetUIntFn)();

/* ------------------------------------------------------------------------------------------------
 *  Helpers
 * ---------------------------------------------------------------------------------------------- */
static inline void* GetMainCamera()
{
    return *(void**)(pGTASA + VAR_Scene_camera); // RwCamera* Scene.camera
}

// Real screen aspect ratio (always >= 1). Lazy, because the screen size is not known when the mod loads.
static float GetRealAspectRatio()
{
    static float fCached = 0.0f;
    if(fCached > 0.0f) return fCached;

    float w = (float)((RetUIntFn)(pGTASA + FN_OS_ScreenGetWidth))();
    float h = (float)((RetUIntFn)(pGTASA + FN_OS_ScreenGetHeight))();
    if(w < 1.0f || h < 1.0f) return 4.0f / 3.0f; // not ready yet, ask again next frame

    float fRatio = w / h;
    if(!isfinite(fRatio) || fRatio <= 0.0f) return 4.0f / 3.0f;
    if(fRatio < 1.0f) fRatio = 1.0f / fRatio;
    return fCached = fRatio;
}

/* ------------------------------------------------------------------------------------------------
 *  Hooks
 * ---------------------------------------------------------------------------------------------- */
// The game sets the far plane from the time cycle (weather, time of day, interiors...), so instead of
// replacing it with one fixed number the value is MULTIPLIED. x1.0 behaves exactly like the vanilla game.
DECL_HOOK(void*, RwCameraSetFarClipPlane, void* self, float a1)
{
    if(self == GetMainCamera())
    {
        // CCamera::Process passes the current far plane back (rounded to 2 decimals).
        // That is our own, already scaled value -> must not be scaled a second time.
        bool bOwnValue = fCurrentFar > 0.0f && fabsf(a1 - fCurrentFar) < 0.02f;
        if(!bOwnValue && fDrawDistanceScale != 1.0f && a1 > 1.0f)
        {
            a1 *= fDrawDistanceScale;
        }
        fCurrentFar = a1;
    }
    return RwCameraSetFarClipPlane(self, a1);
}

// Depth buffer precision gets worse with a bigger far/near ratio. With a huge far plane and a tiny near
// plane the depth buffer runs out of precision and distant textures flicker (z-fighting).
// So: the near plane is lowered to NearClip (the game often uses 0.3 - 0.9, which clips into cars/walls),
// but raised again as much as needed to keep far/near under MaxDepthRatio.
DECL_HOOK(void*, RwCameraSetNearClipPlane, void* self, float a1)
{
    if(self == GetMainCamera())
    {
        if(fNearClip > 0.0f && a1 > fNearClip) a1 = fNearClip;

        if(fMaxDepthRatio > 0.0f && fCurrentFar > 0.0f)
        {
            float fMinNear = fCurrentFar / fMaxDepthRatio;
            if(a1 < fMinNear) a1 = fMinNear;
        }
    }
    return RwCameraSetNearClipPlane(self, a1);
}

// The game resets LOD / generation multipliers every frame in CCamera::Process, so scaling them after it
// is safe (it never stacks up).
DECL_HOOK(void*, CameraProcess, uintptr_t self)
{
    void* ret = CameraProcess(self);

    float fScaler = GetRealAspectRatio() * 0.75f * fStreamingScale; // AspectRatio / 4:3
    if(isfinite(fScaler) && fScaler > 0.0f)
    {
        *(float*)(self + CCAMERA_LOD_DIST_MULT) *= fScaler;
        *(float*)(self + CCAMERA_GEN_DIST_MULT) *= fScaler;
    }
    return ret;
}

DECL_HOOK(void*, CStreamingUpdate, void* self)
{
    if(*streamingMemoryAvailable < nStreamingMemoryOverride)
        *streamingMemoryAvailable = nStreamingMemoryOverride;
    return CStreamingUpdate(self);
}

/* ------------------------------------------------------------------------------------------------
 *  SAUtils sliders
 * ---------------------------------------------------------------------------------------------- */
char szSliderText[16];

void DrawDistanceChanged(int oldVal, int newVal)
{
    fDrawDistanceScale = newVal / DEFAULT_DRAWDISTANCE_SLIDER;
    pDrawDistanceCfg->SetInt(newVal);
    cfg->Save();
}
const char* DrawDistanceDraw(int nNewValue)
{
    snprintf(szSliderText, sizeof(szSliderText), "x%.2f", nNewValue / DEFAULT_DRAWDISTANCE_SLIDER);
    return szSliderText;
}
void StreamingDistanceChanged(int oldVal, int newVal)
{
    fStreamingScale = 0.01f * newVal;
    pStreamingScaleCfg->SetFloat(fStreamingScale);
    cfg->Save();
}
const char* StreamingDistanceDraw(int nNewValue)
{
    snprintf(szSliderText, sizeof(szSliderText), "x%.2f", nNewValue / 100.0f);
    return szSliderText;
}

/* ------------------------------------------------------------------------------------------------
 *  AArch64 patching
 * ---------------------------------------------------------------------------------------------- */
struct Insn
{
    uintptr_t offset;   // in libGTASA.so
    uint32_t  expected; // what must be there now
    uint32_t  value;    // what to write
};

#define A64_NOP         0xD503201Fu
#define A64_MOVZ_W8(hi) (0x52A00008u | ((uint32_t)(hi) << 5)) // MOVZ W8, #hi, LSL #16
#define A64_FMOV_S3_W8  0x1E270103u                           // FMOV S3, W8
#define A64_FMOV_S7_W8  0x1E270107u                           // FMOV S7, W8

static inline uint32_t A64_B(uintptr_t from, uintptr_t to) // B <to>, placed at <from> (both are offsets)
{
    return 0x14000000u | ((uint32_t)(((intptr_t)(to - from) >> 2)) & 0x03FFFFFFu);
}

// All instructions are checked first, and only then written. Nothing is half-patched.
static bool Patch(const char* szName, const Insn* aInsns, int nCount)
{
    for(int i = 0; i < nCount; ++i)
    {
        uint32_t nCur = *(uint32_t*)(pGTASA + aInsns[i].offset);
        if(nCur != aInsns[i].expected)
        {
            logger->Error("%s: unexpected instruction 0x%08X at +0x%X (expected 0x%08X), skipped. Wrong game version?",
                          szName, nCur, (unsigned)aInsns[i].offset, aInsns[i].expected);
            return false;
        }
    }
    for(int i = 0; i < nCount; ++i)
    {
        uint32_t nValue = aInsns[i].value;
        aml->Write(pGTASA + aInsns[i].offset, (uintptr_t)&nValue, sizeof(nValue));
    }
    return true;
}

// HookPLT only after the GOT slot is proven to hold the expected function
static bool HookGOT(const char* szName, uintptr_t nGotOffset, uintptr_t nFuncOffset, void* pHook, void** ppOriginal)
{
    if(*(uintptr_t*)(pGTASA + nGotOffset) != pGTASA + nFuncOffset)
    {
        logger->Error("%s: GOT slot +0x%X does not point to the expected function, not hooked. Wrong game version?",
                      szName, (unsigned)nGotOffset);
        return false;
    }
    aml->HookPLT((void*)(pGTASA + nGotOffset), pHook, ppOriginal);
    return true;
}
#define HOOK_GOT(_name) HookGOT(#_name, GOT_##_name, FN_##_name, (void*)&HookOf_##_name, (void**)&_name)

/* ------------------------------------------------------------------------------------------------
 *  Entry point
 * ---------------------------------------------------------------------------------------------- */
static void InstallWaterPatch(unsigned int nBlocks)
{
    // Vanilla: 70 blocks (0..69). Bigger arrays are needed for more.
    if(nBlocks <= 70) return;
    if(nBlocks > 4096)
    {
        logger->Error("WaterBlocksToRender is too big (max 4096)");
        return;
    }

    const Insn aLimit[] = { { INSN_WaterBlockLimit, 0x7101153F, 0x7100013Fu | ((nBlocks - 1) << 10) } };
    if(*(uint32_t*)(pGTASA + aLimit[0].offset) != aLimit[0].expected)
    {
        logger->Error("WaterBlocksToRender: unexpected instruction at +0x%X, skipped. Wrong game version?", (unsigned)aLimit[0].offset);
        return;
    }

    // The bigger arrays go in FIRST, so the game can never write past the end of the old 70 entry ones.
    // The game reads both arrays through GOT slots, so replacing the slot swaps the array.
    pBlocksToBeRenderedOutsideWorldX = new int16_t[nBlocks]();
    aml->Write(pGTASA + GOT_BlocksToBeRenderedX, (uintptr_t)&pBlocksToBeRenderedOutsideWorldX, sizeof(void*));
    pBlocksToBeRenderedOutsideWorldY = new int16_t[nBlocks]();
    aml->Write(pGTASA + GOT_BlocksToBeRenderedY, (uintptr_t)&pBlocksToBeRenderedOutsideWorldY, sizeof(void*));

    // CWaterLevel::BlockHit: `CMP W9, #69 ; B.GT <skip>`  ->  `CMP W9, #(nBlocks-1)`
    Patch("WaterBlocksToRender", aLimit, 1);
}

static void InstallTrafficPatches(bool bRemoveCarsBehind, int nOutOfSightSpawnDist)
{
    // CCarCtrl::PossiblyRemoveVehicle: a car that is out of sight is removed when it is farther than 45m
    // (looking forward) - that is the "behind the camera" removal. Always use the wide range (170 * multiplier).
    //   TBNZ W0, #0, <wide>   ->   B <wide>
    if(bRemoveCarsBehind)
    {
        const Insn a[] = { { 0x3B06E4, 0x37000220, A64_B(0x3B06E4, 0x3B0728) } };
        Patch("DontRemoveVehicleBehindCamera", a, 1);
    }

    // CCarCtrl::GenerateOneRandomCar creates traffic in two places:
    //   - in front of you, on screen, 160 * multiplier meters away
    //   - out of sight (behind / next to you) exactly 38 meters away   <- cars popping up right behind you
    // then it throws away every out of sight car that is farther than 45 meters.
    // Both numbers come from constants shared with a lot of other code, so instead of editing them the
    // instructions that load them are replaced with `MOVZ W8, #(value >> 16), LSL #16` + `FMOV Sx, W8`
    // (every whole number up to 255 has its lower 16 bits cleared, so it fits).
    if(nOutOfSightSpawnDist != 38)
    {
        float fCreate = (float)nOutOfSightSpawnDist;
        float fKeep = (float)(nOutOfSightSpawnDist + 25 > 255 ? 255 : nOutOfSightSpawnDist + 25); // a bit of room for the lane offset
        uint32_t nCreateBits = *(uint32_t*)&fCreate;
        uint32_t nKeepBits = *(uint32_t*)&fKeep;

        const Insn a[] = {
            // ADRP X8, 38.0 ; LDR S7, [X8, 38.0]     ->  distance where out of sight cars are created
            { 0x3AC09C, 0xD0001C68, A64_MOVZ_W8(nCreateBits >> 16) },
            { 0x3AC0A0, 0xBD4C3507, A64_FMOV_S7_W8 },
            // ADRP X8, 45.0 ; ... ; LDR S3, [X8, 45.0] -> farthest distance an out of sight car may have
            { 0x3AD4EC, 0xB0001C68, A64_MOVZ_W8(nKeepBits >> 16) },
            { 0x3AD4F4, 0xBD4B8D03, A64_FMOV_S3_W8 },
        };
        Patch("OutOfSightVehicleSpawnDistance", a, 4);
    }
}

extern "C" void OnModLoad()
{
    logger->SetTag("GTASA Draw Distance");
    pGTASA = aml->GetLib("libGTASA.so");
    if(pGTASA == 0)
    {
        logger->Error("libGTASA.so was not found, nothing to do");
        return;
    }

    /* ---- Config ---- */
    fNearClip = cfg->Bind("NearClip", "0.1")->GetFloat();
    pDrawDistanceCfg = cfg->Bind("DrawDistance", "800.0");
    fDrawDistanceScale = pDrawDistanceCfg->GetFloat() / DEFAULT_DRAWDISTANCE_SLIDER;
    pStreamingScaleCfg = cfg->Bind("StreamingDistanceScale", "1.0");
    fStreamingScale = pStreamingScaleCfg->GetFloat();
    fMaxDepthRatio = cfg->Bind("MaxDepthRatio", "6144")->GetFloat();

    if(!isfinite(fDrawDistanceScale) || fDrawDistanceScale < 0.25f) fDrawDistanceScale = 1.0f; // 0 / garbage = off
    if(!isfinite(fStreamingScale) || fStreamingScale <= 0.0f) fStreamingScale = 1.0f;

    int nStreamingMemMB = cfg->Bind("PreferredStreamingMemMB", "1024")->GetInt();
    if(nStreamingMemMB > 2047)
    {
        logger->Error("PreferredStreamingMemMB is too big (max 2047)");
        nStreamingMemMB = 2047;
    }
    nStreamingMemoryOverride = (nStreamingMemMB > 0 ? nStreamingMemMB : 0) * 1024 * 1024;

    unsigned int nWaterBlocks = (unsigned int)cfg->Bind("WaterBlocksToRender", "384")->GetInt();
    bool bMoreOftenPopulation = cfg->Bind("MoreOftenPopulationUpdate", "1")->GetBool();
    bool bKeepCarsBehind = cfg->Bind("DontRemoveVehicleBehindCamera", "1")->GetBool();
    bool bKeepPedsBehind = cfg->Bind("DontRemovePedBehindCamera", "1")->GetBool();

    // Where traffic is created when it is out of sight. 38 = vanilla. Cars popping up right behind you
    // is the vanilla behaviour; farther is much less noticeable.
    int nOutOfSightSpawnDist = cfg->Bind("OutOfSightVehicleSpawnDistance", "80")->GetInt();
    if(nOutOfSightSpawnDist < 38 || nOutOfSightSpawnDist > 230)
    {
        logger->Error("OutOfSightVehicleSpawnDistance must be 38-230, using 38 (vanilla)");
        nOutOfSightSpawnDist = 38;
    }
    // A car created out of sight farther than 45m would be removed on the same frame otherwise
    if(nOutOfSightSpawnDist != 38) bKeepCarsBehind = true;

    /* ---- Hooks ---- */
    streamingMemoryAvailable = (int*)(pGTASA + VAR_CStreaming_ms_memoryAvailable);

    HOOK_GOT(RwCameraSetFarClipPlane);
    HOOK_GOT(RwCameraSetNearClipPlane);
    HookGOT("CameraProcess", GOT_CCamera_Process, FN_CCamera_Process, (void*)&HookOf_CameraProcess, (void**)&CameraProcess);
    HookGOT("CStreamingUpdate", GOT_CStreaming_Update, FN_CStreaming_Update, (void*)&HookOf_CStreamingUpdate, (void**)&CStreamingUpdate);

    /* ---- Patches ---- */
    InstallWaterPatch(nWaterBlocks);

    // CGame::Process only calls CPopulation::Update a second time when a (performance) setting is >= 2.
    //   B.LT <skip>  ->  NOP
    if(bMoreOftenPopulation)
    {
        const Insn a[] = { { 0x4D6450, 0x5400006B, A64_NOP } };
        Patch("MoreOftenPopulationUpdate", a, 1);
    }

    // CPopulation::ManagePed: peds that are out of sight (behind the camera) are removed even when they are
    // close. `CBZ W9, <end>`  ->  `B <end>`
    if(bKeepPedsBehind)
    {
        const Insn a[] = { { 0x5CDB80, 0x34000429, A64_B(0x5CDB80, 0x5CDC04) } };
        Patch("DontRemovePedBehindCamera", a, 1);
    }

    InstallTrafficPatches(bKeepCarsBehind, nOutOfSightSpawnDist);

    /* ---- Menu ---- */
    sautils = (ISAUtils*)GetInterface("SAUtils");
    if(sautils != nullptr)
    {
        sautils->AddSliderItem(Display, "Real Draw Distance", (int)(fDrawDistanceScale * DEFAULT_DRAWDISTANCE_SLIDER), 200, 1400, DrawDistanceChanged, DrawDistanceDraw);
        sautils->AddSliderItem(Display, "Streaming Distance Scale", (int)(100 * fStreamingScale), 100 * 0.25f, 100 * 5.0f, StreamingDistanceChanged, StreamingDistanceDraw);
    }
}
