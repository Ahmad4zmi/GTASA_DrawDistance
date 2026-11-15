#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include "isautils.h"
ISAUtils* sautils = nullptr;

#define THUMB_ADDRESS(_address)   ((_address) | 1)

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
unsigned int nStreamingMemoryOverride;
unsigned int nWaterBlocksToRender = 0;
uintptr_t pointerWaterBlocksToRender = 0;
float fRealAspectRatio = 0.0f;
float fAspectRatioScaler = 0.0f;
float flDontGetCfgValueEveryTick = 0.0f;

/* Configs */
ConfigEntry* pNearClipOverride;
ConfigEntry* pDrawDistanceOverride;
ConfigEntry* pStreamingDistanceScale;
ConfigEntry* pHiWaterDistanceScale;

/* Lib Pointers */
void* maincamera; // RwCamera*
int* streamingMemoryAvailable;
void* gMobileMenu;
float* CameraRangeMinX;
float* CameraRangeMaxX;
float* CameraRangeMinY;
float* CameraRangeMaxY;

#ifdef GTASA_ARM64
    // CCamera::LODDistMultiplier / GenerationDistMultiplier
    #define CCAMERA_LOD_DIST_MULT 0xF4
    #define CCAMERA_GEN_DIST_MULT 0xF8
#else
    #define CCAMERA_LOD_DIST_MULT 236
    #define CCAMERA_GEN_DIST_MULT 240
#endif

/* Realloc */
int16_t* pBlocksToBeRenderedOutsideWorldX;
int16_t* pBlocksToBeRenderedOutsideWorldY;

/* Functions Pointers */
typedef float (*RetFloatFn)();

DECL_HOOK(void*, RwCameraSetNearClipPlane, void* self, float a1)
{
    if(self == maincamera && pNearClipOverride->GetFloat() > 0.0f)
    {
        return RwCameraSetNearClipPlane(self, pNearClipOverride->GetFloat());
    }
    return RwCameraSetNearClipPlane(self, a1);
}
DECL_HOOK(void*, RwCameraSetFarClipPlane, void* self, float a1)
{
    if(self == maincamera && pDrawDistanceOverride->GetInt() > 200)
        return RwCameraSetFarClipPlane(self, pDrawDistanceOverride->GetFloat());
    return RwCameraSetFarClipPlane(self, a1);
}

DECL_HOOK(void*, CameraCreate, int a1, int a2, int a3)
{
    maincamera = CameraCreate(a1, a2, a3);
    return maincamera;
}

DECL_HOOK(void*, CameraProcess, uintptr_t self)
{
    fAspectRatioScaler = fRealAspectRatio * 0.75 * pStreamingDistanceScale->GetFloat(); // AspectRatio / 4:3
    
    void* ret = CameraProcess(self);
    *(float*)(self + CCAMERA_LOD_DIST_MULT) *= fAspectRatioScaler;
    *(float*)(self + CCAMERA_GEN_DIST_MULT) *= fAspectRatioScaler;
    return ret;
}

DECL_HOOK(void*, CStreamingUpdate, void* self)
{
    if(*streamingMemoryAvailable < nStreamingMemoryOverride)
        *streamingMemoryAvailable = nStreamingMemoryOverride;
    return CStreamingUpdate(self);
}

