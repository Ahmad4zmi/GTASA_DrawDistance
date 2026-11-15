#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <math.h>

#include "isautils.h"
ISAUtils* sautils = nullptr;

#if defined(__aarch64__)
    #define GTASA_ARM64 1
#endif

MYMODCFG(net.rusjj.gtasa.drawdistance, GTA:SA Draw Distance, 1.1, RusJJ)
NEEDGAME(com.rockstargames.gtasa)
BEGIN_DEPLIST()
    ADD_DEPENDENCY_VER(net.rusjj.aml, 1.0)
END_DEPLIST()

/* Saves */
uintptr_t pGTASA = 0;
int nStreamingMemoryOverride = 0; // bytes
unsigned int nWaterBlocksToRender = 0; // used by the 32-bit asm, keep it global!
uintptr_t pointerWaterBlocksToRender = 0; // used by the 32-bit asm, keep it global!

/* Cached config values (read every frame, so never parse the config there) */
float fNearClip = 0.0f;         // 0 = don't override
float fDrawDistance = 800.0f;   // <= 200 = don't override
float fStreamingScale = 1.0f;
float fMaxDepthRatio = 8192.0f; // far / near limit, 0 = don't limit
float fLastFarClip = 0.0f;      // last far plane of the main camera

/* Lib Pointers */
int* streamingMemoryAvailable;

#define DEFAULT_DRAWDISTANCE 800.0f

#ifdef GTASA_ARM64
    // CCamera::LODDistMultiplier / GenerationDistMultiplier
    #define CCAMERA_LOD_DIST_MULT 0xF4
    #define CCAMERA_GEN_DIST_MULT 0xF8
#else
    #define CCAMERA_LOD_DIST_MULT 236
    #define CCAMERA_GEN_DIST_MULT 240

    void* maincamera = nullptr; // RwCamera*
#endif

/* Realloc */
int16_t* pBlocksToBeRenderedOutsideWorldX;
int16_t* pBlocksToBeRenderedOutsideWorldY;

/* Functions Pointers */
typedef float (*RetFloatFn)();
typedef unsigned int (*RetUIntFn)();

/* Main RwCamera */
static inline void* GetMainCamera()
{
#ifdef GTASA_ARM64
    return *(void**)(pGTASA + 0xC9B378); // Scene.camera (Scene @ 0xC9B370, camera is at +8)
#else
    return maincamera;
#endif
}

/* Real screen aspect ratio. Lazy: the screen size is not known yet when the mod loads. */
static float GetRealAspectRatio()
{
    static float fCached = 0.0f;
    if(fCached > 0.0f) return fCached;

    float fRatio;
#ifdef GTASA_ARM64
    float w = (float)((RetUIntFn)(pGTASA + 0x31E0B8))(); // OS_ScreenGetWidth
    float h = (float)((RetUIntFn)(pGTASA + 0x31E0C8))(); // OS_ScreenGetHeight
    if(w < 1.0f || h < 1.0f) return 4.0f / 3.0f;         // not ready yet, try again next time
    fRatio = w / h;
#else
    fRatio = ((RetFloatFn)(pGTASA + 0x18E984))();
#endif
    if(!isfinite(fRatio) || fRatio <= 0.0f) return 4.0f / 3.0f;
    if(fRatio < 1.0f) fRatio = 1.0f / fRatio;
    return fCached = fRatio;
}

DECL_HOOK(void*, RwCameraSetNearClipPlane, void* self, float a1)
{
    if(self == GetMainCamera())
    {
        float fNear = (fNearClip > 0.0f) ? fNearClip : a1;

        // Depth buffer precision depends on far/near. With a huge far plane and a tiny near plane
        // the depth buffer runs out of precision and the textures flicker (z-fighting).
        // So the near plane is raised just enough to keep far/near under the limit.
        if(fMaxDepthRatio > 0.0f && fLastFarClip > 0.0f)
        {
            float fMinNear = fLastFarClip / fMaxDepthRatio;
            if(fNear < fMinNear) fNear = fMinNear;
        }
        a1 = fNear;
    }
    return RwCameraSetNearClipPlane(self, a1);
}

DECL_HOOK(void*, RwCameraSetFarClipPlane, void* self, float a1)
{
    if(self == GetMainCamera())
    {
        if(fDrawDistance > 200.0f) a1 = fDrawDistance;
        fLastFarClip = a1;
    }
    return RwCameraSetFarClipPlane(self, a1);
}

