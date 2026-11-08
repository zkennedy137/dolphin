// Porpoise's native routines.
// Copyright (C) 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A few hot game routines, found by their exact machine code when the game
// boots, run as native code instead of through the JIT. Each gives the same
// memory and the same registers the game's own code would (the same rounding,
// step by step, as Dolphin's JIT), only faster. Nothing is hooked for a game
// that doesn't contain the code word for word.

#pragma once

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace DiscIO
{
class VolumeDisc;
}

namespace HLE_PorpoiseNative
{
// After the game's program is in memory: finds the routines and hooks them.
void Apply(Core::System& system, const Core::CPUThreadGuard& guard,
           const DiscIO::VolumeDisc& volume);

// Installs the hooks found by Apply again (HLE::PatchFixedFunctions).
void Repatch(Core::System& system);

// Forgets the game (HLE::Clear).
void Reset();

// The hooks.
void SkinAccumulate(const Core::CPUThreadGuard& guard);
void SkinMorphAccumulate(const Core::CPUThreadGuard& guard);
void OffsetShells(const Core::CPUThreadGuard& guard);
}  // namespace HLE_PorpoiseNative