// SAUtils
#define DEFAULT_DRAWDISTANCE 800.0f
char szRetDrawDistanceSlider[12];
void RealDrawDistanceChanged(int oldVal, int newVal)
{
    pDrawDistanceOverride->SetInt(newVal);
    cfg->Save();
}
const char* RealDrawDistanceDraw(int nNewValue)
{
    sprintf(szRetDrawDistanceSlider, "x%.2f", (nNewValue / DEFAULT_DRAWDISTANCE));
    return szRetDrawDistanceSlider;
}
void StreamingDistanceChanged(int oldVal, int newVal)
{
    pStreamingDistanceScale->SetFloat(0.01f * newVal);
    cfg->Save();
}
const char* StreamingDistanceDraw(int nNewValue)
{
    sprintf(szRetDrawDistanceSlider, "x%.2f", (nNewValue / 100.0f));
    return szRetDrawDistanceSlider;
}
void HiWaterDistanceChanged(int oldVal, int newVal)
{
    pHiWaterDistanceScale->SetFloat(0.01f * newVal);
    flDontGetCfgValueEveryTick = pHiWaterDistanceScale->GetFloat();
    cfg->Save();
}
const char* HiWaterDistanceDraw(int nNewValue)
{
    sprintf(szRetDrawDistanceSlider, "%d%%", nNewValue);
    return szRetDrawDistanceSlider;
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

DECL_HOOK(void, emu_SetWater, bool set)
{
    emu_SetWater(set);
    if(set)
    {
        //*CameraRangeMinX *= flDontGetCfgValueEveryTick;
        *CameraRangeMaxX *= flDontGetCfgValueEveryTick;
        //*CameraRangeMinY *= flDontGetCfgValueEveryTick;
        *CameraRangeMaxY *= flDontGetCfgValueEveryTick;
    }
}

extern "C" void OnModLoad()
{
    logger->SetTag("GTASA Draw Distance");
    pGTASA = aml->GetLib("libGTASA.so");

    pNearClipOverride = cfg->Bind("NearClip", "0.1");
    pDrawDistanceOverride = cfg->Bind("DrawDistance", "800.0");
    nStreamingMemoryOverride = cfg->Bind("PreferredStreamingMemMB", "1024")->GetInt() * 1024 * 1024;
    nWaterBlocksToRender = (unsigned int)cfg->Bind("WaterBlocksToRender", "384")->GetInt();
    pStreamingDistanceScale = cfg->Bind("StreamingDistanceScale", "1.0");

#ifdef GTASA_ARM64
    /* ===== arm64-v8a (offsets for the 64-bit libGTASA.so) ===== */
    fRealAspectRatio = ((RetFloatFn)(pGTASA + 0x6C9CC8))(); // CDraw::CalculateAspectRatio
    if(fRealAspectRatio < 1.0f) fRealAspectRatio = 1.0f / fRealAspectRatio;
    streamingMemoryAvailable = (int*)(pGTASA + 0x85EBD8);  // CStreaming::ms_memoryAvailable

    gMobileMenu = (void*)(pGTASA + 0x8BE780);

    if(nWaterBlocksToRender > 1)
    {
        if(nWaterBlocksToRender > 70) // Default value is 0-69 (70) so don`t override if not necessary
        {
            // GOT slots of CWaterLevel::m_BlocksToBeRenderedOutsideWorld{X,Y}
            pBlocksToBeRenderedOutsideWorldX = new int16_t[nWaterBlocksToRender];
            aml->Write(pGTASA + 0x84CFB0, (uintptr_t)&pBlocksToBeRenderedOutsideWorldX, sizeof(void*));

            pBlocksToBeRenderedOutsideWorldY = new int16_t[nWaterBlocksToRender];
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

    HOOK(CameraCreate, pGTASA + 0x55B978); // called inside the lib directly, no GOT slot -> inline hook
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
    fRealAspectRatio = ((RetFloatFn)(pGTASA + 0x18E984))();
    if(fRealAspectRatio < 1.0f) fRealAspectRatio = 1.0f / fRealAspectRatio;
    streamingMemoryAvailable = (int*)(pGTASA + 0x685FA0);

    gMobileMenu = (void*)(pGTASA + 0x6E006C);

    if(nWaterBlocksToRender > 1)
    {
        if(nWaterBlocksToRender > 70) // Default value is 0-69 (70) so don`t override if not necessary
        {
            pBlocksToBeRenderedOutsideWorldX = new int16_t[nWaterBlocksToRender];
            aml->Write(pGTASA + 0x6777C4, (uintptr_t)&pBlocksToBeRenderedOutsideWorldX, sizeof(void*));

            pBlocksToBeRenderedOutsideWorldY = new int16_t[nWaterBlocksToRender];
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

    /* Unstable + Useless */
    //pHiWaterDistanceScale = cfg->Bind("HighWaterDistanceScale", 1.0f);
    //flDontGetCfgValueEveryTick = pHiWaterDistanceScale->GetFloat();
    //CameraRangeMinX = (float*)(pGTASA + 0xA1DC8C);
    //CameraRangeMaxX = (float*)(pGTASA + 0xA1DC90);
    //CameraRangeMinY = (float*)(pGTASA + 0xA1DC94);
    //CameraRangeMaxY = (float*)(pGTASA + 0xA1DC98);
    //HOOKPLT(emu_SetWater, pGTASA + 0x673530);

#endif // GTASA_ARM64

    sautils = (ISAUtils*)GetInterface("SAUtils");
    if(sautils != nullptr)
    {
        sautils->AddSliderItem(Display, "Real Draw Distance", pDrawDistanceOverride->GetInt(), 200, 4000, RealDrawDistanceChanged, RealDrawDistanceDraw);
        sautils->AddSliderItem(Display, "Streaming Distance Scale", 100 * pStreamingDistanceScale->GetFloat(), 100 * 0.25f, 100 * 5.0f, StreamingDistanceChanged, StreamingDistanceDraw);
        //sautils->AddSliderItem(Display, "Detailed Water Draw Distance", 100 * pHiWaterDistanceScale->GetFloat(), 0, 100 * 5.0f, HiWaterDistanceChanged, HiWaterDistanceDraw);
    }
}
