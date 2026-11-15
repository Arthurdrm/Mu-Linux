// Particle_Move.cpp

#include "stdafx.h"
#include <cstdint>
#include "globals.h"
#include "functions.h"

// MoveParticles @ 0x00477090 (~1130 lines) — FULL IMPLEMENTATION
// Per-frame particle system update.
// 1. Random particle drift with clamping (not the camera's EarthQuake)
// 2. Per active particle: decrement lifetime, MovePosition,
//    switch on type for gravity/terrain snap/fade/scale/color
//
// Particle slot layout (stride 0x70, base Particles):
//   +0x00 active  +0x04 type  +0x08 sub  +0x0C scale
//   +0x10 posX    +0x14 posY  +0x18 posZ
//   +0x1C dirX    +0x20 angle
//   +0x28 lightR  +0x2C lightG +0x30 lightB
//   +0x38 lifetime +0x3C entity_ptr +0x40 rotation +0x44 frame
//   +0x48 gravity  +0x4C velX +0x50 velY +0x54 velZ
//   +0x58 extra0   +0x5C extra1 +0x60 extra2
//   +0x64 targetX  +0x68 targetY +0x6C targetZ
//
// NOTE: Ghidra shows unaff_EBX/EBP/ESI/EDI phantom params in the AngleMatrix/VectorRotate
//       calls for type 0x4c6 — these are 63 phantom stack params (anti-tamper artifact).
//       The AngleMatrix+VectorRotate path for type 0x4c6 is implemented structurally but
//       the phantom register values cannot be reconstructed; it runs the math correctly
//       with the actual particle data.
//
// Bitmaps access: DAT_083a7ca0 + type * 0x38 + 0x24 = height field per bitmap slot.
// Models access: DAT_05828d58 + entity_type * 0xBC = BMD struct base.
// WorldTime = WorldTime (global animation tick).

// Helper macros for particle field access
// Campos del slot cuyo offset en bytes es o (el recorrido avanza de a 0x70).
#define P_(o)       (Particles[(o) / PARTICLE_STRIDE])
#define P_BASE      (reinterpret_cast<char*>(Particles))
#define P_ACTIVE(o)  (P_(o).Live)
#define P_TYPE(o)    (P_(o).Type)
#define P_SUB(o)     (P_(o).SubType)
#define P_SCALE(o)   (P_(o).Scale)
#define P_POSX(o)    (P_(o).Position[0])
#define P_POSY(o)    (P_(o).Position[1])
#define P_POSZ(o)    (P_(o).Position[2])
#define P_DIRX(o)    (P_(o).Angle[0])
#define P_ANGLE(o)   (P_(o).Angle[1])
#define P_LR(o)      (P_(o).Light[0])
#define P_LG(o)      (P_(o).Light[1])
#define P_LB(o)      (P_(o).Light[2])
#define P_LIFE(o)    (P_(o).LifeTime)
#define P_ENT(o)    (g_ParticleOwner[(o) / PARTICLE_STRIDE])
#define P_ROT(o)     (P_(o).Rotation)
#define P_FRAME(o)   (P_(o).Frame)
#define P_GRAV(o)    (P_(o).Gravity)
#define P_VELX(o)    (P_(o).Velocity[0])
#define P_VELY(o)    (P_(o).Velocity[1])
#define P_VELZ(o)    (P_(o).Velocity[2])
#define P_EX0(o)     (P_(o).Extra[0])
#define P_EX1(o)     (P_(o).Extra[1])
#define P_EX2(o)     (P_(o).Extra[2])
#define P_TGX(o)     (P_(o).StartPosition[0])
#define P_TGY(o)     (P_(o).StartPosition[1])
#define P_TGZ(o)     (P_(o).StartPosition[2])
// Bitmaps height: g_BitmapsRaw + type * 0x38 + 0x24 = height field per bitmap slot
#define BMP_HEIGHT(t)  (*(float*)(g_BitmapsRaw + (t) * TEXTURE_STRIDE + 0x24))

// ── Handlers de MoveParticles (T9d) ───────────────────────────────────────────
// Cada case vive en su propia función y devuelve cómo sigue el bucle del pool:
// Break = después del switch, Continue = próxima partícula, Return = salir de
// MoveParticles, Next = el case de abajo.

enum class ParticleStep { Break, Continue, Return, Next };

// IDA: MoveParticles (0x00477090): case BITMAP_SMOKE + 1:
static ParticleStep MoveParticles_Type1221(float& fVar15, int& iVar9)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552ae4;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    P_SCALE(iVar9) += _DAT_00552a7c;
    P_POSX(iVar9) += P_VELX(iVar9);
    P_POSY(iVar9) += P_VELY(iVar9);
    P_POSZ(iVar9) += P_VELZ(iVar9);
    P_VELX(iVar9) *= _DAT_005526e8;
    P_VELY(iVar9) *= _DAT_005526e8;
    P_VELZ(iVar9) *= _DAT_005526e8;
    fVar15 = RequestTerrainHeight(P_POSX(iVar9), P_POSY(iVar9));
    P_POSZ(iVar9) = BMP_HEIGHT(P_TYPE(iVar9)) * P_SCALE(iVar9) * _DAT_00552504 + fVar15;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 0x4c6:
