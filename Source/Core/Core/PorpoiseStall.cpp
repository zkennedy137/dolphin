// Porpoise's stall report.
// Copyright (C) 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PorpoiseStall.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "Common/GekkoDisassembler.h"
#include "Core/HW/DSP.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Channel.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "VideoCommon/CommandProcessor.h"

namespace PorpoiseStall
{
namespace
{
constexpr u64 kStallUs = 4'000'000;
constexpr u64 kAgainUs = 10'000'000;
constexpr int kReports = 3;

MmuCounts s_mmu;
u64 s_mmu_us = 0;

u32 s_write_pointer = 0;
u64 s_changed_us = 0;
u64 s_field_us = 0;
u64 s_reported_us = 0;
int s_reports = 0;
std::vector<u32> s_pcs;

void Report(Core::System& system, u64 stalled_us)
{
  const auto& ppc = system.GetPPCState();
  auto& pi = system.GetProcessorInterface();
  std::string pcs;
  for (const u32 pc : s_pcs)
    pcs += fmt::format(" {:08x}", pc);
  std::fprintf(stderr,
               "core: stall: no graphics from the game for %.1f s; pc %08x lr %08x msr %08x (EE %u), "
               "exceptions %08x; pi cause %08x mask %08x; pcs%s\n",
               double(stalled_us) / 1e6, ppc.pc, LR(ppc), ppc.msr.Hex, (ppc.msr.Hex >> 15) & 1,
               ppc.Exceptions, pi.GetCause(), pi.GetMask(), pcs.c_str());

  auto& fifo = system.GetCommandProcessor().GetFifo();
  std::fprintf(stderr,
               "core: stall: fifo base %08x end %08x read %08x write %08x distance %08x "
               "breakpoint %08x; read %u link %u breakpoint on %u hit %u; hi %u lo %u\n",
               fifo.CPBase.load(), fifo.CPEnd.load(), fifo.CPReadPointer.load(),
               fifo.CPWritePointer.load(), fifo.CPReadWriteDistance.load(),
               fifo.CPBreakpoint.load(), fifo.bFF_GPReadEnable.load(), fifo.bFF_GPLinkEnable.load(),
               fifo.bFF_BPEnable.load(), fifo.bFF_Breakpoint.load(), fifo.bFF_HiWatermark.load(),
               fifo.bFF_LoWatermark.load());

  std::fprintf(stderr, "core: stall: %s\n", system.GetDVDInterface().PorpoiseState().c_str());
  std::fprintf(stderr, "core: stall: %s\n", system.GetDSP().PorpoiseState().c_str());
  for (u32 c = 0; c < 3; ++c)
  {
    if (auto* channel = system.GetExpansionInterface().GetChannel(c))
      std::fprintf(stderr, "core: stall: exi %u %s\n", c, channel->PorpoiseState().c_str());
  }

  auto& memory = system.GetMemory();
  const u32 mem1 = memory.GetRamSizeReal();
  const u32 mem2 = memory.GetExRamSizeReal();
  const auto ok = [&](u32 a) {
    return (a >= 0x80000000 && a - 0x80000000 + 4 <= mem1) ||
           (mem2 && a >= 0x90000000 && a - 0x90000000 + 4 <= mem2);
  };
  const auto rd = [&](u32 a) -> u32 { return ok(a) ? memory.Read_U32(a & 0x3FFFFFFF) : 0; };

  // The code the game is in (where it loops), and where it was called from.
  for (const u32 around : {ppc.pc, LR(ppc)})
  {
    if (!ok(around))
      continue;
    for (u32 a = around - 0x20; a <= around + 0x10; a += 4)
    {
      if (!ok(a))
        continue;
      const u32 op = rd(a);
      std::fprintf(stderr, "core: stall: code %08x%s %08x  %s\n", a, a == around ? " >" : "  ", op,
                   Common::GekkoDisassembler::Disassemble(op, a).c_str());
    }
  }

  // A game that stopped itself: the SDK's PPCHalt (sync; nop; li r3, 0; nop; b),
  // where OSPanic ends after printing why. The registers, and any text the
  // registers or the stack point at: the panic's file, line and message.
  if (rd(ppc.pc) == 0x60000000 && rd(ppc.pc - 4) == 0x7C0004AC && rd(ppc.pc + 4) == 0x38600000 &&
      rd(ppc.pc + 12) == 0x4BFFFFF4)
  {
    std::fprintf(stderr, "core: stall: the game halted itself (PPCHalt: an OSPanic, a fatal error "
                         "of its own)\n");
    std::string regs;
    for (int i = 0; i < 32; ++i)
      regs += fmt::format(" r{}={:08x}", i, ppc.gpr[i]);
    std::fprintf(stderr, "core: stall: registers%s\n", regs.c_str());
    // The last exception the game took (a page fault it couldn't serve, for one).
    std::fprintf(stderr,
                 "core: stall: last exception: srr0 %08x srr1 %08x dar %08x dsisr %08x\n",
                 ppc.spr[SPR_SRR0], ppc.spr[SPR_SRR1], ppc.spr[SPR_DAR], ppc.spr[SPR_DSISR]);
    std::vector<u32> seen;
    const auto text = [&](u32 a) {
      if (!ok(a) || seen.size() >= 24 || std::find(seen.begin(), seen.end(), a) != seen.end())
        return;
      std::string out;
      for (u32 i = 0; i < 160 && ok(a + i); ++i)
      {
        const u8 c = memory.Read_U8((a + i) & 0x3FFFFFFF);
        if (c == 0)
          break;
        if (c == '\n')
        {
          out += "\\n";
          continue;
        }
        if (c < 0x20 || c > 0x7E)
          return;
        out += char(c);
      }
      if (out.size() < 4)
        return;
      seen.push_back(a);
      std::fprintf(stderr, "core: stall: text at %08x: \"%s\"\n", a, out.c_str());
    };
    for (int i = 0; i < 32; ++i)
      text(ppc.gpr[i]);
    for (u32 a = ppc.gpr[1]; a < ppc.gpr[1] + 0x400; a += 4)
      text(rd(a));
  }

  // The game's threads (the SDK's OSThread list): what each is doing and
  // where it waits.
  const u32 current = rd(0x800000E4);
  u32 t = rd(0x800000DC);
  for (int n = 0; n < 24 && ok(t) && ok(t + 0x308); ++n)
  {
    const u32 sp = rd(t + 0x04);
    std::string bt;
    u32 frame = rd(sp);
    for (int d = 0; d < 8 && ok(frame) && ok(frame + 4); ++d)
    {
      bt += fmt::format(" {:08x}", rd(frame + 4));
      const u32 next = rd(frame);
      if (next <= frame)
        break;
      frame = next;
    }
    std::fprintf(stderr,
                 "core: stall: thread %08x%s state %u prio %d suspend %d waits on %08x mutex %08x "
                 "pc %08x lr %08x; calls%s\n",
                 t, t == current ? " (running)" : "", rd(t + 0x2C8) >> 16,
                 static_cast<s32>(rd(t + 0x2D0)), static_cast<s32>(rd(t + 0x2CC)), rd(t + 0x2DC),
                 rd(t + 0x2F0), rd(t + 0x198), rd(t + 0x84), bt.c_str());
    t = rd(t + 0x2FC);
  }
  std::fflush(stderr);
}
}  // namespace

MmuCounts& Mmu()
{
  return s_mmu;
}

void Field(Core::System& system)
{
  const u64 now = Common::Timer::NowUs();
  if (!s_mmu_us)
    s_mmu_us = now;
  if (now - s_mmu_us >= 5'000'000)
  {
    if (s_mmu.rescans || s_mmu.mapped || s_mmu.unmappable || s_mmu.faults)
    {
      std::fprintf(stderr,
                   "mmu: %.0f s: page table rescans %llu (%.1f ms), pages mapped %llu, pages that "
                   "couldn't be (the host's larger pages) %llu, fastmem faults %llu\n",
                   double(now - s_mmu_us) / 1e6, (unsigned long long)s_mmu.rescans,
                   double(s_mmu.rescan_us) / 1e3, (unsigned long long)s_mmu.mapped,
                   (unsigned long long)s_mmu.unmappable, (unsigned long long)s_mmu.faults);
    }
    s_mmu = {};
    s_mmu_us = now;
  }
  // A gap between fields is the emulation paused (the menu): not the game's time.
  if (s_field_us && now - s_field_us > 500'000)
    s_changed_us += now - s_field_us;
  s_field_us = now;

  const u32 write = system.GetCommandProcessor().GetFifo().CPWritePointer.load();
  if (write != s_write_pointer || !s_changed_us)
  {
    s_write_pointer = write;
    s_changed_us = now;
    s_reports = 0;
    s_pcs.clear();
    return;
  }
  const u32 pc = system.GetPPCState().pc;
  if (s_pcs.size() < 12 && std::find(s_pcs.begin(), s_pcs.end(), pc) == s_pcs.end())
    s_pcs.push_back(pc);
  if (now - s_changed_us < kStallUs || s_reports >= kReports ||
      (s_reports && now - s_reported_us < kAgainUs))
    return;
  ++s_reports;
  s_reported_us = now;
  Report(system, now - s_changed_us);
}
}  // namespace PorpoiseStall