#ifndef GTASA_ARM64
DECL_HOOK(void*, CameraCreate, int a1, int a2, int a3)
{
    maincamera = CameraCreate(a1, a2, a3);
    return maincamera;
}
#endif

DECL_HOOK(void*, CameraProcess, uintptr_t self)
{
    float fScaler = GetRealAspectRatio() * 0.75f * fStreamingScale; // AspectRatio / 4:3

    void* ret = CameraProcess(self);
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

// SAUtils
char szSliderText[16];
void RealDrawDistanceChanged(int oldVal, int newVal)
{
    fDrawDistance = (float)newVal;
    cfg->Bind("DrawDistance", "800.0")->SetInt(newVal);
    cfg->Save();
}
const char* RealDrawDistanceDraw(int nNewValue)
{
    snprintf(szSliderText, sizeof(szSliderText), "x%.2f", (nNewValue / DEFAULT_DRAWDISTANCE));
    return szSliderText;
}
void StreamingDistanceChanged(int oldVal, int newVal)
{
    fStreamingScale = 0.01f * newVal;
    cfg->Bind("StreamingDistanceScale", "1.0")->SetFloat(fStreamingScale);
    cfg->Save();
}
const char* StreamingDistanceDraw(int nNewValue)
{
    snprintf(szSliderText, sizeof(szSliderText), "x%.2f", (nNewValue / 100.0f));
    return szSliderText;
}

#ifndef GTASA_ARM64
TARGET_THUMB ASM_NAKED void HitWaterBlock_JMP()
{
    __asm(
    ".hidden nWaterBlocksToRender\n"
    ".hidden pointerWaterBlocksToRender\n"
    ".thumb\n"
        "PUSH {R1}\n" // Backup R1
        "LDR R1, =(nWaterBlocksToRender - 100001f - 2*(100002f-100001f))\n" // Copy contents of variable to R0
        "100001:\nADD R1, PC\n100002:\n"
        "LDR R1, [R1]\n"

        "CMP R0, R1\n" // Compare R0=nWaterBlocksToRender with R1=m_NumBlocksOutsideWorldToBeRendered
        "POP {R1}\n" // Bring back R1

        "IT GT\n"
        "POPGT {R4,R5,R7,PC}\n"
        "LDR R1, =(0x6777C4 - 0x59869C)\n"

        "PUSH {R0,R1}\n"
        "LDR R0, =(pointerWaterBlocksToRender - 100001f - 2*(100002f-100001f))\n" // Copy contents of variable to R0
        "100001:\nADD R0, PC\n100002:\n"
        "LDR R0, [R0]\n"
        "STR R0, [SP, #4]\n"
        "POP {R0,PC}\n"
    );
}

void CodeRedirect(uintptr_t address, uintptr_t newAddress, bool isThumb)
{
    if(isThumb)
    {
        char code[12];
        unsigned int sizeOfData = 0;

        if (address % 4 == 0)
        {
            *(uint32_t*)(code + 0) = 0xF000F8DF;
            *(const void**)(code + 4) = (const void*)newAddress;
            sizeOfData = 8;
        }
        else
        {
            *(uint32_t*)(code + 0) = 0xBF00;
            *(uint32_t*)(code + 2) = 0xF000F8DF;
            *(const void**)(code + 6) = (const void*)newAddress;
            sizeOfData = 10;
        }
        aml->Write(address, (uintptr_t)code, sizeOfData);
        return;
    }

    char code[8];
    *(uint32_t*)(code + 0) = 0xE51FF004;
    *(const void**)(code + 4) = (const void*)newAddress;
    aml->Write(address, (uintptr_t)code, sizeof(code));
}
#endif // !GTASA_ARM64

#ifdef GTASA_ARM64
// ---- AArch64 helpers: every patch is a single 4-byte instruction ----
static inline uint32_t A64_B(uintptr_t from, uintptr_t to)
{
    return 0x14000000u | ((uint32_t)(((intptr_t)(to - from) >> 2)) & 0x03FFFFFFu);
}
// Only patches if the instruction is exactly what we expect (protects against other game versions)
static bool A64_Patch(uintptr_t offset, uint32_t expected, uint32_t value, const char* what)
{
    uintptr_t addr = pGTASA + offset;
    uint32_t cur = *(uint32_t*)addr;
    if(cur != expected)
    {
        logger->Error("%s: unexpected instruction 0x%08X at +0x%X (expected 0x%08X), skipped", what, cur, (unsigned)offset, expected);
        return false;
    }
    aml->Write(addr, (uintptr_t)&value, sizeof(value));
    return true;
}
#endif // GTASA_ARM64

extern "C" void OnModLoad()
{
    logger->SetTag("GTASA Draw Distance");
    pGTASA = aml->GetLib("libGTASA.so");
    if(pGTASA == 0)
    {
        logger->Error("libGTASA.so was not found, nothing to do");
        return;
    }

    fNearClip = cfg->Bind("NearClip", "0.1")->GetFloat();
    fDrawDistance = cfg->Bind("DrawDistance", "800.0")->GetFloat();
    fStreamingScale = cfg->Bind("StreamingDistanceScale", "1.0")->GetFloat();
    fMaxDepthRatio = cfg->Bind("MaxDepthRatio", "8192")->GetFloat(); // far/near limit (fixes the flickering), 0 = off
    nWaterBlocksToRender = (unsigned int)cfg->Bind("WaterBlocksToRender", "384")->GetInt();

    int nStreamingMemMB = cfg->Bind("PreferredStreamingMemMB", "1024")->GetInt();
    if(nStreamingMemMB > 2047)
    {
        logger->Error("PreferredStreamingMemMB is too big (max 2047)");
        nStreamingMemMB = 2047;
    }
    nStreamingMemoryOverride = (nStreamingMemMB > 0 ? nStreamingMemMB : 0) * 1024 * 1024;

#ifdef GTASA_ARM64
    /* ===== arm64-v8a (offsets for the 64-bit libGTASA.so) ===== */
    streamingMemoryAvailable = (int*)(pGTASA + 0x85EBD8);  // CStreaming::ms_memoryAvailable

    if(nWaterBlocksToRender > 1)
    {
        if(nWaterBlocksToRender > 70) // Default value is 0-69 (70) so don`t override if not necessary
        {
            // GOT slots of CWaterLevel::m_BlocksToBeRenderedOutsideWorld{X,Y}
            pBlocksToBeRenderedOutsideWorldX = new int16_t[nWaterBlocksToRender]();
            aml->Write(pGTASA + 0x84CFB0, (uintptr_t)&pBlocksToBeRenderedOutsideWorldX, sizeof(void*));

            pBlocksToBeRenderedOutsideWorldY = new int16_t[nWaterBlocksToRender]();
            aml->Write(pGTASA + 0x84F530, (uintptr_t)&pBlocksToBeRenderedOutsideWorldY, sizeof(void*));
        }
        --nWaterBlocksToRender;
        // CWaterLevel::BlockHit: `CMP W9, #69 ; B.GT` -> `CMP W9, #nWaterBlocksToRender`
        if(nWaterBlocksToRender <= 0xFFF)
        {
            A64_Patch(0x6BCECC, 0x7101153F, 0x7100013F | (nWaterBlocksToRender << 10), "WaterBlocksToRender");
        }
        else logger->Error("WaterBlocksToRender is too big (max 4096)");
    }

    HOOKPLT(RwCameraSetNearClipPlane, pGTASA + 0x841838);
    HOOKPLT(RwCameraSetFarClipPlane, pGTASA + 0x846D60);

    HOOKPLT(CameraProcess, pGTASA + 0x8429D8);

    HOOKPLT(CStreamingUpdate, pGTASA + 0x845FD0);

    // CGame::Process: only calls CPopulation::Update a 2nd time when a setting is >= 2. Make it unconditional.
    if(cfg->Bind("MoreOftenPopulationUpdate", "1")->GetBool())
    {
        A64_Patch(0x4D6450, 0x5400006B, 0xD503201F, "MoreOftenPopulationUpdate"); // B.LT -> NOP
    }

    // Do not delete vehicles behind the player camera
    // CCarCtrl::PossiblyRemoveVehicle: always use the wide removal range (skip the 45.0 "behind camera" range)
    bool bRemoveCarsBehind = cfg->Bind("DontRemoveVehicleBehindCamera", "1")->GetBool();
    if(bRemoveCarsBehind)
    {
        A64_Patch(0x3B06E4, 0x37000220, A64_B(pGTASA + 0x3B06E4, pGTASA + 0x3B0728), "DontRemoveVehicleBehindCamera");
    }

    // Do not delete peds behind the player camera
    // CPopulation::ManagePed: `CBZ W9, end` -> `B end`
    if(cfg->Bind("DontRemovePedBehindCamera", "1")->GetBool())
    {
        A64_Patch(0x5CDB80, 0x34000429, A64_B(pGTASA + 0x5CDB80, pGTASA + 0x5CDC04), "DontRemovePedBehindCamera");
    }

    // CCarCtrl::GenerateOneRandomCar: skip the "off-screen new car must be within 45m" check
    if(cfg->Bind("SpawnVehiclesInFrontOfPlayer", "1")->GetBool())
    {
        if(!bRemoveCarsBehind)
        {
            A64_Patch(0x3AD4DC, 0x7D4C32C1, A64_B(pGTASA + 0x3AD4DC, pGTASA + 0x3AD528), "SpawnVehiclesInFrontOfPlayer");
        }
    }
#else
    /* ===== armeabi-v7a (offsets for the 32-bit libGTASA.so) ===== */
    streamingMemoryAvailable = (int*)(pGTASA + 0x685FA0);

    if(nWaterBlocksToRender > 1)
    {
        if(nWaterBlocksToRender > 70) // Default value is 0-69 (70) so don`t override if not necessary
        {
            pBlocksToBeRenderedOutsideWorldX = new int16_t[nWaterBlocksToRender]();
            aml->Write(pGTASA + 0x6777C4, (uintptr_t)&pBlocksToBeRenderedOutsideWorldX, sizeof(void*));

            pBlocksToBeRenderedOutsideWorldY = new int16_t[nWaterBlocksToRender]();
            aml->Write(pGTASA + 0xA1C05C, (uintptr_t)&pBlocksToBeRenderedOutsideWorldY, sizeof(void*));
        }
        --nWaterBlocksToRender;
        pointerWaterBlocksToRender = (pGTASA + 0x598694) | 1;
        CodeRedirect(pGTASA + 0x59868C, (uintptr_t)&HitWaterBlock_JMP, true); // From FLA
    }

    HOOKPLT(RwCameraSetNearClipPlane, pGTASA + 0x670C9C);
    HOOKPLT(RwCameraSetFarClipPlane, pGTASA + 0x6740CC);

    HOOKPLT(CameraCreate, pGTASA + 0x675174);
    HOOKPLT(CameraProcess, pGTASA + 0x6717BC);

    HOOKPLT(CStreamingUpdate, pGTASA + 0x673898);

    if(cfg->Bind("MoreOftenPopulationUpdate", "1")->GetBool())
    {
        aml->Unprot(pGTASA + 0x3F40C0, sizeof(char));
        *(char*)(pGTASA + 0x3F40C0) = 2;
    }

    // Do not delete vehicles behind the player camera
    bool bRemoveCarsBehind = cfg->Bind("DontRemoveVehicleBehindCamera", "1")->GetBool();
    if(bRemoveCarsBehind)
    {
        aml->PlaceJMP(pGTASA + 0x2EC660, pGTASA + 0x2EC6D6);
    }

    // Do not delete peds behind the player camera
    if(cfg->Bind("DontRemovePedBehindCamera", "1")->GetBool())
    {
        aml->PlaceJMP(pGTASA + 0x4CE4EA, pGTASA + 0x4CE55C);
    }

    if(cfg->Bind("SpawnVehiclesInFrontOfPlayer", "1")->GetBool())
    {
        if(bRemoveCarsBehind) // They are already spawning!
        {
            //aml->Unprot(pGTASA + 0x2E866F, sizeof(char));
            //*(char*)(pGTASA + 0x2E866F) = 0xDB;
        }
        else
        {
            aml->PlaceJMP(pGTASA + 0x2E864E, pGTASA + 0x2E8670);
        }
    }
    else
    {
        if(bRemoveCarsBehind) // Huh?
        {
            aml->Unprot(pGTASA + 0x2E866F, sizeof(char));
            *(char*)(pGTASA + 0x2E866F) = 0xDB;
        }
    }
#endif // GTASA_ARM64

    sautils = (ISAUtils*)GetInterface("SAUtils");
    if(sautils != nullptr)
    {
        sautils->AddSliderItem(Display, "Real Draw Distance", (int)fDrawDistance, 200, 4000, RealDrawDistanceChanged, RealDrawDistanceDraw);
        sautils->AddSliderItem(Display, "Streaming Distance Scale", (int)(100 * fStreamingScale), 100 * 0.25f, 100 * 5.0f, StreamingDistanceChanged, StreamingDistanceDraw);
    }
}
