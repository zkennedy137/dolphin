// Porpoise's universal widescreen (beta).
// Copyright (C) 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// For a GameCube game without a widescreen code of its own: find the SDK's
// projection functions (C_MTXPerspective and C_MTXFrustum, from mtx44.o) in
// the game's main program, by the checksums in Dolphin's own signature
// database, and hook them so a 4:3 projection is widened to 16:9. When they
// can't be found, nothing is hooked and the game stays 4:3.

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

namespace HLE_PorpoiseWidescreen
{
// Chosen for the game about to boot (the frontend's "Universal (Beta)").
void SetEnabled(bool enabled);

// After the game's program is in memory: finds the functions and hooks them.
// true when the game is widened.
bool Apply(Core::System& system, const Core::CPUThreadGuard& guard,
           const DiscIO::VolumeDisc& volume);

// Installs the hooks found by Apply again (HLE::PatchFixedFunctions).
void Repatch(Core::System& system);

// Forgets the game (HLE::Clear).
void Reset();

// The hooks, run at the start of each function.
void Perspective(const Core::CPUThreadGuard& guard);
void Frustum(const Core::CPUThreadGuard& guard);
}  // namespace HLE_PorpoiseWidescreen
