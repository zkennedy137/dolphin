// Copyright 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace Libretro::Achievements
{
// As the game boots (after the config is read; game_path: the file booted)
// and after it has stopped.
void Start(const std::string& game_path);
void Stop();
}  // namespace Libretro::Achievements
