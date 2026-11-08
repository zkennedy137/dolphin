// Porpoise's universal widescreen (beta).
// Copyright (C) 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_PorpoiseWidescreen.h"

#include <array>
#include <atomic>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/HLE/HLE.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/System.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "VideoCommon/VideoConfig.h"

namespace HLE_PorpoiseWidescreen
{
namespace
{
constexpr u32 BLR = 0x4E800020;
// C_MTXFrustum's first instruction: fsubs f9, f4, f3 (r - l).
constexpr u32 FRUSTUM_FIRST = 0xED241828;
// 4:3 to 16:9.
constexpr double WIDEN = 4.0 / 3.0;

struct Signature
{
  u32 hash;
  u32 size;  // bytes
};

// From Dolphin's Data/Sys/totaldb.dsy (two builds of the SDK each).
constexpr std::array<Signature, 2> PERSPECTIVE{{{0x2f85f228, 236}, {0xd2bd4590, 208}}};
constexpr std::array<Signature, 2> ORTHO{{{0xc76f95b2, 152}, {0xcdcb1aa9, 152}}};

bool s_enabled = false;
u32 s_perspective = 0;
u32 s_frustum = 0;
std::atomic<bool> s_logged_perspective{false};
std::atomic<bool> s_logged_frustum{false};

struct Section
{
  u32 start;
  u32 end;  // exclusive
};

// The main program's text sections, from its DOL header on the disc.
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

u32 Read(const Core::CPUThreadGuard& guard, u32 address)
{
  return PowerPC::MMU::HostRead_Instruction(guard, address);
}

// A function with one of the signatures: it starts just after a blr (or at the
// section's start) and its last instruction is a blr.
template <std::size_t N>
u32 FindKnown(const Core::CPUThreadGuard& guard, const std::vector<Section>& sections,
              const std::array<Signature, N>& known)
{
  for (const Section& section : sections)
  {
    for (u32 address = section.start; address + 8 <= section.end; address += 4)
    {
      if (address != section.start && Read(guard, address - 4) != BLR)
        continue;
      for (const Signature& signature : known)
      {
        const u32 last = address + signature.size - 4;
        if (last >= section.end || Read(guard, last) != BLR)
          continue;
        if (HashSignatureDB::ComputeCodeChecksum(guard, address, last) == signature.hash)
          return address;
      }
    }
  }
  return 0;
}

// C_MTXFrustum sits just before C_MTXPerspective in mtx44.o: the function that
// ends at the blr before it, when it starts with C_MTXFrustum's first
// instruction.
u32 FindFrustumBefore(const Core::CPUThreadGuard& guard, u32 next, u32 start)
{
  if (next < start + 8 || Read(guard, next - 4) != BLR)
    return 0;
  const u32 limit = next - start > 0x200 ? next - 0x200 : start;
  for (u32 address = next - 8; address >= limit; address -= 4)
  {
    if (Read(guard, address) == BLR)
      return Read(guard, address + 4) == FRUSTUM_FIRST ? address + 4 : 0;
    if (address == start)
      return Read(guard, address) == FRUSTUM_FIRST ? address : 0;
    if (address < 4)
      break;
  }
  return 0;
}

// A 4:3-ish aspect, widened to 16:9; anything else (a square shadow map, a
// game that is wide already) is left alone.
std::optional<double> Widened(double aspect)
{
  const double magnitude = std::fabs(aspect);
  if (!std::isfinite(magnitude) || magnitude < 1.2 || magnitude > 1.6)
    return std::nullopt;
  return aspect * WIDEN;
}
}  // namespace

void SetEnabled(bool enabled)
{
  s_enabled = enabled;
}

namespace
{
// Where the game's own projection can't be widened: Dolphin's widescreen hack
// instead, so the game still plays in 16:9 (its HUD stretched, things at the
// edges perhaps popping in, as with the hack chosen outright).
bool UseHack(const char* why)
{
  NOTICE_LOG_FMT(BOOT, "Universal widescreen: {}; Dolphin's widescreen hack instead", why);
  Config::SetCurrent(Config::GFX_WIDESCREEN_HACK, true);
  return false;
}
}  // namespace

bool Apply(Core::System& system, const Core::CPUThreadGuard& guard,
           const DiscIO::VolumeDisc& volume)
{
  Reset();
  if (!s_enabled)
    return false;
  if (system.IsWii() || volume.GetVolumeType() != DiscIO::Platform::GameCubeDisc)
    return UseHack("a Wii game (GameCube games' projections only)");

  const std::vector<Section> sections = TextSections(volume);
  if (sections.empty())
    return UseHack("the program's sections weren't readable");

  u32 perspective = FindKnown(guard, sections, PERSPECTIVE);
  u32 frustum = 0;
  if (perspective)
  {
    for (const Section& section : sections)
      if (perspective >= section.start && perspective < section.end)
        frustum = FindFrustumBefore(guard, perspective, section.start);
  }
  const u32 ortho = FindKnown(guard, sections, ORTHO);

  if (!perspective && !frustum)
  {
    NOTICE_LOG_FMT(BOOT, "Universal widescreen: no known projection function (ortho {:08x})", ortho);
    return UseHack("the game's own projection function wasn't found");
  }

  s_perspective = perspective;
  s_frustum = frustum;
  NOTICE_LOG_FMT(BOOT,
                 "Universal widescreen: C_MTXPerspective {:08x}, C_MTXFrustum {:08x}, "
                 "C_MTXOrtho {:08x}",
                 perspective, frustum, ortho);
  Repatch(system);

  Config::SetCurrent(Config::GFX_ASPECT_RATIO, AspectMode::ForceWide);
  Config::SetCurrent(Config::GFX_WIDESCREEN_HACK, false);
  return true;
}

void Repatch(Core::System& system)
{
  if (s_perspective)
    HLE::Patch(system, s_perspective, "PorpoiseWidePerspective");
  if (s_frustum)
    HLE::Patch(system, s_frustum, "PorpoiseWideFrustum");
}

void Reset()
{
  s_perspective = 0;
  s_frustum = 0;
  s_logged_perspective = false;
  s_logged_frustum = false;
}

// C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f): aspect in f2.
void Perspective(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  const double aspect = ppc_state.ps[2].PS0AsDouble();
  const std::optional<double> wide = Widened(aspect);
  if (!wide)
    return;
  ppc_state.ps[2].Fill(double(float(*wide)));
  if (!s_logged_perspective.exchange(true))
    NOTICE_LOG_FMT(BOOT, "Universal widescreen: perspective {:.3f} widened to {:.3f}", aspect,
                   *wide);
}

// C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f): l and r in
// f3 and f4, widened about their middle.
void Frustum(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  const double t = ppc_state.ps[1].PS0AsDouble();
  const double b = ppc_state.ps[2].PS0AsDouble();
  const double l = ppc_state.ps[3].PS0AsDouble();
  const double r = ppc_state.ps[4].PS0AsDouble();
  if (t == b)
    return;
  if (!Widened((r - l) / (t - b)))
    return;
  const double middle = (l + r) / 2;
  const double new_l = middle + (l - middle) * WIDEN;
  const double new_r = middle + (r - middle) * WIDEN;
  ppc_state.ps[3].Fill(double(float(new_l)));
  ppc_state.ps[4].Fill(double(float(new_r)));
  if (!s_logged_frustum.exchange(true))
    NOTICE_LOG_FMT(BOOT, "Universal widescreen: frustum {:.3f}..{:.3f} widened to {:.3f}..{:.3f}",
                   l, r, new_l, new_r);
}
}  // namespace HLE_PorpoiseWidescreen
