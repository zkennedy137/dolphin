// Porpoise's native routines.
// Copyright (C) 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_PorpoiseNative.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <vector>

#include <immintrin.h>

#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

// Every multiply and add below rounds on its own, as the game's instructions do
// under the JIT: none may be fused into another.
#ifdef __clang__
#pragma clang fp contract(off)
#define PORPOISE_FMA __attribute__((target("fma")))
#define PORPOISE_AVX __attribute__((target("avx,fma")))
#else
#define PORPOISE_FMA
#define PORPOISE_AVX
#endif

namespace HLE_PorpoiseNative
{
namespace
{
// Punch-Out!! (R7PE01, 801c0be0): skinning. For each vertex, its position and
// normal through up to four weighted bone matrices (4x4, column-major, 64
// bytes), summed into the output arrays.
//   r3 vertices, r4 matrices, r5 24-byte weight records (four weights, four
//   bone indices, a count), r6/r7 positions in/out, r9/r10 normals in/out
constexpr u32 kSkinAccumulate[] = {
    0x9421FFF0, 0x39800000, 0xC1A29EA8, 0x93E1000C, 0x48000104, 0xD1A70000,
    0x7CAB2B78, 0x88050014, 0x3BE00000, 0xD1A70004, 0xD1A70008, 0xD1AA0000,
    0xD1AA0004, 0xD1AA0008, 0x7C0903A6, 0x28000000, 0x408100BC, 0x7D05FA14,
    0xC18B0000, 0x88080010, 0x396B0004, 0xE0060000, 0x3BFF0001, 0x54003032,
    0xE0268008, 0x7D040214, 0xE0480000, 0xE0680008, 0x11420018, 0xE0880010,
    0x11630018, 0xE0A80018, 0xE0C80020, 0x1144501E, 0x1165581E, 0xE0E80028,
    0xE1080030, 0x1146505C, 0xE1280038, 0x1167585C, 0xE0070000, 0x1148502A,
    0xE0278008, 0x1169582A, 0x114A031C, 0xE0090000, 0x116B0B1C, 0xE0298008,
    0xF1470000, 0x11420018, 0xF1678008, 0x11630018, 0x1144501E, 0x1165581E,
    0xE00A0000, 0x1146505C, 0x1167585C, 0xE02A8008, 0x114A031C, 0x116B0B1C,
    0xF14A0000, 0xF16A8008, 0x4200FF4C, 0x38A50018, 0x38C6000C, 0x38E7000C,
    0x3929000C, 0x394A000C, 0x398C0001, 0x7C0C1800, 0x4180FEFC, 0x83E1000C,
    0x38210010, 0x4E800020,
};

// Punch-Out!! (801c108c): the same, the position first moved by a per-vertex
// offset (a morph), r8.
constexpr u32 kSkinMorphAccumulate[] = {
    0x9421FFF0, 0xC1A29EA8, 0x93E1000C, 0x3BE00000, 0x93C10008, 0x48000118,
    0xD1A70000, 0x7CAC2B78, 0x88050014, 0x3BC00000, 0xD1A70004, 0xD1A70008,
    0xD1AA0000, 0xD1AA0004, 0xD1AA0008, 0x7C0903A6, 0x28000000, 0x408100CC,
    0x7D65F214, 0xC18C0000, 0x880B0010, 0x398C0004, 0xE0060000, 0x3BDE0001,
    0xE1480000, 0x54003032, 0x7D640214, 0xE0268008, 0x1000502A, 0xE04B0000,
    0xE1688008, 0xE06B0008, 0x11420018, 0xE08B0010, 0x1021582A, 0xE0AB0018,
    0x11630018, 0xE0CB0020, 0x1144501E, 0xE0EB0028, 0x1165581E, 0xE10B0030,
    0xE12B0038, 0x1146505C, 0x1167585C, 0xE0070000, 0xE0278008, 0x1148502A,
    0x1169582A, 0x114A031C, 0xE0090000, 0x116B0B1C, 0xE0298008, 0xF1470000,
    0x11420018, 0xF1678008, 0x11630018, 0x1144501E, 0x1165581E, 0xE00A0000,
    0x1146505C, 0x1167585C, 0xE02A8008, 0x114A031C, 0x116B0B1C, 0xF14A0000,
    0xF16A8008, 0x4200FF3C, 0x38A50018, 0x38C6000C, 0x38E7000C, 0x3908000C,
    0x3929000C, 0x394A000C, 0x3BFF0001, 0x7C1F1800, 0x4180FEE8, 0x83E1000C,
    0x83C10008, 0x38210010, 0x4E800020,
};

// Punch-Out!! (8011eb44, inside a larger function): three copies of each
// vertex pushed out along its normal, by a per-vertex byte times a scale (the
// boxers' outlines), in the locked cache. The loop only, from its mtctr to its
// bdnz; it continues at the instruction after.
constexpr u32 kOffsetShells[] = {
    0x7C6903A6, 0x40810090, 0x88690003, 0x39290004, 0x93E10050, 0xC3AF0000,
    0x90610054, 0xE0080000, 0xC9A10050, 0xE0288008, 0x3908000C, 0xEDADF828,
    0xE0470000, 0xE0678008, 0x38E7000C, 0xC3CF0004, 0xEF7C0372, 0xC1AF0008,
    0xEC9D06F2, 0xECBE06F2, 0xECCD06F2, 0x10E0111C, 0x1101191C, 0x1120115C,
    0xF0E50000, 0x1141195C, 0x1160119C, 0xF1058008, 0x1181199C, 0x38A5000C,
    0xF1260000, 0xF1468008, 0x38C6000C, 0xF1640000, 0xF1848008, 0x3884000C,
    0x4200FF78,
};

// Words whose 16-bit displacement may differ from one build of the game to
// another (another region's constants, or its stack frame laid out otherwise):
// only their upper half has to match, and the routine uses the game's own
// displacement. -1 ends the list.
struct Routine
{
  const char* hook;
  const char* what;
  const u32* words;
  u32 count;
  int free[3];
};

constexpr Routine kRoutines[] = {
    // lfs f13, d(rtoc)
    {"PorpoiseSkinAccumulate", "skinning", kSkinAccumulate, u32(std::size(kSkinAccumulate)),
     {2, -1, -1}},
    {"PorpoiseSkinMorphAccumulate", "morph skinning", kSkinMorphAccumulate,
     u32(std::size(kSkinMorphAccumulate)), {1, -1, -1}},
    // stw r31, d(sp); stw r3, d+4(sp); lfd f13, d(sp)
    {"PorpoiseOffsetShells", "outline shells", kOffsetShells, u32(std::size(kOffsetShells)),
     {4, 6, 8}},
};

struct Match
{
  u32 address;
  s16 displacement[3];
};

struct Hooked
{
  u32 address;
  const char* hook;
};
std::vector<Hooked> s_hooked;
// Where the outline loop continues (it is hooked in the middle of a function).
u32 s_shells_end = 0;
// The displacements of the build found: the clearing constant's from rtoc (for
// each skinning routine), and the outline loop's scratch doubleword on the stack.
s32 s_clear_at[2] = {-0x6158, -0x6158};
s32 s_shells_scratch = 0x50;
std::atomic<bool> s_logged[std::size(kRoutines)];
// The two-vertex path needs AVX (the PS5 has it; checked anyway).
bool s_avx = false;

struct Section
{
  u32 start;
  u32 end;  // exclusive
};

std::vector<Section> TextSections(const DiscIO::VolumeDisc& volume)
{
  std::vector<Section> sections;
  const DiscIO::Partition partition = volume.GetGamePartition();
  const std::optional<u64> dol = DiscIO::GetBootDOLOffset(volume, partition);
  if (!dol)
    return sections;
  for (u32 i = 0; i < 7; ++i)
  {
    const std::optional<u32> address = volume.ReadSwapped<u32>(*dol + 0x48 + i * 4, partition);
    const std::optional<u32> size = volume.ReadSwapped<u32>(*dol + 0x90 + i * 4, partition);
    if (!address || !size || *size == 0 || *size > 0x01800000)
      continue;
    if (*address < 0x80000000 || *address >= 0x81800000)
      continue;
    sections.push_back({*address, *address + (*size & ~3u)});
  }
  return sections;
}

bool IsFree(const Routine& routine, u32 i)
{
  for (const int f : routine.free)
    if (f >= 0 && u32(f) == i)
      return true;
  return false;
}

std::vector<Match> Find(const Core::CPUThreadGuard& guard, const std::vector<Section>& sections,
                        const Routine& routine)
{
  std::vector<Match> found;
  for (const Section& section : sections)
  {
    for (u32 address = section.start; address + routine.count * 4 <= section.end; address += 4)
    {
      if (PowerPC::MMU::HostRead_Instruction(guard, address) != routine.words[0])
        continue;
      u32 i = 1;
      for (; i < routine.count; ++i)
      {
        const u32 word = PowerPC::MMU::HostRead_Instruction(guard, address + i * 4);
        const u32 mask = IsFree(routine, i) ? 0xFFFF0000u : 0xFFFFFFFFu;
        if ((word & mask) != (routine.words[i] & mask))
          break;
      }
      if (i != routine.count)
        continue;
      Match match{address, {0, 0, 0}};
      for (int k = 0; k < 3; ++k)
        if (routine.free[k] >= 0)
          match.displacement[k] =
              s16(PowerPC::MMU::HostRead_Instruction(guard, address + u32(routine.free[k]) * 4));
      found.push_back(match);
    }
  }
  return found;
}

void LogFirst(std::size_t routine)
{
  if (!s_logged[routine].exchange(true))
    std::fprintf(stderr, "native: %s running natively\n", kRoutines[routine].what);
}

// The game's memory. Fast: host pointers, for ranges already known to be plain
// RAM (the locked cache, or BAT-mapped memory in the fastmem arena, as the
// JIT's own fastmem accesses use). Slow: the MMU, for anything else.
struct Fast
{
  u8* base;
  u8* l1;
  u8* At(u32 ea) const { return (ea >> 28) == 0xE ? l1 + (ea - 0xE0000000u) : base + ea; }
  u8 R8(u32 ea) const { return *At(ea); }
  u32 R32(u32 ea) const
  {
    u32 v;
    std::memcpy(&v, At(ea), 4);
    return Common::swap32(v);
  }
  void W32(u32 ea, u32 v) const
  {
    v = Common::swap32(v);
    std::memcpy(At(ea), &v, 4);
  }
  // Two big-endian singles (psq_l, W=0), as doubles: the first in the low lane.
  PORPOISE_FMA __m128d Pair(u32 ea) const
  {
    u64 v;
    std::memcpy(&v, At(ea), 8);
    v = std::rotl(__builtin_bswap64(v), 32);
    return _mm_cvtps_pd(_mm_castsi128_ps(_mm_cvtsi64_si128(s64(v))));
  }
  // psq_st, W=0.
  PORPOISE_FMA void StorePair(u32 ea, __m128d v) const
  {
    const u64 bits = u64(_mm_cvtsi128_si64(_mm_castps_si128(_mm_cvtpd_ps(v))));
    const u64 out = __builtin_bswap64(std::rotl(bits, 32));
    std::memcpy(At(ea), &out, 8);
  }
};

struct Slow
{
  PowerPC::MMU& mmu;
  u8 R8(u32 ea) const { return mmu.Read<u8>(ea); }
  u32 R32(u32 ea) const { return mmu.Read<u32>(ea); }
  void W32(u32 ea, u32 v) const { mmu.Write<u32>(v, ea); }
  PORPOISE_FMA __m128d Pair(u32 ea) const
  {
    const float lo = std::bit_cast<float>(R32(ea));
    const float hi = std::bit_cast<float>(R32(ea + 4));
    return _mm_set_pd(double(hi), double(lo));
  }
  PORPOISE_FMA void StorePair(u32 ea, __m128d v) const
  {
    alignas(16) float f[4];
    _mm_store_ps(f, _mm_cvtpd_ps(v));
    W32(ea, std::bit_cast<u32>(f[0]));
    W32(ea + 4, std::bit_cast<u32>(f[1]));
  }
};

bool Plain(Core::System& system, u32 ea, u64 bytes)
{
  if (bytes == 0)
    return true;
  const u64 end = u64(ea) + bytes;
  if (end > 0x1'0000'0000ull)
    return false;
  auto& memory = system.GetMemory();
  if ((ea >> 28) == 0xE || ((end - 1) >> 28) == 0xE)
  {
    return memory.GetL1Cache() && ea >= 0xE0000000u &&
           end <= 0xE0000000ull + memory.GetL1CacheSize();
  }
  if (!memory.GetLogicalBase())
    return false;
  auto& mmu = system.GetMMU();
  for (u64 page = ea & ~u64(PowerPC::BAT_PAGE_SIZE - 1); page < end; page += PowerPC::BAT_PAGE_SIZE)
  {
    if (!mmu.IsOptimizableRAMAddress(u32(std::max<u64>(page, ea)), 8))
      return false;
  }
  return true;
}

Fast FastOf(Core::System& system)
{
  return {system.GetMemory().GetLogicalBase(), system.GetMemory().GetL1Cache()};
}

float F(u32 bits)
{
  return std::bit_cast<float>(bits);
}

u32 B(float value)
{
  return std::bit_cast<u32>(value);
}

// The JIT's paired-single arithmetic on single-precision values, each step
// rounded to single as it is there (accurate single precision, the default):
// a multiply or an add in double rounded to single is the same as one in
// single (the double result is exact, or rounds the same way); a multiply-add
// is fused in double, then rounded to single (Dolphin's FMA path, without
// "Exact Multiply-Add").
PORPOISE_FMA inline float Madd(float a, float c, float b)
{
  return float(__builtin_fma(double(a), double(c), double(b)));
}

// The same on a pair of lanes held as doubles (as the JIT holds paired singles):
// each step in double, then rounded to single and back.
PORPOISE_FMA inline __m128d Round(__m128d v)
{
  return _mm_cvtps_pd(_mm_cvtpd_ps(v));
}
PORPOISE_FMA inline double Round(double v)
{
  return double(float(v));
}
PORPOISE_FMA inline __m128d Dup(double v)
{
  return _mm_set1_pd(v);
}

u32 Compare(s32 a, s32 b, u32 so)
{
  return (a < b ? 8u : a > b ? 4u : 2u) | so;
}

template <bool Morph, class M>
PORPOISE_FMA void Skin(const M& m, PowerPC::PowerPCState& s, s32 clear_at)
{
  u32* const g = s.gpr;
  const u32 n = g[3], mats = g[4];
  u32 r0 = g[0], r5 = g[5], r6 = g[6], r7 = g[7], r8 = g[8], r9 = g[9], r10 = g[10];
  u32 r11 = g[11], r12 = g[12], r30 = g[30], r31 = g[31];
  u32 ctr = CTR(s);

  // Prologue: stwu sp, -16(sp); lfs f13, clear_at(rtoc); stw r31 (and r30).
  u32 sp = g[1];
  m.W32(sp - 16, sp);
  sp -= 16;
  const u32 clear = m.R32(g[2] + u32(clear_at));
  m.W32(sp + 0xC, r31);
  // The vertex counter: r12 (skinning) or r31 (morph); the weight pointer: r11
  // or r12; the bone counter: r31 or r30; the record and matrix pointer: r8 or r11.
  u32& vertex = Morph ? r31 : r12;
  u32& weights = Morph ? r12 : r11;
  u32& bone = Morph ? r30 : r31;
  u32& scratch = Morph ? r11 : r8;
  vertex = 0;
  if (Morph)
    m.W32(sp + 8, r30);

  while (s32(vertex) < s32(n))
  {
    m.W32(r7, clear);
    weights = r5;
    r0 = m.R8(r5 + 0x14);
    bone = 0;
    m.W32(r7 + 4, clear);
    m.W32(r7 + 8, clear);
    m.W32(r10, clear);
    m.W32(r10 + 4, clear);
    m.W32(r10 + 8, clear);
    ctr = r0;
    if (r0 != 0)
    {
      do
      {
        // Every load of the bone comes before its first store, as in the game.
        scratch = r5 + bone;
        const float w = F(m.R32(weights));
        r0 = m.R8(scratch + 0x10);
        weights += 4;
        float x = F(m.R32(r6));
        float y = F(m.R32(r6 + 4));
        float z = F(m.R32(r6 + 8));
        bone += 1;
        if constexpr (Morph)
        {
          x = x + F(m.R32(r8));
          y = y + F(m.R32(r8 + 4));
          z = z + F(m.R32(r8 + 8));
        }
        r0 = std::rotl(r0, 6) & 0xFFFFFFC0u;
        scratch = mats + r0;
        const u32 mx = scratch;
        // The matrix's columns: (0,1) and 2, (4,5) and 6, (8,9) and 10, (12,13) and 14.
        const __m128d c01 = m.Pair(mx), c45 = m.Pair(mx + 0x10), c89 = m.Pair(mx + 0x20),
                      cCD = m.Pair(mx + 0x30);
        const double c2 = double(F(m.R32(mx + 0x08))), c6 = double(F(m.R32(mx + 0x18))),
                     cA = double(F(m.R32(mx + 0x28))), cE = double(F(m.R32(mx + 0x38)));
        const double dx = double(x), dy = double(y), dz = double(z), dw = double(w);

        // Position: the matrix times (x, y, z, 1), times the weight, plus the sum so far.
        __m128d p = Round(_mm_mul_pd(c01, Dup(dx)));
        double pz = Round(c2 * dx);
        p = Round(_mm_fmadd_pd(c45, Dup(dy), p));
        pz = Round(__builtin_fma(c6, dy, pz));
        p = Round(_mm_fmadd_pd(c89, Dup(dz), p));
        pz = Round(__builtin_fma(cA, dz, pz));
        const __m128d o = m.Pair(r7);
        const double oz = double(F(m.R32(r7 + 8)));
        p = Round(_mm_add_pd(cCD, p));
        pz = Round(cE + pz);
        p = Round(_mm_fmadd_pd(p, Dup(dw), o));
        pz = Round(__builtin_fma(pz, dw, oz));
        const __m128d nxy = m.Pair(r9);
        const double nz = double(F(m.R32(r9 + 8)));
        m.StorePair(r7, p);
        m.W32(r7 + 8, B(float(pz)));

        // Normal: the matrix's 3x3 times it, times the weight, plus the sum so far.
        const double nx = _mm_cvtsd_f64(nxy), ny = _mm_cvtsd_f64(_mm_unpackhi_pd(nxy, nxy));
        __m128d q = Round(_mm_mul_pd(c01, Dup(nx)));
        double qz = Round(c2 * nx);
        q = Round(_mm_fmadd_pd(c45, Dup(ny), q));
        qz = Round(__builtin_fma(c6, ny, qz));
        const __m128d so = m.Pair(r10);
        const double sz = double(F(m.R32(r10 + 8)));
        q = Round(_mm_fmadd_pd(c89, Dup(nz), q));
        qz = Round(__builtin_fma(cA, nz, qz));
        q = Round(_mm_fmadd_pd(q, Dup(dw), so));
        qz = Round(__builtin_fma(qz, dw, sz));
        m.StorePair(r10, q);
        m.W32(r10 + 8, B(float(qz)));
      } while (--ctr != 0);
    }
    r5 += 24;
    r6 += 12;
    r7 += 12;
    if constexpr (Morph)
      r8 += 12;
    r9 += 12;
    r10 += 12;
    vertex += 1;
  }

  // cmpw vertex, r3; the epilogue: lwz r31 (and r30); addi sp, sp, 16; blr.
  s.cr.SetField(0, Compare(s32(vertex), s32(n), s.GetXER_SO()));
  r31 = m.R32(sp + 0xC);
  if (Morph)
    r30 = m.R32(sp + 8);
  g[1] = sp + 16;
  g[0] = r0;
  g[5] = r5;
  g[6] = r6;
  g[7] = r7;
  g[8] = r8;
  g[9] = r9;
  g[10] = r10;
  g[11] = r11;
  g[12] = r12;
  g[30] = r30;
  g[31] = r31;
  CTR(s) = ctr;
  s.npc = LR(s);
}

// Two vertices at once, in the lanes of 256-bit registers, for a call whose
// output arrays overlap nothing it reads (Disjoint): then the order of its loads
// and stores can't matter, and each vertex's sums stay in registers. Every
// lane's arithmetic is the scalar routine's, step for step.
PORPOISE_AVX inline __m256d Round(__m256d v)
{
  return _mm256_cvtps_pd(_mm256_cvtpd_ps(v));
}

PORPOISE_AVX inline __m256d Pair2(const Fast& m, u32 ea0, u32 ea1)
{
  return _mm256_set_m128d(m.Pair(ea1), m.Pair(ea0));
}

PORPOISE_AVX inline __m128d Single2(const Fast& m, u32 ea0, u32 ea1)
{
  return _mm_set_pd(double(F(m.R32(ea1))), double(F(m.R32(ea0))));
}

PORPOISE_AVX inline __m256d Spread(__m128d v)
{
  // (a, b) -> (a, a, b, b)
  return _mm256_set_m128d(_mm_unpackhi_pd(v, v), _mm_unpacklo_pd(v, v));
}

template <bool Morph>
PORPOISE_AVX void SkinBatch(const Fast& m, PowerPC::PowerPCState& s, s32 clear_at)
{
  u32* const g = s.gpr;
  const u32 n = g[3], mats = g[4], r5 = g[5], r6 = g[6], r7 = g[7], r8 = g[8], r9 = g[9],
            r10 = g[10];
  u32 r31 = g[31], r30 = g[30];

  u32 sp = g[1];
  m.W32(sp - 16, sp);
  sp -= 16;
  const u32 clear = m.R32(g[2] + u32(clear_at));
  m.W32(sp + 0xC, r31);
  if (Morph)
    m.W32(sp + 8, r30);
  const double clear_d = double(F(clear));

  for (u32 v = 0; v < n; v += 2)
  {
    const bool two = v + 1 < n;
    const u32 v1 = two ? v + 1 : v;
    const u32 rec0 = r5 + v * 24, rec1 = r5 + v1 * 24;
    const u32 k0 = m.R8(rec0 + 0x14), k1 = two ? m.R8(rec1 + 0x14) : 0;
    const u32 in0 = r6 + v * 12, in1 = r6 + v1 * 12;
    __m128d xz = Single2(m, in0, in1);  // x of each vertex
    __m128d yz = Single2(m, in0 + 4, in1 + 4);
    __m128d zz = Single2(m, in0 + 8, in1 + 8);
    if constexpr (Morph)
    {
      // Single adds: the same as the JIT's double add rounded to single.
      const u32 d0 = r8 + v * 12, d1 = r8 + v1 * 12;
      xz = Round(_mm_add_pd(xz, Single2(m, d0, d1)));
      yz = Round(_mm_add_pd(yz, Single2(m, d0 + 4, d1 + 4)));
      zz = Round(_mm_add_pd(zz, Single2(m, d0 + 8, d1 + 8)));
    }
    const __m256d X = Spread(xz), Y = Spread(yz), Z = Spread(zz);
    const u32 n0 = r9 + v * 12, n1 = r9 + v1 * 12;
    const __m128d nxz = Single2(m, n0, n1), nyz = Single2(m, n0 + 4, n1 + 4),
                  nzz = Single2(m, n0 + 8, n1 + 8);
    const __m256d NX = Spread(nxz), NY = Spread(nyz);

    __m256d P = _mm256_set1_pd(clear_d), Q = P;
    __m128d Pz = _mm_set1_pd(clear_d), Qz = Pz;
    const u32 k = std::max(k0, k1);
    for (u32 j = 0; j < k; ++j)
    {
      const u32 i0 = m.R8(rec0 + 0x10 + j), i1 = m.R8(rec1 + 0x10 + j);
      const u32 m0 = mats + (i0 << 6), m1 = mats + (i1 << 6);
      const __m128d wz = Single2(m, rec0 + j * 4, rec1 + j * 4);
      const __m256d W = Spread(wz);
      const __m256d c01 = Pair2(m, m0, m1), c45 = Pair2(m, m0 + 0x10, m1 + 0x10),
                    c89 = Pair2(m, m0 + 0x20, m1 + 0x20), cCD = Pair2(m, m0 + 0x30, m1 + 0x30);
      const __m128d c2 = Single2(m, m0 + 0x08, m1 + 0x08), c6 = Single2(m, m0 + 0x18, m1 + 0x18),
                    cA = Single2(m, m0 + 0x28, m1 + 0x28), cE = Single2(m, m0 + 0x38, m1 + 0x38);
      const __m256d on = _mm256_castsi256_pd(_mm256_set_epi64x(
          j < k1 ? -1 : 0, j < k1 ? -1 : 0, j < k0 ? -1 : 0, j < k0 ? -1 : 0));
      const __m128d onz = _mm_castsi128_pd(_mm_set_epi64x(j < k1 ? -1 : 0, j < k0 ? -1 : 0));

      __m256d p = Round(_mm256_mul_pd(c01, X));
      __m128d pz = Round(_mm_mul_pd(c2, xz));
      p = Round(_mm256_fmadd_pd(c45, Y, p));
      pz = Round(_mm_fmadd_pd(c6, yz, pz));
      p = Round(_mm256_fmadd_pd(c89, Z, p));
      pz = Round(_mm_fmadd_pd(cA, zz, pz));
      p = Round(_mm256_add_pd(cCD, p));
      pz = Round(_mm_add_pd(cE, pz));
      P = _mm256_blendv_pd(P, Round(_mm256_fmadd_pd(p, W, P)), on);
      Pz = _mm_blendv_pd(Pz, Round(_mm_fmadd_pd(pz, wz, Pz)), onz);

      __m256d q = Round(_mm256_mul_pd(c01, NX));
      __m128d qz = Round(_mm_mul_pd(c2, nxz));
      q = Round(_mm256_fmadd_pd(c45, NY, q));
      qz = Round(_mm_fmadd_pd(c6, nyz, qz));
      q = Round(_mm256_fmadd_pd(c89, Spread(nzz), q));
      qz = Round(_mm_fmadd_pd(cA, nzz, qz));
      Q = _mm256_blendv_pd(Q, Round(_mm256_fmadd_pd(q, W, Q)), on);
      Qz = _mm_blendv_pd(Qz, Round(_mm_fmadd_pd(qz, wz, Qz)), onz);
    }

    // A vertex with no bones keeps the stfs'd words as they were.
    const auto put = [&](u32 out, u32 bones, __m128d xy, double z) {
      if (bones == 0)
      {
        m.W32(out, clear);
        m.W32(out + 4, clear);
        m.W32(out + 8, clear);
        return;
      }
      m.StorePair(out, xy);
      m.W32(out + 8, B(float(z)));
    };
    put(r7 + v * 12, k0, _mm256_castpd256_pd128(P), _mm_cvtsd_f64(Pz));
    put(r10 + v * 12, k0, _mm256_castpd256_pd128(Q), _mm_cvtsd_f64(Qz));
    if (two)
    {
      put(r7 + v1 * 12, k1, _mm256_extractf128_pd(P, 1), _mm_cvtsd_f64(_mm_unpackhi_pd(Pz, Pz)));
      put(r10 + v1 * 12, k1, _mm256_extractf128_pd(Q, 1), _mm_cvtsd_f64(_mm_unpackhi_pd(Qz, Qz)));
    }
  }

  // The registers as the scalar routine leaves them (n > 0 here).
  const u32 last = r5 + (n - 1) * 24;
  const u32 k_last = m.R8(last + 0x14);
  u32 r0 = k_last ? (u32(m.R8(last + 0x10 + k_last - 1)) << 6) : 0;
  u32 scratch = Morph ? g[11] : g[8];
  for (u32 v = n; v-- > 0;)
  {
    const u32 rec = r5 + v * 24;
    const u32 kv = m.R8(rec + 0x14);
    if (kv)
    {
      scratch = mats + (u32(m.R8(rec + 0x10 + kv - 1)) << 6);
      break;
    }
  }
  const u32 weights = last + k_last * 4;
  s.cr.SetField(0, Compare(s32(n), s32(n), s.GetXER_SO()));
  r31 = m.R32(sp + 0xC);
  if (Morph)
    r30 = m.R32(sp + 8);
  g[1] = sp + 16;
  g[0] = r0;
  g[5] = r5 + n * 24;
  g[6] = r6 + n * 12;
  g[7] = r7 + n * 12;
  g[8] = Morph ? r8 + n * 12 : scratch;
  g[9] = r9 + n * 12;
  g[10] = r10 + n * 12;
  if (Morph)
  {
    g[11] = scratch;
    g[12] = weights;
    g[30] = r30;
    g[31] = r31;
  }
  else
  {
    g[11] = weights;
    g[12] = n;
    g[31] = r31;
  }
  CTR(s) = 0;
  s.npc = LR(s);
}

bool Disjoint(u32 a, u64 a_bytes, u32 b, u64 b_bytes)
{
  return u64(a) + a_bytes <= b || u64(b) + b_bytes <= a;
}

template <class M>
PORPOISE_FMA void Shells(const M& m, PowerPC::PowerPCState& s, u32 end, s32 scratch)
{
  u32* const g = s.gpr;
  u32 r3 = g[3], r4 = g[4], r5 = g[5], r6 = g[6], r7 = g[7], r8 = g[8], r9 = g[9];
  const u32 r15 = g[15], r31 = g[31], sp = g[1];

  // mtctr r3. Only reached past "cmpwi r3, 0; ble-", so r3 is above 0 and the
  // second ble- isn't taken.
  CTR(s) = r3;

  const double f28 = s.ps[28].PS0AsDouble();
  const double f31 = s.ps[31].PS0AsDouble();
  u32 ctr = r3;
  float f4 = 0, f5 = 0, f6 = 0, f13 = 0, f27 = 0, f29 = 0, f30 = 0;
  do
  {
    r3 = m.R8(r9 + 3);
    r9 += 4;
    m.W32(sp + u32(scratch), r31);
    f29 = F(m.R32(r15));
    m.W32(sp + u32(scratch) + 4, r3);
    const float p0x = F(m.R32(r8)), p0y = F(m.R32(r8 + 4)), p1x = F(m.R32(r8 + 8));
    // lfd f13, scratch(sp); fsubs f13, f13, f31: the byte, through the 2^52 trick.
    const u64 bits = (u64(m.R32(sp + u32(scratch))) << 32) | m.R32(sp + u32(scratch) + 4);
    r8 += 12;
    const float byte = float(std::bit_cast<double>(bits) - f31);
    const float p2x = F(m.R32(r7)), p2y = F(m.R32(r7 + 4)), p3x = F(m.R32(r7 + 8));
    r7 += 12;
    f30 = F(m.R32(r15 + 4));
    f27 = float(f28 * double(byte));
    f13 = F(m.R32(r15 + 8));
    f4 = f29 * f27;
    f5 = f30 * f27;
    f6 = f13 * f27;
    const float p7x = Madd(p0x, f4, p2x), p7y = Madd(p0y, f4, p2y), p8 = Madd(p1x, f4, p3x);
    const float p9x = Madd(p0x, f5, p2x), p9y = Madd(p0y, f5, p2y), p10 = Madd(p1x, f5, p3x);
    const float p11x = Madd(p0x, f6, p2x), p11y = Madd(p0y, f6, p2y), p12 = Madd(p1x, f6, p3x);
    m.W32(r5, B(p7x));
    m.W32(r5 + 4, B(p7y));
    m.W32(r5 + 8, B(p8));
    r5 += 12;
    m.W32(r6, B(p9x));
    m.W32(r6 + 4, B(p9y));
    m.W32(r6 + 8, B(p10));
    r6 += 12;
    m.W32(r4, B(p11x));
    m.W32(r4 + 4, B(p11y));
    m.W32(r4 + 8, B(p12));
    r4 += 12;
  } while (--ctr != 0);

  g[3] = r3;
  g[4] = r4;
  g[5] = r5;
  g[6] = r6;
  g[7] = r7;
  g[8] = r8;
  g[9] = r9;
  CTR(s) = 0;
  // The scalar results, in both halves as the console leaves them.
  s.ps[4].Fill(double(f4));
  s.ps[5].Fill(double(f5));
  s.ps[6].Fill(double(f6));
  s.ps[13].Fill(double(f13));
  s.ps[27].Fill(double(f27));
  s.ps[29].Fill(double(f29));
  s.ps[30].Fill(double(f30));
  s.npc = end;
}

template <bool Morph>
void RunSkin(const Core::CPUThreadGuard& guard, std::size_t routine)
{
  LogFirst(routine);
  Core::System& system = guard.GetSystem();
  PowerPC::PowerPCState& s = system.GetPPCState();
  const u32* const g = s.gpr;
  const s32 clear_at = s_clear_at[Morph ? 1 : 0];
  const u64 count = s32(g[3]) > 0 ? g[3] : 0;
  const bool fast =
      Plain(system, g[1] - 16, 16) && Plain(system, g[2] + u32(clear_at), 4) &&
      (count == 0 ||
       (Plain(system, g[4], 256 * 64) && Plain(system, g[5], count * 24 + 0x10 + 256) &&
        Plain(system, g[6], count * 12) && Plain(system, g[7], count * 12) &&
        (!Morph || Plain(system, g[8], count * 12)) && Plain(system, g[9], count * 12) &&
        Plain(system, g[10], count * 12)));
  if (fast && count > 0 && s_avx)
  {
    // The outputs overlap nothing read: the stack frame, the constant, the
    // matrices, the records, the inputs, or each other.
    const u64 out = count * 12;
    const u32 o1 = g[7], o2 = g[10];
    bool apart = Disjoint(o1, out, o2, out);
    for (const u32 o : {o1, o2})
    {
      apart = apart && Disjoint(o, out, g[1] - 16, 16) && Disjoint(o, out, g[2] + u32(clear_at), 4) &&
              Disjoint(o, out, g[4], 256 * 64) && Disjoint(o, out, g[5], count * 24 + 0x10 + 256) &&
              Disjoint(o, out, g[6], count * 12) && Disjoint(o, out, g[9], count * 12) &&
              (!Morph || Disjoint(o, out, g[8], count * 12));
    }
    if (apart)
    {
      SkinBatch<Morph>(FastOf(system), s, clear_at);
      return;
    }
  }
  if (fast)
    Skin<Morph>(FastOf(system), s, clear_at);
  else
    Skin<Morph>(Slow{system.GetMMU()}, s, clear_at);
}
}  // namespace

void SkinAccumulate(const Core::CPUThreadGuard& guard)
{
  RunSkin<false>(guard, 0);
}

void SkinMorphAccumulate(const Core::CPUThreadGuard& guard)
{
  RunSkin<true>(guard, 1);
}

void OffsetShells(const Core::CPUThreadGuard& guard)
{
  LogFirst(2);
  Core::System& system = guard.GetSystem();
  PowerPC::PowerPCState& s = system.GetPPCState();
  const u32* const g = s.gpr;
  if (s32(g[3]) <= 0)
  {
    // Not reached from the game's own code (see Shells); as its ble- would.
    CTR(s) = g[3];
    s.npc = s_shells_end;
    return;
  }
  const u64 count = g[3];
  const bool fast = Plain(system, g[1] + u32(s_shells_scratch), 8) && Plain(system, g[15], 12) &&
                    Plain(system, g[9], count * 4) && Plain(system, g[8], count * 12) &&
                    Plain(system, g[7], count * 12) && Plain(system, g[5], count * 12) &&
                    Plain(system, g[6], count * 12) && Plain(system, g[4], count * 12);
  if (fast)
    Shells(FastOf(system), s, s_shells_end, s_shells_scratch);
  else
    Shells(Slow{system.GetMMU()}, s, s_shells_end, s_shells_scratch);
}

void Apply(Core::System& system, const Core::CPUThreadGuard& guard,
           const DiscIO::VolumeDisc& volume)
{
  Reset();
  s_avx = cpu_info.bAVX && cpu_info.bFMA;
  const std::vector<Section> sections = TextSections(volume);
  if (sections.empty())
    return;
  // The routines round as the JIT does with FMA and without Exact Multiply-Add.
  if (!Config::Get(Config::SESSION_USE_FMA) || Config::Get(Config::MAIN_ACCURATE_FMADDS))
  {
    Repatch(system);
    return;
  }
  for (std::size_t r = 0; r < std::size(kRoutines); ++r)
  {
    const Routine& routine = kRoutines[r];
    const std::vector<Match> found = Find(guard, sections, routine);
    if (found.empty())
      continue;
    // Every copy must use the same displacements: a hook doesn't know which
    // copy called it.
    bool same = true;
    for (const Match& match : found)
      same = same && std::equal(std::begin(match.displacement), std::end(match.displacement),
                                std::begin(found[0].displacement));
    if (!same)
      continue;
    const s16* const d = found[0].displacement;
    if (routine.words == kOffsetShells)
    {
      // Hooked mid-function, so it must know where it continues: one copy only.
      // The byte's doubleword: the high word and the low one just after it,
      // read back from the same place.
      if (found.size() != 1 || d[1] != d[0] + 4 || d[2] != d[0])
        continue;
      s_shells_end = found[0].address + routine.count * 4;
      s_shells_scratch = d[0];
    }
    else
    {
      s_clear_at[routine.words == kSkinMorphAccumulate ? 1 : 0] = d[0];
    }
    for (const Match& match : found)
    {
      s_hooked.push_back({match.address, routine.hook});
      std::fprintf(stderr, "native: %s found at %08x\n", routine.what, match.address);
    }
  }
  Repatch(system);
}

void Repatch(Core::System& system)
{
  for (const Hooked& hooked : s_hooked)
    HLE::Patch(system, hooked.address, hooked.hook);
}

void Reset()
{
  s_hooked.clear();
  s_shells_end = 0;
  s_clear_at[0] = s_clear_at[1] = -0x6158;
  s_shells_scratch = 0x50;
  for (auto& logged : s_logged)
    logged = false;
}
}  // namespace HLE_PorpoiseNative
