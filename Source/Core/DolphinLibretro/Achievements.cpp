// Copyright 2026 Ruben (Project Porpoise)
// SPDX-License-Identifier: GPL-2.0-or-later

// Porpoise: RetroAchievements (softcore) under the host. The host hands over
// the account and its network and popup functions before each game
// (porpoise_ra_setup); the core signs in with the token as the game starts.

#include "DolphinLibretro/Achievements.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef USE_RETRO_ACHIEVEMENTS
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/AchievementHooks.h"
#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#endif

namespace
{
struct Setup
{
  bool given = false;
  std::string username, token;
};
Setup s_setup;
}  // namespace

extern "C" {
struct porpoise_ra_setup_t
{
  unsigned version;  // 1
  const char* username;
  const char* token;
  int (*request)(const char* url, const char* post_data, void* ctx,
                 void (*sink)(void* ctx, const char* data, size_t size));
  void (*event)(int kind, const char* title, const char* text, const char* image_url, int a, int b);
};

// Before retro_load_game. A null or empty account: no achievements this game.
__attribute__((visibility("default"))) int porpoise_ra_setup(const porpoise_ra_setup_t* setup)
{
#ifdef USE_RETRO_ACHIEVEMENTS
  s_setup = {};
  AchievementHooks::request = nullptr;
  AchievementHooks::event = nullptr;
  if (!setup || setup->version != 1 || !setup->username || !setup->token || !*setup->username ||
      !*setup->token || !setup->request)
    return 0;
  s_setup.given = true;
  s_setup.username = setup->username;
  s_setup.token = setup->token;
  AchievementHooks::request = setup->request;
  AchievementHooks::event = setup->event;
  return 1;
#else
  (void)setup;
  return 0;
#endif
}

// The running game's achievements, as text Porpoise reads (its in-game menu
// and the list it keeps for Details): a line "G" for the game, then one "A"
// per achievement, fields separated by tabs. Returns the size the whole text
// needs (with its NUL; copied in when it fits), or -1 when no game's set is
// loaded.
//   G  id  title  badge_url  unlocked  total  points_unlocked  points_total
//   A  id  points  unlocked(0/1)  unlock_time  type  rarity  measured_progress
//      title  description  badge_url  badge_locked_url
__attribute__((visibility("default"))) int porpoise_ra_list(char* buf, int size)
{
#ifdef USE_RETRO_ACHIEVEMENTS
  auto& manager = AchievementManager::GetInstance();
  std::lock_guard lg{manager.GetLock()};
  rc_client_t* client = manager.GetClient();
  const rc_client_game_t* game = client ? rc_client_get_game_info(client) : nullptr;
  if (!game || game->id == 0)
    return -1;
  auto clean = [](const char* s) {
    std::string out = s ? s : "";
    for (char& c : out)
      if (c == '\t' || c == '\n' || c == '\r')
        c = ' ';
    return out;
  };
  rc_client_user_game_summary_t summary{};
  rc_client_get_user_game_summary(client, &summary);
  char game_badge[512] = "";
  if (rc_client_game_get_image_url(game, game_badge, sizeof game_badge) != RC_OK)
    game_badge[0] = '\0';
  std::string out = "G\t" + std::to_string(game->id) + "\t" + clean(game->title) + "\t" + game_badge + "\t" +
                    std::to_string(summary.num_unlocked_achievements) + "\t" +
                    std::to_string(summary.num_core_achievements) + "\t" +
                    std::to_string(summary.points_unlocked) + "\t" + std::to_string(summary.points_core) +
                    "\n";
  if (rc_client_achievement_list_t* list = rc_client_create_achievement_list(
          client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE))
  {
    for (uint32_t b = 0; b < list->num_buckets; ++b)
      for (uint32_t i = 0; i < list->buckets[b].num_achievements; ++i)
      {
        const rc_client_achievement_t* a = list->buckets[b].achievements[i];
        char rarity[16];
        std::snprintf(rarity, sizeof rarity, "%.1f", double(a->rarity));
        out += "A\t" + std::to_string(a->id) + "\t" + std::to_string(a->points) + "\t" +
               (a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED ? "1" : "0") + "\t" +
               std::to_string(static_cast<long long>(a->unlock_time)) + "\t" + std::to_string(a->type) +
               "\t" + rarity + "\t" + clean(a->measured_progress) + "\t" + clean(a->title) + "\t" +
               clean(a->description) + "\t" + clean(a->badge_url) + "\t" + clean(a->badge_locked_url) +
               "\n";
      }
    rc_client_destroy_achievement_list(list);
  }
  const int need = static_cast<int>(out.size()) + 1;
  if (buf && size >= need)
    std::memcpy(buf, out.c_str(), static_cast<size_t>(need));
  return need;
#else
  (void)buf;
  (void)size;
  return -1;
#endif
}

// Requests still queued or being sent (an unlock on its way).
__attribute__((visibility("default"))) int porpoise_ra_pending()
{
#ifdef USE_RETRO_ACHIEVEMENTS
  return AchievementHooks::pending.load();
#else
  return 0;
#endif
}
}

namespace Libretro::Achievements
{
void Start(const std::string& game_path)
{
#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().Shutdown();  // one left by a game that didn't boot
  AchievementHooks::boot_path = game_path;
  {
    std::lock_guard lock{AchievementHooks::message_lock};
    AchievementHooks::last_message.clear();
  }
  // rcheevos' own account of each step goes to core.log (it is off by default).
  if (s_setup.given)
    Common::Log::LogManager::GetInstance()->SetEnable(Common::Log::LogType::ACHIEVEMENTS, true);
  // The current layer: nothing here is written back to RetroAchievements.ini.
  Config::SetCurrent(Config::RA_ENABLED, s_setup.given);
  Config::SetCurrent(Config::RA_HARDCORE_ENABLED, false);  // softcore only
  Config::SetCurrent(Config::RA_UNOFFICIAL_ENABLED, false);
  Config::SetCurrent(Config::RA_ENCORE_ENABLED, false);
  Config::SetCurrent(Config::RA_SPECTATOR_ENABLED, false);
  Config::SetCurrent(Config::RA_DISCORD_PRESENCE_ENABLED, false);
  Config::SetCurrent(Config::RA_USERNAME, s_setup.given ? s_setup.username : std::string());
  Config::SetCurrent(Config::RA_API_TOKEN, s_setup.given ? s_setup.token : std::string());
  if (s_setup.given)
    AchievementManager::GetInstance().Init(nullptr);
#endif
}

void Stop()
{
#ifdef USE_RETRO_ACHIEVEMENTS
  AchievementManager::GetInstance().Shutdown();
  AchievementHooks::boot_path.clear();
  AchievementHooks::request = nullptr;
  AchievementHooks::event = nullptr;
  s_setup = {};
#endif
}
}  // namespace Libretro::Achievements
