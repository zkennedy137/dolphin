// Porpoise's stall report.
// Copyright (C) 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// When a game stops sending graphics for several seconds while the console
// keeps running (a freeze that isn't a crash), the trace gets what the game
// was waiting on: where its code is, its threads, and the disc drive, memory
// card, graphics FIFO and interrupt state. A few reports a stall, no more.

#pragma once

namespace Core
{
class System;
}

#include "Common/CommonTypes.h"

namespace PorpoiseStall
{
// Every video field, on the CPU thread.
void Field(Core::System& system);

// What a game's virtual memory (the MMU's page table) costs, counted on the CPU
// thread and written to the trace every 5 s while there is any.
struct MmuCounts
{
  u64 rescans = 0, rescan_us = 0;
  u64 mapped = 0, unmappable = 0;
  u64 faults = 0;
};
MmuCounts& Mmu();
}  // namespace PorpoiseStall