static ParticleStep MoveParticles_Type1222(float& fVar15, int& iVar9)
{
    if (P_LIFE(iVar9) < 10) {
        fVar15 = _DAT_0055256c - (float)(10 - P_LIFE(iVar9)) * _DAT_005524f4;
    }
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    // AngleMatrix + VectorRotate to transform local offset by entity bone
    // NOTE: Ghidra shows phantom register args (unaff_EBX/EBP/ESI/EDI).
    // We use the particle's stored direction as angles and target as local offset.
    {
        float angles3[3], mat3x4[3][4];
        // (audit #11) removed unused: float in1_3[3], in2_3[3][4], out3[3];
        angles3[0] = P_DIRX(iVar9);
        angles3[1] = 0.0f;
        angles3[2] = 0.0f;
        AngleMatrix(angles3, mat3x4);
        // VectorRotate: transform stored offset by matrix
        float localOff[3];
        localOff[0] = P_TGX(iVar9);
        localOff[1] = P_TGY(iVar9);
        localOff[2] = P_TGZ(iVar9); // placeholder
        float worldOff[3];
        Vector_Rotate(localOff, (float*)mat3x4, worldOff);
        // Add to entity position
        intptr_t entPtr = P_ENT(iVar9);
        P_POSX(iVar9) = worldOff[0] + *(float*)(entPtr + 0x10);
        P_POSY(iVar9) = worldOff[1] + *(float*)(entPtr + 0x14);
        P_POSZ(iVar9) = worldOff[2] + *(float*)(entPtr + 0x18);
    }
    P_ANGLE(iVar9) += _DAT_00552660;
    P_SCALE(iVar9) -= _DAT_005524f8;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_SHINY:
static ParticleStep MoveParticles_Shiny(float10& fVar11, int& iVar9)
{
    fVar11 = (float10)fsin((float10)P_LIFE(iVar9) * (float10)_DAT_00552ae0);
    P_SCALE(iVar9) = (float)fVar11;
    if (P_SUB(iVar9) == 1) {
        P_SCALE(iVar9) = (float)(fVar11 * (float10)_DAT_00552adc);
        P_ROT(iVar9) -= _DAT_00552664;
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_SHINY + 1:
static ParticleStep MoveParticles_Type1231(float10& fVar11, float10& fVar12, int& iVar9)
{
    if (P_SUB(iVar9) == 99) {
        intptr_t entPtr = P_ENT(iVar9);
        P_POSX(iVar9) = *(float*)(entPtr + 0x10);
        P_POSY(iVar9) = *(float*)(entPtr + 0x14);
        P_POSZ(iVar9) = *(float*)(entPtr + 0x18) + _DAT_00552598;
        P_ROT(iVar9) -= _DAT_0055256c;
    } else {
        fVar12 = (float10)fsin((float10)P_LIFE(iVar9) * (float10)_DAT_00552ad8);
        fVar11 = (float10)_DAT_00552660;
        P_SCALE(iVar9) = (float)(fVar12 * fVar11);
        if (P_SUB(iVar9) < 2) {
            ItemDrop_SetupRenderRef(reinterpret_cast<BYTE*>(P_BASE + iVar9));
        } else {
            P_SCALE(iVar9) = (float)(fVar12 * fVar11 * (float10)_DAT_00552adc);
            P_ROT(iVar9) -= _DAT_00552664;
            ItemDrop_SetupRenderRef(reinterpret_cast<BYTE*>(P_BASE + iVar9));
        }
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_SHINY + 2:
static ParticleStep MoveParticles_Type1232(float10& fVar11, int& iVar9)
{
    fVar11 = (float10)fsin((float10)P_LIFE(iVar9) * (float10)_DAT_00552ae0);
    P_SCALE(iVar9) = (float)(fVar11 * (float10)_DAT_00552540);
    ItemDrop_SetupRenderRef(reinterpret_cast<BYTE*>(P_BASE + iVar9));
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_SHINY + 4:
static ParticleStep MoveParticles_Type1234(float10& fVar11, float& fVar15, int& iVar9)
{
    if (P_SUB(iVar9) == 0) {
        fVar11 = (float10)fsin((float10)P_LIFE(iVar9) * (float10)_DAT_00552ae0);
        P_SCALE(iVar9) = (float)(fVar11 * (float10)_DAT_00552540 + (float10)_DAT_0055264c);
    }
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552b28;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_BUBBLE:
static ParticleStep MoveParticles_Bubble(int& iVar5, int& iVar9)
{
    P_FRAME(iVar9) += 1;
    iVar5 = _rand();
    P_POSX(iVar9) += (float)(iVar5 % 0x14 - 10) * P_SCALE(iVar9) * _DAT_00552950;
    iVar5 = _rand();
    P_POSY(iVar9) += (float)(iVar5 % 0x14 - 10) * P_SCALE(iVar9) * _DAT_00552950;
    iVar5 = _rand();
    P_POSZ(iVar9) += (float)(iVar5 % 0x14 + 10) * P_SCALE(iVar9) * _DAT_00552950;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_FLOWER01:, case BITMAP_FLOWER01 + 1:, case BITMAP_FLOWER01 + 2:
static ParticleStep MoveParticles_Flower01(float& fVar15, int& iVar9, unsigned int& uVar7)
{
    if (P_FRAME(iVar9) == 0) {
        uVar7 = _rand();
        uVar7 = uVar7 & 0x8000001f;
        if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xffffffe0) + 1;
        P_VELX(iVar9) += (float)(int)(uVar7 - 0x10) * _DAT_005524f4;

        uVar7 = _rand();
        uVar7 = uVar7 & 0x8000001f;
        if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xffffffe0) + 1;
        P_VELY(iVar9) += (float)(int)(uVar7 - 0x10) * _DAT_005524f4;

        uVar7 = _rand();
        uVar7 = uVar7 & 0x8000001f;
        if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xffffffe0) + 1;
        P_VELZ(iVar9) += (float)(int)(uVar7 - 0x10) * _DAT_005524f4;

        P_POSX(iVar9) += P_VELX(iVar9);
        P_POSY(iVar9) += P_VELY(iVar9);
        P_POSZ(iVar9) += P_VELZ(iVar9);
    }
    fVar15 = RequestTerrainHeight(P_POSX(iVar9), P_POSY(iVar9));
    if (P_POSZ(iVar9) < fVar15) {
        P_POSZ(iVar9) = fVar15;
        P_FRAME(iVar9) = 1;
        P_VELX(iVar9) = 0.0f;
        P_VELY(iVar9) = 0.0f;
        P_VELZ(iVar9) = 0.0f;
    }
    uVar7 = _rand();
    uVar7 = uVar7 & 0x8000000f;
    if ((int)uVar7 < 0) uVar7 = ((uVar7 - 1) | 0xfffffff0) + 1;
    P_ROT(iVar9) += (float)(int)uVar7;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_FIRECRACKER:
static ParticleStep MoveParticles_Firecracker(int& iVar9)
{
    P_VELZ(iVar9) -= _DAT_00552504;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_FLARE:
static ParticleStep MoveParticles_Flare(float10& fVar11, float10& fVar12, float& fVar15, int& iVar5, int& iVar9)
{
    iVar5 = P_SUB(iVar9);
    float lifeF = (float)P_LIFE(iVar9);
    if (iVar5 == 0 || iVar5 == 3) {
        fVar11 = ((float10)(int)P_LIFE(iVar9) + (float10)P_VELX(iVar9)) * (float10)_DAT_005524f4;
        fVar12 = (float10)fsin((double)fVar11);
        P_POSX(iVar9) = (float)(fVar12 * (float10)_DAT_005528e4 + (float10)P_TGX(iVar9));
        fVar11 = (float10)fcos((double)fVar11);
        P_POSY(iVar9) = (float)((float10)P_TGY(iVar9) - fVar11 * (float10)_DAT_005528e4);
        fVar15 = P_GRAV(iVar9);
        P_POSZ(iVar9) += fVar15;
        P_SCALE(iVar9) -= _DAT_005528e0;
    } else if (iVar5 == 2) {
        P_POSX(iVar9) = P_TGX(iVar9);
        P_POSY(iVar9) = P_TGY(iVar9);
        float steps = (float)((0x3c - P_LIFE(iVar9)) / 10);
        fVar15 = steps * P_GRAV(iVar9);
        P_POSZ(iVar9) += fVar15;
        P_SCALE(iVar9) -= _DAT_005528e0;
    }

    if (iVar5 == 4) {
        fVar11 = ((float10)(int)lifeF + (float10)P_VELX(iVar9)) * (float10)_DAT_005524f4;
        fVar12 = (float10)fsin((double)fVar11);
        P_POSX(iVar9) = (float)(fVar12 * (float10)_DAT_005528e4 + (float10)P_TGX(iVar9));
        fVar11 = (float10)fcos((double)fVar11);
        P_POSY(iVar9) = (float)((float10)P_TGY(iVar9) - fVar11 * (float10)_DAT_005528e4);
        P_POSZ(iVar9) += P_GRAV(iVar9);
        P_SCALE(iVar9) -= _DAT_005524bc;
        if ((int)lifeF < 0x1f) {
            goto label_color_decay;
        }
    } else if ((int)lifeF < 0x15) {
        label_color_decay:
        P_LR(iVar9) *= _DAT_00552ae8;
        P_LG(iVar9) *= _DAT_00552ae8;
        P_LB(iVar9) *= _DAT_00552ae8;
    }

    if (iVar5 == 0) {
        unsigned char animState = AsObject(P_ENT(iVar9))->CurrentAction;
        if ((animState > 0xc && animState < 0x22) || (animState > 0x37 && animState < 0x3d)) {
            P_SUB(iVar9) = 1;
            if ((int)lifeF > 0x14) {
                lifeF = 20.0f;
            }
            P_LIFE(iVar9) = (int)lifeF;
            P_VELX(iVar9) = 0.0f;
            P_VELY(iVar9) = 0.0f;
            P_VELZ(iVar9) = 0.0f;
            P_TGY(iVar9) = P_POSY(iVar9);
            P_TGX(iVar9) = P_POSX(iVar9);
            P_TGZ(iVar9) = P_POSZ(iVar9);
        }
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_CLOUD:
static ParticleStep MoveParticles_Cloud(float10& fVar11, float& fVar15, int& iVar9, unsigned int& uVar7)
{
    if (P_SUB(iVar9) == 6) {
        P_POSX(iVar9) += P_VELX(iVar9);
        P_POSY(iVar9) += P_VELY(iVar9);
        P_POSZ(iVar9) += P_VELZ(iVar9);
        P_VELX(iVar9) *= _DAT_00552b44;
        P_VELY(iVar9) *= _DAT_00552b44;
        P_VELZ(iVar9) += _DAT_00552534;

        uVar7 = _rand(); uVar7 &= 0x80000003;
        if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
        P_POSX(iVar9) += (float)(int)(uVar7 - 2);

        uVar7 = _rand(); uVar7 &= 0x80000003;
        if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
        P_POSY(iVar9) += (float)(int)(uVar7 - 2);

        uVar7 = _rand(); uVar7 &= 0x80000003;
        if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
        P_POSZ(iVar9) += (float)(int)(uVar7 - 2) * _DAT_00552530;

        P_SCALE(iVar9) += _DAT_00552874;
        fVar15 = (float)P_LIFE(iVar9) * _DAT_00552944;
        P_LR(iVar9) = fVar15;
        P_LG(iVar9) = fVar15;
        P_LB(iVar9) = fVar15;

        uVar7 = _rand(); uVar7 &= 0x80000001;
        if ((int)uVar7 < 0) uVar7 = ((uVar7 - 1) | 0xfffffffe) + 1;
        P_ROT(iVar9) += (float)(int)uVar7 + _DAT_0055256c;
    } else {
        // Non-sub6: check entity death flag
        if (*(char*)&AsObject(P_ENT(iVar9))->Visible == '\x01') {
            P_LIFE(iVar9) = 0x32;
        }
        if (P_LIFE(iVar9) < 1) {
            *(int*)(P_ENT(iVar9) + 0x58) = 0;
            P_ACTIVE(iVar9) = '\0';
        }
        fVar11 = (float10)fsin(((float10)WorldTime + (float10)P_GRAV(iVar9)) * (float10)_DAT_00552b0c);
        P_POSZ(iVar9) = (float)(fVar11 * (float10)_DAT_005524fc + (float10)P_TGZ(iVar9));
    }
    // Sub-type rotation
    switch (P_SUB(iVar9)) {
    case 1:
    case 4:
        P_ROT(iVar9) = (float)WorldTime * P_EX0(iVar9) * _DAT_00552914 + P_TGY(iVar9);
        break;
    case 2:
    case 5:
        P_ROT(iVar9) = (float)WorldTime * P_EX0(iVar9) * _DAT_00552acc + P_TGY(iVar9);
        break;
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_ADV_SMOKE:
static ParticleStep MoveParticles_AdvSmoke(float& fVar15, int& iVar9)
{
    P_POSX(iVar9) += P_VELX(iVar9);
    P_POSY(iVar9) += P_VELY(iVar9);
    P_POSZ(iVar9) += P_VELZ(iVar9);
    P_VELX(iVar9) *= _DAT_00552b44;
    P_VELY(iVar9) *= _DAT_00552b44;
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552a00;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    P_VELZ(iVar9) += _DAT_005528b8;
    if (P_SUB(iVar9) == 0) {
        P_SCALE(iVar9) += _DAT_00552ac8;
    } else {
        P_SCALE(iVar9) += _DAT_00552ac4;
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_ADV_SMOKE + 1:
static ParticleStep MoveParticles_Type1382(float& fVar15, int& iVar9, unsigned int& uVar7)
{
    P_POSX(iVar9) += P_VELX(iVar9);
    P_POSY(iVar9) += P_VELY(iVar9);
    P_POSZ(iVar9) += P_VELZ(iVar9);
    P_VELX(iVar9) *= _DAT_00552b44;
    P_VELY(iVar9) *= _DAT_00552b44;
    P_VELZ(iVar9) += _DAT_00552534;

    uVar7 = _rand(); uVar7 &= 0x80000003;
    if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
    P_POSX(iVar9) += (float)(int)(uVar7 - 2);

    uVar7 = _rand(); uVar7 &= 0x80000003;
    if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
    P_POSY(iVar9) += (float)(int)(uVar7 - 2);

    uVar7 = _rand(); uVar7 &= 0x80000003;
    if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
    P_POSZ(iVar9) += (float)(int)(uVar7 - 2) * _DAT_00552530;

    P_SCALE(iVar9) += _DAT_00552874;
    fVar15 = (float)P_LIFE(iVar9) * _DAT_0055295c;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;

    uVar7 = _rand(); uVar7 &= 0x80000001;
    if ((int)uVar7 < 0) uVar7 = ((uVar7 - 1) | 0xfffffffe) + 1;
    P_ROT(iVar9) += (float)(int)uVar7 + _DAT_0055256c;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_POUNDING_BALL:
static ParticleStep MoveParticles_PoundingBall(float& fVar15, int& iVar9)
{
    fVar15 = P_GRAV(iVar9) + _DAT_005524bc;
    P_GRAV(iVar9) = fVar15;
    P_SCALE(iVar9) -= _DAT_00552914;
    fVar15 = fVar15 * _DAT_00552488;
    P_FRAME(iVar9) = (0x17 - P_LIFE(iVar9)) / 6;
    P_POSZ(iVar9) += fVar15;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 0x596:
static ParticleStep MoveParticles_Type1430(int& iVar9)
{
    P_FRAME(iVar9) = P_SUB(iVar9);
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 0x598:
static ParticleStep MoveParticles_Type1432(float& fVar15, int& iVar9)
{
    float lifeF2 = (float)P_LIFE(iVar9);
    P_GRAV(iVar9) = (float)(5 - P_LIFE(iVar9)) * _DAT_00552ad4;
    fVar15 = lifeF2 * _DAT_005528dc;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 0x599:
static ParticleStep MoveParticles_Type1433(float10& fVar12, float& fVar15, int& iVar9)
{
    float grav = P_GRAV(iVar9);
    P_GRAV(iVar9) = grav - _DAT_005526e4;
    fVar12 = (float10)fsin((float10)P_LIFE(iVar9) * (float10)_DAT_00552ad0);
    P_SCALE(iVar9) = (float)(fVar12 + (float10)(grav - _DAT_005526e4) + (float10)_DAT_0055256c);
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552a30;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_LIGHTNING + 1:
static ParticleStep MoveParticles_Type1191(int& iVar9)
{
    // Terrain light + positioning
    unsigned long long uv = (unsigned long long)(long long)(double)((float10)WorldTime * (float10)_DAT_00552500);
    P_ROT(iVar9) = (float)(int)((long long)(uv) % 1000) * _DAT_00552500;
    float lightA = (float)P_LIFE(iVar9) * _DAT_00552a00;
    float ltA0 = lightA * _DAT_00552504;
    float ltA1 = lightA;
    float ltA2 = lightA * _DAT_00552530;
    {
        float ltBuf[3] = { ltA0, ltA1, ltA2 };
        AddTerrainLight(P_POSX(iVar9), P_POSY(iVar9), (float*)ltBuf, 3, (float*)&DAT_081cb608[0]);
    }
    if (P_SUB(iVar9) == 2) {
        intptr_t entPtr = P_ENT(iVar9);
        P_LR(iVar9) = ltA0;
        P_LG(iVar9) = ltA1;
        P_SCALE(iVar9) += _DAT_005524f4;
        P_LB(iVar9) = ltA2;
        P_POSX(iVar9) = *(float*)(entPtr + 0x10);
        P_POSY(iVar9) = *(float*)(entPtr + 0x14);
        P_POSZ(iVar9) = *(float*)(entPtr + 0x18) + _DAT_00552878;
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_FIRE:, case BITMAP_FIRE + 2:
static ParticleStep MoveParticles_Fire(float& fVar15, int& iVar5, int& iVar9)
{
    float grav2 = P_GRAV(iVar9) + _DAT_005524bc;
    P_GRAV(iVar9) = grav2;
    switch (P_SUB(iVar9)) {
    case 5:
    case 6:
        P_SCALE(iVar9) -= _DAT_00552a0c;
        P_ROT(iVar9) += _DAT_00552660;
        break;
    case 7:
        P_FRAME(iVar9) = (0xf - P_LIFE(iVar9)) / 6;
        // fall through
    case 0:
        P_SCALE(iVar9) -= _DAT_00552a0c;
        break;
    case 8:
        P_SCALE(iVar9) *= _DAT_00552b44;
        P_ROT(iVar9) += _DAT_00552660;
        break;
    case 9: {
        intptr_t entPtr = P_ENT(iVar9);
        P_POSX(iVar9) = *(float*)(entPtr + 0x10) + P_TGX(iVar9);
        P_POSY(iVar9) = *(float*)(entPtr + 0x14) + P_TGY(iVar9);
        P_POSZ(iVar9) = *(float*)(entPtr + 0x18) + P_TGZ(iVar9);
        iVar5 = _rand();
        float rndG = (float)((iVar5 % 0x3c + 0x3c) / 100);
        fVar15 = P_GRAV(iVar9);
        P_GRAV(iVar9) = rndG + fVar15;
        P_SCALE(iVar9) -= (rndG + fVar15) * _DAT_00552a60;
        break;
    }
    case 10:
        P_SCALE(iVar9) *= _DAT_00552b44;
        break;
    default:
        P_SCALE(iVar9) = grav2 + P_SCALE(iVar9);
        P_VELX(iVar9) *= _DAT_00552b2c;
        P_VELY(iVar9) *= _DAT_00552b2c;
        P_VELZ(iVar9) *= _DAT_00552b2c;
        break;
    }
    P_POSZ(iVar9) += P_GRAV(iVar9) * _DAT_00552488;
    P_FRAME(iVar9) = (0x17 - P_LIFE(iVar9)) / 6;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_FIRE + 1:
static ParticleStep MoveParticles_Type1196(float& fVar15, int& iVar9)
{
    fVar15 = P_GRAV(iVar9) + _DAT_00552914;
    P_GRAV(iVar9) = fVar15;
    P_SCALE(iVar9) += fVar15;
    P_VELX(iVar9) *= _DAT_00552b24;
    P_VELY(iVar9) *= _DAT_00552b24;
    P_VELZ(iVar9) *= _DAT_00552b24;
    P_POSZ(iVar9) += fVar15 * _DAT_005524fc;
    fVar15 = (float)P_LIFE(iVar9) * _DAT_005526e4;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_EXPLOTION:
static ParticleStep MoveParticles_Explotion(int& iVar9)
{
    P_FRAME(iVar9) = (0x14 - P_LIFE(iVar9)) / 2;
    if (P_SUB(iVar9) != 1) {
        float ltBF = (float)P_LIFE(iVar9) * _DAT_00552a10;
        float lt4[3] = { ltBF * _DAT_00552504, ltBF * _DAT_005528b8, ltBF * _DAT_005524f4 };
        AddTerrainLight(P_POSX(iVar9), P_POSY(iVar9), (float*)lt4, 4, (float*)&DAT_081cb608[0]);
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case BITMAP_EXPLOTION + 1:
static ParticleStep MoveParticles_Type1216(int& iVar9)
{
    P_FRAME(iVar9) = (0xc - P_LIFE(iVar9)) / 3;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 1:
static ParticleStep MoveParticles_FireSub1(float& fVar15, int& iVar9)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552944;
    P_LR(iVar9) = fVar15 * _DAT_00552504;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15 * _DAT_00552530;
    P_VELX(iVar9) *= _DAT_005528b4;
    P_VELY(iVar9) *= _DAT_005528b4;
    P_VELZ(iVar9) *= _DAT_005528b4;
    P_SCALE(iVar9) += _DAT_00552874;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 2:
static ParticleStep MoveParticles_FireSub2(float& fVar15, int& iVar9)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552944;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    fVar15 = P_GRAV(iVar9) - _DAT_005524f4;
    P_GRAV(iVar9) = fVar15;
    P_POSX(iVar9) -= fVar15 * _DAT_005526e4;
    P_POSZ(iVar9) += fVar15;
    P_SCALE(iVar9) -= _DAT_005524f8;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 3:
static ParticleStep MoveParticles_FireSub3(float& fVar15, float& fVar3, int& iVar9)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552b1c;
    fVar3 = fVar15 * _DAT_00552530;
    P_LR(iVar9) = fVar3;
    P_LG(iVar9) = fVar3;
    P_LB(iVar9) = fVar15;
    P_VELX(iVar9) *= _DAT_005528b4;
    P_VELY(iVar9) *= _DAT_005528b4;
    P_VELZ(iVar9) *= _DAT_005528b4;
    P_SCALE(iVar9) += _DAT_005524f4;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 5:
static ParticleStep MoveParticles_FireSub5(float& fVar15, int& iVar9, unsigned int& uVar7)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552a00;
    P_LR(iVar9) = fVar15;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    P_GRAV(iVar9) -= _DAT_005524f4;
    uVar7 = _rand(); uVar7 &= 0x80000001;
    if ((int)uVar7 < 0) uVar7 = ((uVar7 - 1) | 0xfffffffe) + 1;
    P_POSX(iVar9) -= (float)(int)(uVar7 + 1) * _DAT_005526e4 * P_GRAV(iVar9);
    P_POSZ(iVar9) += P_GRAV(iVar9);
    P_SCALE(iVar9) -= _DAT_005524f8;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 6:
static ParticleStep MoveParticles_FireSub6(float10& fVar11, int& iVar9)
{
    fVar11 = (float10)WorldTime;
    P_LIFE(iVar9) = 10;
    fVar11 = (float10)fsin((double)((fVar11 + (float10)P_GRAV(iVar9)) * (float10)_DAT_00552b0c));
    P_POSZ(iVar9) = (float)(fVar11 * (float10)_DAT_005524fc + (float10)P_ROT(iVar9));
    {
        unsigned long long uv = (unsigned long long)(long long)(double)((float10)WorldTime * (float10)_DAT_00552500);
        int modVal = (int)((long long)uv % 0x708);
        P_LR(iVar9) = 0.36f;  // 0x3eb851ec
        P_LG(iVar9) = 0.3f;   // 0x3e99999a
        P_LB(iVar9) = 0.24f;  // 0x3e75c290
        fVar11 = (float10)fsin((float10)modVal * (float10)_DAT_00552b08);
        P_SCALE(iVar9) = (float)(fVar11 * (float10)_DAT_00552504 + (float10)_DAT_00552a18);
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 7:
static ParticleStep MoveParticles_FireSub7(float& fVar15, float& fVar3, int& iVar9)
{
    P_SCALE(iVar9) += _DAT_00552910;
    float lifeF3 = (float)P_LIFE(iVar9);
    fVar3 = P_GRAV(iVar9) + _DAT_0055256c;
    P_GRAV(iVar9) = fVar3;
    P_VELY(iVar9) -= _DAT_005524f4;
    P_POSZ(iVar9) -= fVar3;
    if (P_LIFE(iVar9) < 5) {
        fVar15 = lifeF3 * _DAT_00552b1c;
        P_SCALE(iVar9) -= _DAT_005524f4;
    }
    P_LR(iVar9) = fVar15 * _DAT_00552b04;
    P_LG(iVar9) = fVar15 * _DAT_00552b00;
    P_LB(iVar9) = fVar15 * _DAT_00552afc;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 8:
static ParticleStep MoveParticles_FireSub8(float& fVar15, int& iVar9)
{
    fVar15 = P_GRAV(iVar9) + _DAT_00552914;
    float lifeF4 = (float)P_LIFE(iVar9);
    P_GRAV(iVar9) = fVar15;
    if (P_LIFE(iVar9) < 6) {
        fVar15 = P_LR(iVar9) * _DAT_00552504;
        P_VELX(iVar9) = 0.0f;
        P_VELY(iVar9) = 0.0f;
        P_VELZ(iVar9) = 0.0f;
        P_LR(iVar9) = fVar15;
        P_LG(iVar9) *= _DAT_00552504;
        P_LB(iVar9) *= _DAT_00552504;
    } else {
        P_SCALE(iVar9) += fVar15;
        P_POSZ(iVar9) += fVar15 * _DAT_005524fc;
        fVar15 = lifeF4 * _DAT_00552af8;
        P_LR(iVar9) = fVar15;
        P_LG(iVar9) = fVar15;
        P_LB(iVar9) = fVar15;
        P_VELX(iVar9) *= _DAT_00552af4;
        P_VELY(iVar9) *= _DAT_00552af4;
        P_VELZ(iVar9) *= _DAT_00552af4;
    }
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 9:
static ParticleStep MoveParticles_FireSub9(float& fVar15, int& iVar9)
{
    P_LB(iVar9) = 0.0f;
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552a00;
    P_LR(iVar9) = fVar15 * _DAT_00552af0;
    P_LG(iVar9) = fVar15 * _DAT_00552aec;
    fVar15 = P_GRAV(iVar9) + _DAT_00552504;
    P_GRAV(iVar9) = fVar15;
    P_POSZ(iVar9) += fVar15;
    P_SCALE(iVar9) += _DAT_005524f4;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 10:
static ParticleStep MoveParticles_FireSub10(float& fVar15, int& iVar9)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_005526dc;
    P_LR(iVar9) = fVar15;
    fVar15 = fVar15 * _DAT_005524f4;
    P_LG(iVar9) = fVar15;
    P_LB(iVar9) = fVar15;
    P_SCALE(iVar9) += _DAT_00552874;
    return ParticleStep::Break;
}

// IDA: MoveParticles (0x00477090): case 0xb:
static ParticleStep MoveParticles_FireSub11(float& fVar15, int& iVar9)
{
    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552944;
    P_LR(iVar9) = fVar15 * P_EX0(iVar9);
    P_LG(iVar9) = fVar15 * P_EX1(iVar9);
    P_LB(iVar9) = fVar15 * P_EX2(iVar9);
    P_VELX(iVar9) *= _DAT_005528b4;
    P_VELY(iVar9) *= _DAT_005528b4;
    P_VELZ(iVar9) *= _DAT_005528b4;
    P_SCALE(iVar9) += _DAT_00552874;
    P_POSZ(iVar9) -= _DAT_0055256c;
    return ParticleStep::Break;
}

void __stdcall MoveParticles(void)
{
    float fVar15, fVar19, fVar3;
    float10 fVar11, fVar12;
    int iVar4, iVar5, iVar6, iVar9;
    unsigned int uVar7;

    // ── 1. Particle drift: random perturbation + clamping ───────────────────
    // Velocity array: DAT_07c80104 (X), DAT_07c80108 (Y) — two floats
    float* shakeVel = g_ParticleDriftVelocity;
    for (int s = 0; s < 2; s++) {
        iVar4 = _rand();
        fVar19 = (float)(iVar4 % 0x7d1 - 1000) * _DAT_00552b54 + shakeVel[s];
        fVar15 = fVar19;
        if (fVar19 < _DAT_00552534 && fVar19 >= _DAT_00552b50) {
            // nop — keep fVar15 = fVar19
        } else {
            fVar15 = _DAT_00552534;
        }
        if (fVar19 < _DAT_00552b50) {
            fVar15 = _DAT_00552b50;
        }
        shakeVel[s] = fVar15;
    }

    // Accumulate shake velocity into position, clamp
    // DAT_07c800f8 (X), DAT_07c800fc (Y) — two floats
    float* shakePos = g_ParticleDriftPosition;
    for (int s = 0; s < 2; s++) {
        fVar19 = shakeVel[s] + shakePos[s];
        shakePos[s] = fVar19;
        fVar15 = fVar19;
        if (fVar19 < _DAT_00552b4c && fVar19 >= _DAT_00552b48) {
            // nop
        } else {
            fVar15 = _DAT_00552b4c;
        }
        if (fVar19 < _DAT_00552b48) {
            fVar15 = _DAT_00552b48;
        }
        shakePos[s] = fVar15;
    }

    // ── 2. Per-particle update loop ──────────────────────────────────────────
    for (iVar4 = 0; iVar4 < kParticlePoolSize; iVar4++) {
        iVar9 = iVar4 * 0x70;

        if (P_ACTIVE(iVar9) == '\0') continue;

        // Decrement lifetime
        iVar5 = P_LIFE(iVar9);
        P_LIFE(iVar9) = iVar5 - 1;
        if (iVar5 - 1 < 1) {
            P_ACTIVE(iVar9) = '\0';
        }

        // MovePosition(pos, dir, vel) — advances position along velocity rotated by direction
        float* pPos = &P_POSX(iVar9);
        float* pDir = &P_DIRX(iVar9);
        float* pVel = &P_VELX(iVar9);
        Vector_AddRotated(pPos, pDir, pVel);

        fVar15 = _DAT_0055256c;
        iVar5 = P_TYPE(iVar9);

        // ── Main type switch ─────────────────────────────────────────────────
        // The original binary has a complex nested switch. We replicate the
        // structure exactly, including fall-through and goto patterns.

        // IDA 0.98j MoveParticles (sub_484310): tipos 103/104, el polvo que
        // levanta el Tamachan al caminar (no existen en el 0.97k).
        //     if (life <= 0) flag = 0;
        //     Scale = SubType == 1 ? Scale + (+72) : Scale + 0.03;
        //     Light[0..2] -= 0.05;
        if (iVar5 == 103 || iVar5 == 104) {
            if (P_LIFE(iVar9) <= 0) P_ACTIVE(iVar9) = 0;
            if (P_SUB(iVar9) == 1) P_SCALE(iVar9) = P_GRAV(iVar9) + P_SCALE(iVar9);
            else                   P_SCALE(iVar9) = P_SCALE(iVar9) + 0.03f;
            P_LR(iVar9) -= 0.05f;
            P_LG(iVar9) -= 0.05f;
            P_LB(iVar9) -= 0.05f;
            continue;
        }

        if (iVar5 > 0x4c4) {
            // High type range: 0x4c5..0x599
            switch (iVar5) {

            case BITMAP_SMOKE + 1:
                {
                    const ParticleStep step = MoveParticles_Type1221(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 0x4c6:
                {
                    const ParticleStep step = MoveParticles_Type1222(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_SHINY:
                {
                    const ParticleStep step = MoveParticles_Shiny(fVar11, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_SHINY + 1:
                {
                    const ParticleStep step = MoveParticles_Type1231(fVar11, fVar12, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_SHINY + 2:
                {
                    const ParticleStep step = MoveParticles_Type1232(fVar11, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_SHINY + 4:
                {
                    const ParticleStep step = MoveParticles_Type1234(fVar11, fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_BUBBLE:
                {
                    const ParticleStep step = MoveParticles_Bubble(iVar5, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_FLOWER01:
            case BITMAP_FLOWER01 + 1:
            case BITMAP_FLOWER01 + 2:
                {
                    const ParticleStep step = MoveParticles_Flower01(fVar15, iVar9, uVar7);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_FIRECRACKER:
                {
                    const ParticleStep step = MoveParticles_Firecracker(iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_FLARE:
                {
                    const ParticleStep step = MoveParticles_Flare(fVar11, fVar12, fVar15, iVar5, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_CLOUD:
                {
                    const ParticleStep step = MoveParticles_Cloud(fVar11, fVar15, iVar9, uVar7);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_ADV_SMOKE:
                {
                    const ParticleStep step = MoveParticles_AdvSmoke(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_ADV_SMOKE + 1:
                {
                    const ParticleStep step = MoveParticles_Type1382(fVar15, iVar9, uVar7);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case BITMAP_POUNDING_BALL:
                {
                    const ParticleStep step = MoveParticles_PoundingBall(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 0x596:
                {
                    const ParticleStep step = MoveParticles_Type1430(iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 0x598:
                {
                    const ParticleStep step = MoveParticles_Type1432(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 0x599:
                {
                    const ParticleStep step = MoveParticles_Type1433(fVar12, fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            default:
                break;
            } // end high-type switch
            continue;
        } // end if iVar5 > 0x4c4

        if (iVar5 == 0x4c4) {
            // ── Type 0x4c4: multi-sub particle (flame/fire) ──────────────────
            switch (P_SUB(iVar9)) {
            case 0: {
                fVar15 = (float)P_LIFE(iVar9) * _DAT_00552b1c;
                P_LR(iVar9) = fVar15;
                P_LG(iVar9) = fVar15;
                P_LB(iVar9) = fVar15;
                fVar15 = P_GRAV(iVar9) + _DAT_005526e4;
                P_GRAV(iVar9) = fVar15;
                P_POSZ(iVar9) += fVar15;
                P_SCALE(iVar9) += _DAT_00552874;
                break;
            }
            case 1:
                {
                    const ParticleStep step = MoveParticles_FireSub1(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 2:
                {
                    const ParticleStep step = MoveParticles_FireSub2(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 3:
                {
                    const ParticleStep step = MoveParticles_FireSub3(fVar15, fVar3, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 4: {
                fVar15 = (float)P_LIFE(iVar9) * _DAT_00552b1c;
                P_LR(iVar9) = fVar15 * _DAT_00552b18;
                P_LG(iVar9) = fVar15 * _DAT_00552b14;
                fVar15 = fVar15 * _DAT_00552b10;
                P_LB(iVar9) = fVar15;
                fVar15 = P_GRAV(iVar9) + _DAT_005526e4;
                P_GRAV(iVar9) = fVar15;
                P_POSZ(iVar9) += fVar15;
                P_SCALE(iVar9) += _DAT_00552874;
                break;
            }
            case 5:
                {
                    const ParticleStep step = MoveParticles_FireSub5(fVar15, iVar9, uVar7);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 6:
                {
                    const ParticleStep step = MoveParticles_FireSub6(fVar11, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 7:
                {
                    const ParticleStep step = MoveParticles_FireSub7(fVar15, fVar3, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 8:
                {
                    const ParticleStep step = MoveParticles_FireSub8(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 9:
                {
                    const ParticleStep step = MoveParticles_FireSub9(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 10:
                {
                    const ParticleStep step = MoveParticles_FireSub10(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            case 0xb:
                {
                    const ParticleStep step = MoveParticles_FireSub11(fVar15, iVar9);
                    if (step == ParticleStep::Break) break;
                }
                break;
            } // end 0x4c4 sub switch
            continue;
        } // end iVar5 == 0x4c4

        // ── Low type range: 0x67..0x4c3 ──────────────────────────────────────
        if (iVar5 < 0x4a7) {
            if (iVar5 == 0x4a6) {
                // Type 0x4a6: terrain light particle
                uVar7 = _rand(); uVar7 &= 0x80000003;
                if ((int)uVar7 < 0) uVar7 = (uVar7 - 1 | 0xfffffffc) + 1;
                P_FRAME(iVar9) = (int)uVar7;
                float lightF = (float)P_LIFE(iVar9) * _DAT_00552b28;
                P_LR(iVar9) = lightF;
                P_LG(iVar9) = lightF;
                P_LB(iVar9) = lightF;
                // AddTerrainLight — first call
                {
                    float lt0[3];
                    lt0[0] = lightF;  // placeholder
                    lt0[1] = lightF;
                    lt0[2] = lightF;
                    AddTerrainLight(P_POSX(iVar9), P_POSY(iVar9), (float*)lt0, 6, (float*)&DAT_081cb608[0]);
                }
                // AddTerrainLight — second call with color modulation
                {
                    float lt1[3];
                    lt1[0] = lightF * _DAT_005526e4;
                    lt1[1] = lightF * _DAT_005528b4;
                    lt1[2] = lightF;
                    AddTerrainLight(P_POSX(iVar9), P_POSY(iVar9), (float*)lt1, 4, (float*)&DAT_081cb608[0]);
                }
                continue;
            }

            if (iVar5 > 0x497) {
                if (iVar5 == 0x498) {
                    // Type 0x498: multi-sub particle
                    switch (P_SUB(iVar9)) {
                    case 0:
                        fVar15 = P_SCALE(iVar9) - _DAT_00552504;
                        P_SCALE(iVar9) = fVar15;
                        if (fVar15 < _DAT_005526e4) P_ACTIVE(iVar9) = '\0';
                        break;
                    case 1:
                        fVar15 = P_SCALE(iVar9) - _DAT_0055264c;
                        P_SCALE(iVar9) = fVar15;
                        if (fVar15 < _DAT_005526e4) P_ACTIVE(iVar9) = '\0';
                        break;
                    case 2:
                        if (P_LIFE(iVar9) < 1) P_ACTIVE(iVar9) = '\0';
                        if (P_LIFE(iVar9) > 5 &&
                            *reinterpret_cast<const char*>(P_ENT(iVar9)) == '\0') {
                            // El owner conserva su campo de 32 bits; no depende de la base del pool.
                            P_LIFE(iVar9) = 5;
                        }
                        {
                            float lt2 = (float)P_LIFE(iVar9) * _DAT_00552b28;
                            P_LR(iVar9) = lt2;
                            P_LG(iVar9) = lt2;
                            P_LB(iVar9) = lt2;
                        }
                        P_SCALE(iVar9) += _DAT_00552874;
                        P_POSX(iVar9) = P_TGX(iVar9) + P_VELX(iVar9);
                        {
                            float newY = P_VELY(iVar9) + P_TGY(iVar9);
                            P_POSY(iVar9) = newY;
                            float newZ = P_VELZ(iVar9) + P_TGZ(iVar9);
                            P_TGY(iVar9) = newY;
                            P_POSZ(iVar9) = newZ;
                            P_TGX(iVar9) += P_VELX(iVar9);
                            P_TGZ(iVar9) = newZ;
                        }
                        break;
                    case 3:
                        P_SCALE(iVar9) *= _DAT_00552530;
                        break;
                    }
                } else {
                    if (iVar5 == 0x499) {
                        // Mismo frenado que BITMAP_BLOOD (antes un goto a ese case).
                        fVar15 = P_VELX(iVar9) * _DAT_00552b44;
                        P_VELX(iVar9) = fVar15;
                        float vy = P_VELY(iVar9) * _DAT_00552b44;
                        P_VELY(iVar9) = vy;
                        float vz = P_VELZ(iVar9) * _DAT_00552b44;
                        P_VELZ(iVar9) = vz;
                        P_FRAME(iVar9) = (0xc - P_LIFE(iVar9)) / 3;
                        P_POSX(iVar9) += fVar15;
                        P_POSY(iVar9) += vy;
                        P_POSZ(iVar9) += vz;
                        continue;
                    }
                    if (iVar5 == 0x49c) {
                        P_ROT(iVar9) += _DAT_005524fc;
                    }
                }
                continue;
            }

            if (iVar5 == 0x497) {
                // Type 0x497: bounce + glow decay
                fVar15 = (float)P_LIFE(iVar9) * _DAT_005526dc;
                P_LR(iVar9) = fVar15;
                P_LG(iVar9) = fVar15;
                P_LB(iVar9) = fVar15;
                P_POSZ(iVar9) += P_GRAV(iVar9);
                P_GRAV(iVar9) -= _DAT_0055264c;
                fVar15 = RequestTerrainHeight(P_POSX(iVar9), P_POSY(iVar9));
                if (P_POSZ(iVar9) < fVar15) {
                    P_POSZ(iVar9) = fVar15;
                    fVar15 = P_GRAV(iVar9) * _DAT_00552b50;
                    P_LIFE(iVar9) -= 4;
                    P_GRAV(iVar9) = fVar15;
                }
                P_POSX(iVar9) += P_VELX(iVar9);
                P_POSY(iVar9) += P_VELY(iVar9);
                P_POSZ(iVar9) += P_VELZ(iVar9);
                continue;
            }

            // Types < 0x497
            if (iVar5 > 0x47f) {
                if (iVar5 == 0x490) {
                    fVar15 = P_GRAV(iVar9) + _DAT_005524f8;
                    P_GRAV(iVar9) = fVar15;
                    P_SCALE(iVar9) += fVar15;
                    fVar15 = (float)P_LIFE(iVar9) * _DAT_00552a00;
                    P_LR(iVar9) = fVar15;
                    P_LG(iVar9) = fVar15;
                    P_LB(iVar9) = fVar15;
                }
                continue;
            }

            if (iVar5 == 0x47f) {
                P_POSZ(iVar9) -= P_GRAV(iVar9);
                P_POSY(iVar9) -= _DAT_0055256c;
                P_GRAV(iVar9) += _DAT_0055256c;
                P_SCALE(iVar9) *= _DAT_00552b44;
                continue;
            }

            if (iVar5 < 0x67) continue;

            if (iVar5 > 0x68) {
                if (iVar5 == 0x47e) {
                    // Type 0x47e: complex multi-sub with camera shake
                    iVar5 = P_SUB(iVar9);
                    if (iVar5 == 0) {
                        fVar19 = 0.5f;
                    } else if (iVar5 == 2) {
                        fVar19 = 4.0f;
                    } else if (iVar5 == 1 || iVar5 == 5) {
                        P_LR(iVar9) *= _DAT_005526e8;
                        P_LG(iVar9) *= _DAT_005526e8;
                        P_LB(iVar9) *= _DAT_005526e8;
                        P_SCALE(iVar9) *= _DAT_00552b44;
                        continue;
                    } else if (iVar5 == 3) {
                        intptr_t entPtr = P_ENT(iVar9);
                        P_LR(iVar9) *= _DAT_005526e8;
                        P_LG(iVar9) *= _DAT_005526e8;
                        P_LB(iVar9) *= _DAT_005526e8;
                        P_SCALE(iVar9) *= _DAT_00552b40;
                        fVar15 = P_GRAV(iVar9) + _DAT_00552660;
                        P_GRAV(iVar9) = fVar15;
                        P_POSX(iVar9) = *(float*)(entPtr + 0x10);
                        fVar3 = P_POSX(iVar9) + _DAT_0055264c;
                        P_POSY(iVar9) = *(float*)(entPtr + 0x14);
                        P_POSZ(iVar9) = *(float*)(entPtr + 0x18);
                        P_POSX(iVar9) = fVar3;
                        P_POSY(iVar9) -= _DAT_0055264c;
                        P_POSZ(iVar9) += fVar15;
                        continue;
                    } else if (iVar5 == 4) {
                        // Subtipo 4: la particula se RE-ANCLA cada tick a un hueso
                        // del dueño.  Es lo que hace que el aura de Swell Life
                        // (efecto 0x47e/1, que spawnea estas particulas) siga al
                        // personaje en vez de quedarse donde se casteo.
                        //
                        // IDA 0x477090:
                        //   TransformPosition(Models + 188*ownerType, boneMatrix,
                        //                     Position, (float*)v5 + 4, 0);
                        //   *v8             = *(float*)(v14+16) + *v8;
                        //   *((float*)v5+5) = *(float*)(v14+20) + ...;
                        //   *((float*)v5+6) = *(float*)(v14+24) + ...;
                        //
                        // El indice de hueso vive en +0x40 (P_ROT): lo escribe
                        // Particle_Spawn case 4 con su parametro Scale, que el
                        // caller toma de la tabla DAT_00559b78 {25,26,27,20,34,
                        // 35,36,0}.  El +0x0C del slot queda fijo en 2.0f, asi
                        // que leer el hueso de ahi daba siempre el mismo.
                        intptr_t entPtr = P_ENT(iVar9);
                        if (entPtr != 0) {
                            float localP[3] = { 0.0f, 0.0f, 0.0f };
                            void* mdl = (void*)((char*)Models + *(short*)(entPtr + 2) * BMD_STRIDE);
                            intptr_t boneTab = *(intptr_t*)&AsObject(entPtr)->BoneMatrices;
                            long long lBone = (long long)(double)P_ROT(iVar9); // __ftol analog
                            // Guard del port (IDA no lo tiene): sin tabla de huesos
                            // el deref cae fuera de todo lo mapeado, y un indice
                            // mayor que numBones (model+34) lee pasado el alloc.
                            int nBones = (boneTab != 0) ? (int)*(short*)((char*)mdl + 34) : 0;
                            if (boneTab != 0 && lBone >= 0 && (int)lBone < nBones) {
                                // BMD__TransformPosition(this, BoneMatrix, Pos, WorldPos, Translate)
                                BMD_TransformPosition(mdl,
                                    (float*)((intptr_t)lBone * 0x30 + boneTab),
                                    localP, &P_POSX(iVar9), 0);
                                P_POSX(iVar9) += *(float*)(entPtr + 0x10);
                                P_POSY(iVar9) += *(float*)(entPtr + 0x14);
                                P_POSZ(iVar9) += *(float*)(entPtr + 0x18);
                            }
                        }
                        iVar5 = _rand();
                        P_GRAV(iVar9) += (float)(iVar5 % 0x28 + 0x3c) * _DAT_00552b3c;
                        iVar5 = _rand();
                        P_SCALE(iVar9) -= (float)(iVar5 % 400 + 400) * _DAT_00552b38;
                        P_POSZ(iVar9) += P_GRAV(iVar9);
                        P_LR(iVar9) *= _DAT_00552b34;
                        P_LG(iVar9) *= _DAT_00552b34;
                        P_LB(iVar9) *= _DAT_00552b34;
                        continue;
                    } else {
                        continue;
                    }
                    // Shared path for sub 0 and 2: camera shake + drift
                    iVar5 = P_SUB(iVar9);
                    iVar6 = _rand();
                    fVar15 = (float)(_rand() % 0x7d1 - 1000) * _DAT_00552b30;
                    float fDrift = (float)(iVar6 % 0x7d1 - 1000) * _DAT_00552b30 + DAT_07c800fc;
                    float fShakeZ = DAT_07c80100 * fVar19;
                    P_POSX(iVar9) += (DAT_07c800f8 + fVar15) * fVar19;
                    P_POSY(iVar9) += fDrift * fVar19;
                    P_POSZ(iVar9) += fVar19 * _DAT_00552660 + fShakeZ;
                    if (iVar5 == 0) {
                        fVar15 = P_SCALE(iVar9) - _DAT_00552874;
                    } else if (iVar5 == 1) {
                        fVar15 = P_SCALE(iVar9) * _DAT_00552b2c;
                    } else {
                        fVar15 = P_SCALE(iVar9) * _DAT_00552b44;
                    }
                    P_SCALE(iVar9) = fVar15;
                    P_LR(iVar9) = fVar15;
                    P_LG(iVar9) = fVar15;
                    P_LB(iVar9) = fVar15;
                    if (iVar5 == 1) {
                        fVar15 = P_SCALE(iVar9) * _DAT_005528b8;
                        P_LR(iVar9) = fVar15;
                        P_LG(iVar9) = fVar15;
                        P_LB(iVar9) = fVar15;
                    }
                    if (iVar5 == 5) {
                        fVar15 = P_SCALE(iVar9) * _DAT_005528b8;
                        P_LR(iVar9) = fVar15;
                        P_LG(iVar9) = fVar15;
                        P_LB(iVar9) = fVar15;
                    }
                    if ((iVar5 == 0 && P_SCALE(iVar9) <= _DAT_005524f4) ||
                        (iVar5 == 2 && P_SCALE(iVar9) <= _DAT_005528b8)) {
                        P_LIFE(iVar9) = -1;
                        P_ACTIVE(iVar9) = '\0';
                    }
                    continue;
                }
                continue; // types between 0x68 and 0x47e not handled above
            }

            // Types 0x67..0x68: glow + fade
            if (P_LIFE(iVar9) < 1) P_ACTIVE(iVar9) = '\0';
            if (P_SUB(iVar9) == 1) {
                fVar15 = P_GRAV(iVar9) + P_SCALE(iVar9);
            } else {
                fVar15 = P_SCALE(iVar9) + _DAT_00552910;
            }
            P_SCALE(iVar9) = fVar15;
            {
                float lt3[3] = { 0.05f, 0.05f, 0.05f };
                // fall through to color decay label
            }
            P_LR(iVar9) -= _DAT_00552874;
            P_LG(iVar9) -= _DAT_00552874;
            P_LB(iVar9) -= _DAT_00552874;
            continue;
        } // end iVar5 < 0x4a7

        // ── Types 0x4a7..0x4c3 ───────────────────────────────────────────────
        switch (iVar5) {

        case BITMAP_LIGHTNING + 1:
            {
                const ParticleStep step = MoveParticles_Type1191(iVar9);
                if (step == ParticleStep::Break) break;
            }
            break;
        case BITMAP_FIRE:
        case BITMAP_FIRE + 2:
            {
                const ParticleStep step = MoveParticles_Fire(fVar15, iVar5, iVar9);
                if (step == ParticleStep::Break) break;
            }
            break;
        case BITMAP_FIRE + 1:
            {
                const ParticleStep step = MoveParticles_Type1196(fVar15, iVar9);
                if (step == ParticleStep::Break) break;
            }
            break;
        case BITMAP_FLAME: {
            if (P_LIFE(iVar9) < 1) P_ACTIVE(iVar9) = '\0';
            switch (P_SUB(iVar9)) {
            case 1:
                if (P_LIFE(iVar9) == 10) {
                    P_VELY(iVar9) += _DAT_00552b20;
                    P_SCALE(iVar9) -= _DAT_00552934;
                }
                fVar15 = (float)(_rand() % 0x168);
                break;
            case 2:
                fVar15 = (float)(_rand() % 0x168);
                break;
            case 3:
                P_ROT(iVar9) = (float)(_rand() % 0x168);
                continue;
            case 4: {
                intptr_t entPtr = P_ENT(iVar9);
                P_POSX(iVar9) += *(float*)(entPtr + 0x10) - P_TGX(iVar9);
                P_POSY(iVar9) += *(float*)(entPtr + 0x14) - P_TGY(iVar9);
                P_POSZ(iVar9) += *(float*)(entPtr + 0x18) - P_TGZ(iVar9) + P_GRAV(iVar9);
                fVar15 = P_GRAV(iVar9) + _DAT_005524f4;
                P_TGX(iVar9) = *(float*)(entPtr + 0x10);
                P_TGY(iVar9) = *(float*)(entPtr + 0x14);
                P_TGZ(iVar9) = *(float*)(entPtr + 0x18);
                P_GRAV(iVar9) = fVar15;
                fVar15 = (float)(_rand() % 0x168);
                break;
            }
            case 5:
                if (P_LIFE(iVar9) == 10) {
                    P_VELY(iVar9) += _DAT_00552b20;
                    P_SCALE(iVar9) *= _DAT_00552530;
                }
                fVar15 = (float)(_rand() % 0x168);
                break;
            default:
                continue;
            }
            P_ROT(iVar9) = fVar15;
            P_LR(iVar9) -= _DAT_00552874;
            P_LG(iVar9) -= _DAT_00552874;
            P_LB(iVar9) -= _DAT_00552874;
            break;
        }

        case BITMAP_BLOOD:
        case BITMAP_BLOOD + 1:
        {
            fVar15 = P_VELX(iVar9) * _DAT_00552b44;
            P_VELX(iVar9) = fVar15;
            float vy = P_VELY(iVar9) * _DAT_00552b44;
            P_VELY(iVar9) = vy;
            float vz = P_VELZ(iVar9) * _DAT_00552b44;
            P_VELZ(iVar9) = vz;
            P_FRAME(iVar9) = (0xc - P_LIFE(iVar9)) / 3;
            P_POSX(iVar9) += fVar15;
            P_POSY(iVar9) += vy;
            P_POSZ(iVar9) += vz;
            break;
        }

        case BITMAP_EXPLOTION:
            {
                const ParticleStep step = MoveParticles_Explotion(iVar9);
                if (step == ParticleStep::Break) break;
            }
            break;
        case BITMAP_EXPLOTION + 1:
            {
                const ParticleStep step = MoveParticles_Type1216(iVar9);
                if (step == ParticleStep::Break) break;
            }
            break;
        default:
            break;
        } // end types 0x4a7..0x4c3 switch

    } // end particle loop
}

// Undefine particle helper macros to avoid polluting other translation units
#undef P_BASE
#undef P_
#undef P_ACTIVE
#undef P_TYPE
#undef P_SUB
#undef P_SCALE
#undef P_POSX
#undef P_POSY
#undef P_POSZ
#undef P_DIRX
#undef P_ANGLE
#undef P_LR
#undef P_LG
#undef P_LB
#undef P_LIFE
#undef P_ENT
#undef P_ROT
#undef P_FRAME
#undef P_GRAV
#undef P_VELX
#undef P_VELY
#undef P_VELZ
#undef P_EX0
#undef P_EX1
#undef P_EX2
#undef P_TGX
#undef P_TGY
#undef P_TGZ
#undef BMP_HEIGHT
