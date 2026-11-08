// Copyright 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later

// Porpoise (libretro): RetroAchievements with the host doing the network and
// the popups. The host fills these in (DolphinLibretro/Achievements.cpp,
// porpoise_ra_setup) before the game loads; AchievementManager calls them.

#pragma once

#ifdef USE_RETRO_ACHIEVEMENTS

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

namespace AchievementHooks
{
// One answer's bytes, as they come.
using Sink = void (*)(void* ctx, const char* data, size_t size);
// A GET (post_data null) or a form POST. Returns the HTTP status, or -1 when
// there was no answer at all.
using RequestFn = int (*)(const char* url, const char* post_data, void* ctx, Sink sink);
// Something for the player to see (Kind), with up to two numbers.
using EventFn = void (*)(int kind, const char* title, const char* text, const char* image_url, int a,
                         int b);

enum Kind
{
  LoginOk = 1,         // title: the player's name; a: softcore points
  LoginFailed = 2,     // a: rcheevos' result
  GameLoaded = 3,      // title: the game; a: unlocked, b: total
  Unlocked = 4,        // title, text: the achievement; a: its points
  Completed = 5,       // title: the game
  NoAchievements = 6,  // RetroAchievements doesn't know this game (or this version of it)
  ServerError = 7,     // title: the API call; text: the server's message
};

extern RequestFn request;
extern EventFn event;
// The game's file as the boot opened it: the set is identified from a fresh
// reader on it (a copy of the boot's own reader is the fallback).
extern std::string boot_path;
// rcheevos' last message, for a load that fails: the popup says why.
extern std::string last_message;
extern std::mutex message_lock;  // last_message's
// Requests queued or under way, so the host can wait for an unlock still
// being sent before it closes the game.
extern std::atomic<int> pending;

inline void Send(int kind, const char* title, const char* text = "", const char* image_url = "",
                 int a = 0, int b = 0)
{
  if (event)
    event(kind, title ? title : "", text ? text : "", image_url ? image_url : "", a, b);
}
}  // namespace AchievementHooks

#endif  // USE_RETRO_ACHIEVEMENTS
