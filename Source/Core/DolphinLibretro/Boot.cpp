#include <cstdio>
#include "Common/CPUDetect.h"
#include <libretro.h>
#include <string>
#include <functional>
#include <filesystem>
#include <fstream>

#include "Common/CommonPaths.h"
#include "Core/HLE/HLE_PorpoiseWidescreen.h"
#include "Core/CommonTitles.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/HSP/HSP_Device.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/WiiUtils.h"
#include "Core/SysConf.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/ES/Formats.h"
#include "Common/IOFile.h"
#include "Common/NandPaths.h"
#include "DiscIO/Enums.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "DolphinLibretro/Achievements.h"
#include "DolphinLibretro/Audio.h"
#include "DolphinLibretro/Input.h"
#include "DolphinLibretro/Log.h"
#include "DolphinLibretro/Common/VFile.h"
#include "DolphinLibretro/Common/Globals.h"
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/Video.h"
#include "DolphinLibretro/VideoContexts/ContextStatus.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "UICommon/DiscordPresence.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/Resources/CustomResourceManager.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace fs = std::filesystem;

namespace Libretro
{
extern retro_environment_t environ_cb;

// Disk swapping
static void InitDiskControlInterface();
static unsigned disk_index = 0;
static bool eject_state;
static std::vector<std::string> disk_paths;

static void ResetDiskControlState()
{
  disk_index = 0;
  eject_state = false;
  disk_paths.clear();
}

// GBPlayer
static std::string GBPlayer_rom_path;
static bool GBPlayer_active;

// silence forward declaration warnings
void reload_cheats_from_ini();
void generate_cht_from_ini(std::string fileName);

void reload_cheats_from_ini()
{
  // Load Dolphin INI files
  const auto& sconfig = SConfig::GetInstance();
  Common::IniFile globalIni = sconfig.LoadDefaultGameIni();
  Common::IniFile localIni = sconfig.LoadLocalGameIni();

  // Load ActionReplay codes
  Libretro::g_ar_codes = ActionReplay::LoadCodes(globalIni, localIni);

  // Load Gecko codes
  Libretro::g_gecko_codes = Gecko::LoadCodes(globalIni, localIni);
}

void generate_cht_from_ini(std::string fileName)
{
  const char* sysdir = nullptr;

  if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sysdir) || !sysdir)
    return;

  std::string base = sysdir;
  size_t pos = base.find_last_of("/\\");
  if (pos != std::string::npos)
      base = base.substr(0, pos);

  // Write RetroArch .cht
  std::string cheatDir = base + "/cheats/dolphin-emu/";
  std::string cheatFile = cheatDir + fileName + ".cht";

  if (File::Exists(cheatFile))
  {
    INFO_LOG_FMT(BOOT, "Cheats: pre-existing cht file found: {}", cheatFile);
    return;
  }

  size_t total = Libretro::g_ar_codes.size() + Libretro::g_gecko_codes.size();
  if (total == 0)
  {
    INFO_LOG_FMT(BOOT, "Cheats: not generating cht file as no ActionReply or Gecko codes found");
    return;
  }

  if (!File::CreateFullPath(cheatFile))
    return;

  std::ofstream out(cheatFile);
  if (!out.is_open())
  {
    ERROR_LOG_FMT(BOOT, "Cheats: unable to create a new cht file at: {}", cheatFile);
    return;
  }

  out << "cheats = " << total << "\n\n";

  size_t cheat_index = 0;

  // -------------------------
  // Action Replay
  // -------------------------
  for (const auto& c : Libretro::g_ar_codes)
  {
    std::string code;

    for (const auto& op : c.ops)
    {
      if (!code.empty())
        code += "+";

      code += fmt::format("{:08X} {:08X}", op.cmd_addr, op.value);
    }

    out << "cheat" << cheat_index << "_desc = \"" << c.name << "\"\n";
    out << "cheat" << cheat_index << "_code = \"" << code << "\"\n";
    out << "cheat" << cheat_index << "_enable = " << (c.enabled ? "true" : "false") << "\n\n";

    cheat_index++;
  }

  // -------------------------
  // Gecko
  // -------------------------
  for (const auto& c : Libretro::g_gecko_codes)
  {
    std::string code;

    for (const auto& line : c.codes)
    {
      if (!code.empty())
          code += "+";

      code += line.original_line;
    }

    out << "cheat" << cheat_index << "_desc = \"" << c.name << "\"\n";
    out << "cheat" << cheat_index << "_code = \"" << code << "\"\n";
    out << "cheat" << cheat_index << "_enable = " << (c.enabled ? "true" : "false") << "\n\n";

    cheat_index++;
  }

  out.close();

  INFO_LOG_FMT(BOOT, "Cheats: cht file created successfully at: {}", cheatFile);
}

} // namespace Libretro

bool retro_load_game(const struct retro_game_info* game)
{
  Libretro::ResetDiskControlState();

  const char* save_dir = NULL;
  const char* gba_save_dir = NULL;
  const char* system_dir = NULL;
  const char* core_assets_dir = NULL;
  std::string rebuild_save_dir;
  std::string user_dir;
  std::string sys_dir;

  /*
   *  If the GBPlayer is active, we try to reconstruct the GC save dir location
   *  with regards to content sorting and core sorting. If we do not do this,
   *  there will be a User directory with GC saves in the GB/GBC/GBA rom save path
   *  when a user has content/core sorting enabled. Since the user is trying to avoid
   *  said behavior actively if those sorting options are enabled,
   *  we should seperate them accordingly.
   */
  if (Libretro::GBPlayer_active)
  {
    std::filesystem::path gba_content_dir = std::filesystem::path("");
    if (!Libretro::GBPlayer_rom_path.empty())
      gba_content_dir = std::filesystem::path(Libretro::GBPlayer_rom_path).parent_path().filename().string();

    Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &gba_save_dir);
    std::filesystem::path base = std::filesystem::path(gba_save_dir);

    bool core_sort = false;
    retro_system_info sys_info{};
    retro_get_system_info(&sys_info);
    const std::string library_name = sys_info.library_name;

    if (!base.empty() && !library_name.empty())
      core_sort = Common::CaseInsensitiveEquals(base.filename().string(), library_name);
    if (core_sort)
      base = base.parent_path();

    bool content_sort = false;
    std::string gc_content_dir;
    if (!gba_content_dir.empty() && !base.empty())
      content_sort = Common::CaseInsensitiveEquals(base.filename().string(), gba_content_dir.string());
    if (content_sort)
    {
      gc_content_dir = std::filesystem::path(game->path).parent_path().filename().string();
      base = base.parent_path();
    }

    rebuild_save_dir = base.string();
    if (content_sort)
      rebuild_save_dir = rebuild_save_dir + DIR_SEP + gc_content_dir;
    if (core_sort)
      rebuild_save_dir = rebuild_save_dir + DIR_SEP + library_name;

    save_dir = rebuild_save_dir.c_str();
    INFO_LOG_FMT(BOOT, "GBPlayer: GC save dir reconstructed to: '{}'", save_dir);
  }
  else
  {
    Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir);
  }

  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY, &core_assets_dir);
  Libretro::InitDiskControlInterface();

  if (save_dir && *save_dir)
    user_dir = std::string(save_dir) + DIR_SEP "User";
  else if (system_dir && *system_dir)
    user_dir = std::string(system_dir) + DIR_SEP "dolphin-emu" DIR_SEP "User";

  if (system_dir && *system_dir)
    sys_dir = std::string(system_dir) + DIR_SEP "dolphin-emu" DIR_SEP "Sys";
  else if (core_assets_dir && *core_assets_dir)
    sys_dir = std::string(core_assets_dir) + DIR_SEP "dolphin-emu" DIR_SEP "Sys";
  else if (save_dir && *save_dir)
    sys_dir = std::string(save_dir) + DIR_SEP "Sys";
  else
    sys_dir = "dolphin-emu" DIR_SEP "Sys";

  using namespace Libretro::Options;
  Libretro::Options::Init();
  Libretro::VFile::Init();

#ifdef ANDROID
  static bool sysdir_set = false;

  if(!sysdir_set)
  {
    File::SetSysDirectory(sys_dir);
    sysdir_set = true;
  }
#else
  File::SetSysDirectory(sys_dir);
#endif

  UICommon::SetUserDirectory(user_dir);
  UICommon::CreateDirectories();
  UICommon::Init();
  Libretro::Log::Init();

  // The GBA settings exist only when libmgba is built (USE_MGBA).
#ifdef HAS_LIBMGBA
  if (Libretro::GBPlayer_active)
  {
    Config::SetBase(Config::MAIN_GBA_ROM_PATHS[Config::GBPLAYER_GBA_INDEX], Libretro::GBPlayer_rom_path);
    INFO_LOG_FMT(BOOT, "GBPlayer: GBA ROM path applied from subsystem: '{}'", Libretro::GBPlayer_rom_path);
    Config::SetBase(Config::MAIN_GBA_SAVES_PATH, std::string(gba_save_dir));
    INFO_LOG_FMT(BOOT, "GBPlayer: ROM save set to: '{}'", gba_save_dir);
    // In order to prevent confusion for the settings of the save location between RA and the
    // emulator we just turn this off
    Config::SetBase(Config::MAIN_GBA_SAVES_IN_ROM_PATH, false);
  }
  else
  {
    Config::SetBase(Config::MAIN_GBA_ROM_PATHS[Config::GBPLAYER_GBA_INDEX], std::string{});
  }
#endif

#ifdef __PROSPERO__
  // The fog debug mode (VideoCommon/PixelShaderGen.cpp) changes shader code
  // without changing a shader's key, so the disk cache would hand back the
  // ordinary shaders: it is off for such a run.
  {
    extern int Ps5DolphinDebugMode();
    if (Ps5DolphinDebugMode() != 0)
      Config::SetBase(Config::GFX_SHADER_CACHE, false);
    // "dump": every frame as an image in the user directory's Dump/Frames, to
    // compare with desktop Dolphin playing the same FIFO log.
    // "swloader": Dolphin's software vertex loader instead of the x86-64 JIT one.
    if (Ps5DolphinDebugMode() == 4)
      Config::SetBase(Config::GFX_VERTEX_LOADER_TYPE, VertexLoaderType::Software);
    // "cmploader": both loaders, every mismatch logged (VertexLoaderTester).
    if (Ps5DolphinDebugMode() == 5)
      Config::SetBase(Config::GFX_VERTEX_LOADER_TYPE, VertexLoaderType::Compare);
    if (Ps5DolphinDebugMode() == 3)
    {
      Config::SetBase(Config::MAIN_MOVIE_DUMP_FRAMES, true);
      Config::SetBase(Config::GFX_DUMP_FRAMES_AS_IMAGES, true);
    }
  }
#endif
  Discord::SetDiscordPresenceEnabled(false);
  Common::SetEnableAlert(false);
  Common::SetAbortOnPanicAlert(false);
  Common::RegisterMsgAlertHandler([](const char* caption, const char* text,
    bool yes_no, Common::MsgType style) -> bool
  {
    // Log the message instead of showing a popup
    WARN_LOG_FMT(COMMON, "Suppressed popup: {} - {}", caption, text);
#ifdef __PROSPERO__
    // The title's trace carries the first alerts too: on screen they last two
    // seconds, and a run is judged from its trace. Bounded, since an alert a
    // frame would otherwise be a line a frame.
    static int ps5_alerts;
    if (ps5_alerts < 32)
    {
      ++ps5_alerts;
      std::fprintf(stderr, "dolphin alert: %s - %s\n", caption ? caption : "",
                   text ? text : "");
    }
#endif
    Libretro::Log::LogFrontEnd(style, caption, text, 2000);
    return true; // Always "continue"
  });

  NOTICE_LOG_FMT(BOOT, "SCM Git revision: {}", Common::GetScmRevGitStr());
  NOTICE_LOG_FMT(BOOT, "User Directory set to '{}'", user_dir);
  NOTICE_LOG_FMT(BOOT, "System Directory set to '{}'", sys_dir);

  const std::string codehandler = std::string(sys_dir) + DIR_SEP GECKO_CODE_HANDLER;

  if (!File::Exists(codehandler))
  {
#if defined(ANDROID) || defined(IPHONEOS)
    // for reduced area to display the message..
    const std::string missing_core_files_msg =
      fmt::format(
        "IMPORTANT - Open Online Updater ->\n"
        "Core System Files Downloader -> Dolphin.zip.");
#else
    const std::string missing_core_files_msg =
      fmt::format(
        "Core file {} missing! Open Online Updater ->\n"
        "Core System Files Downloader -> Install Dolphin.zip. "
        "Restart core to take effect.",
        GECKO_CODE_HANDLER);
#endif

    //OSD::AddMessage(missing_core_files_msg, OSD::Duration::VERY_LONG, OSD::Color::RED);

    Libretro::Log::LogFrontEnd(
      Common::Log::LogLevel::LERROR, missing_core_files_msg.c_str(),
      OSD::Duration::VERY_LONG);

    ERROR_LOG_FMT(BOOT, "{}", missing_core_files_msg);
  }

  // Main.Core
  Config::SetBase(Config::MAIN_CPU_CORE,
    static_cast<PowerPC::CPUCore>(
        Libretro::GetOption<int>(
            core::CPU_CORE,
            static_cast<int>(PowerPC::DefaultCPUCore()))));

#if defined(_DEBUG)
  Config::SetBase(Config::MAIN_FASTMEM, false);
  Config::SetBase(Config::MAIN_FASTMEM_ARENA, false);
#else
  Config::SetBase(Config::MAIN_FASTMEM,
                     Libretro::GetOption<bool>(core::FASTMEM, /*def=*/true));
  Config::SetBase(Config::MAIN_FASTMEM_ARENA,
                     Libretro::GetOption<bool>(core::FASTMEM_ARENA, /*def=*/true));
#endif
  Config::SetBase(Config::MAIN_ACCURATE_CPU_CACHE,
    Libretro::GetOption<bool>(core::MAIN_ACCURATE_CPU_CACHE, /*def=*/false));
#ifdef __PROSPERO__
  {
    const bool exact_fma = Libretro::GetOption<bool>(core::ACCURATE_FMADDS, /*def=*/false);
    Config::SetBase(Config::MAIN_ACCURATE_FMADDS, exact_fma);
    NOTICE_LOG_FMT(BOOT, "Exact multiply-add rounding (AccurateFmadds) = {}", exact_fma ? "Yes" : "No");
  }
  {
    // Porpoise 3.0: fast float math, the game's own choice.
    const bool fast = Libretro::GetOption<bool>(core::FAST_FLOAT_MATH, /*def=*/false);
    // Paired-single results stay unrounded only while paired math and paired
    // stores read them; anything else that reads them, and every exit from a
    // block, gets them rounded first (Jit64::PorpoisePlan). Leaving every result
    // unrounded ran Mario Superstar Baseball twice as fast but froze it.
    Config::SetBase(Config::MAIN_FAST_PAIRED_ROUNDING, fast);
    NOTICE_LOG_FMT(BOOT, "Riptide Boost (deferred paired rounding) = {}", fast ? "Yes" : "No");
  }
  {
    // Porpoise 3.0: the universal widescreen (beta), hooked as the game boots.
    const bool universal = Libretro::GetOption<bool>(core::UNIVERSAL_WIDESCREEN, /*def=*/false);
    HLE_PorpoiseWidescreen::SetEnabled(universal);
    NOTICE_LOG_FMT(BOOT, "Universal widescreen = {}", universal ? "Yes" : "No");
  }

#endif

  Config::SetBase(Config::MAIN_DSP_HLE,
                     Libretro::GetOption<bool>(audio::DSP_HLE, /*def=*/true));

  // dual core (true) or single core (false)
  Config::SetBase(Config::MAIN_CPU_THREAD,
    Libretro::GetOption<bool>(core::MAIN_CPU_THREAD, /*def=*/true));

  Config::SetBase(Config::MAIN_LOAD_GAME_INTO_MEMORY,
    Libretro::GetOption<bool>(core::MAIN_LOAD_GAME_INTO_MEMORY, /*def=*/false));

  Config::SetBase(Config::MAIN_ENABLE_CHEATS,
                     Libretro::GetOption<bool>(core::CHEATS_ENABLED, /*def=*/false));

  Config::SetBase(Config::MAIN_SKIP_IPL, Libretro::GetOption<bool>(core::SKIP_GC_BIOS, /*def=*/true));

  const int language = Libretro::GetOption<int>(core::LANGUAGE, static_cast<int>(DiscIO::Language::English));
  Config::SetBase(Config::SYSCONF_LANGUAGE, language);
  Config::SetBase(Config::MAIN_GC_LANGUAGE, DiscIO::ToGameCubeLanguage(static_cast<DiscIO::Language>(language)));

  Config::SetBase(Config::MAIN_DPL2_DECODER, false);
  Config::SetBase(Config::MAIN_AUDIO_LATENCY, 0);
  Config::SetBase(Config::MAIN_AUDIO_FILL_GAPS, false);

  Config::SetBase(Config::MAIN_EMULATION_SPEED,
                     Libretro::GetOption<double>(core::EMULATION_SPEED, /*def=*/0.0));
  {
    // Overclock (cpu clock rate) — option values in option_defs used strings like "100%" etc.
    double multiplier = Libretro::GetOption<double>(core::CPU_CLOCK_RATE, 1.0);
    Config::SetBase(Config::MAIN_OVERCLOCK, multiplier);
    Config::SetBase(Config::MAIN_OVERCLOCK_ENABLE, multiplier != 1.0);
  }

  Config::SetBase(Config::MAIN_PRECISION_FRAME_TIMING,
    Libretro::GetOption<bool>(core::MAIN_PRECISION_FRAME_TIMING,
                            /*def=*/false)); // true is the standalone default

  Config::SetBase(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
                  Libretro::GetOption<bool>(sysconf::WIIMOTE_CONTINUOUS_SCANNING,
                                             /*def=*/false));
  Config::SetBase(Config::MAIN_MMU,
                  Libretro::GetOption<bool>(core::MAIN_MMU, /*def=*/false));

  Config::SetBase(Config::MAIN_FAST_DISC_SPEED,
                  Libretro::GetOption<bool>(core::FAST_DISC_SPEED, /*def=*/false));

  Config::SetBase(Config::MAIN_RUSH_FRAME_PRESENTATION,
                  Libretro::GetOption<bool>(core::RUSH_FRAME_PRESENTATION, /*def=*/false));

  Config::SetBase(Config::MAIN_SMOOTH_EARLY_PRESENTATION,
                  Libretro::GetOption<bool>(core::SMOOTH_EARLY_PRESENTATION, /*def=*/false));

  // Main.Interface
  Config::SetBase(Config::MAIN_OSD_MESSAGES,
                     Libretro::GetOption<bool>(main_interface::OSD_ENABLED, /*def=*/true));
  Config::SetBase(Config::MAIN_ENABLE_DEBUGGING,
                     Libretro::GetOption<bool>(main_interface::ENABLE_DEBUGGING, /*def=*/false));

  // Main.General
  Config::SetBase(Config::MAIN_TIME_TRACKING, false);

  // Main.DSP
  Config::SetBase(Config::MAIN_DSP_JIT,
                     Libretro::GetOption<bool>(audio::DSP_JIT, /*def=*/true));
  Config::SetBase(Config::MAIN_DUMP_AUDIO, false);

  Config::SetBase(Config::MAIN_AUDIO_BACKEND, BACKEND_LIBRETRO);

  // Main.BluetoothPassthrough
  Config::SetBase(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED,
    Libretro::GetOption<bool>(Libretro::Options::main_bluetooth::BLUETOOTH_PASSTHROUGH, /*def=*/false));

  // SYSCONF (GC)
  {
    const std::string value =
      Libretro::GetOption<std::string>(
        sysconf_gc::SP1_DEVICE,
        "255" // ExpansionInterface::EXIDeviceType::None
      );

    const int device_int = std::stoi(value);
    const auto device = static_cast<ExpansionInterface::EXIDeviceType>(device_int);

    Config::SetBase(Config::MAIN_SERIAL_PORT_1, device);
  }

  // SYSCONF.IPL
  Config::SetBase(Config::SYSCONF_WIDESCREEN,
                 Libretro::GetOption<bool>(sysconf::WIDESCREEN, /*def=*/true));
  Config::SetBase(Config::SYSCONF_PROGRESSIVE_SCAN,
                 Libretro::GetOption<bool>(sysconf::PROGRESSIVE_SCAN, /*def=*/true));
  Config::SetBase(Config::SYSCONF_PAL60,
                 Libretro::GetOption<bool>(sysconf::PAL60, /*def=*/true));

  // SYSCONF.BT
  Config::SetBase(Config::SYSCONF_SENSOR_BAR_POSITION,
                 Libretro::GetOption<int>(sysconf::SENSOR_BAR_POSITION, 0));
  Config::SetBase(Config::SYSCONF_WIIMOTE_MOTOR,
                 Libretro::GetOption<bool>(sysconf::ENABLE_RUMBLE, /*def=*/true));

  // Graphics.Hardware
  //Config::SetBase(Config::GFX_VSYNC,
  //               Libretro::GetOption<bool>(gfx_hardware::VSYNC, /*def=*/false));

  // Graphics.Settings
  Config::SetBase(Config::GFX_WIDESCREEN_HACK,
                 Libretro::GetOption<bool>(gfx_settings::WIDESCREEN_HACK, /*def=*/false));
  Config::SetBase(
    Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(
      Libretro::GetOption<int>(
        gfx_settings::ASPECT_RATIO, static_cast<int>(AspectMode::Stretch)
      )
    )
  );
  Config::SetBase(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                 Libretro::GetOption<int>(gfx_settings::TEXTURE_CACHE_ACCURACY, 128));
  Config::SetBase(Config::GFX_HIRES_TEXTURES,
                 Libretro::GetOption<bool>(gfx_enhancements::LOAD_CUSTOM_TEXTURES, /*def=*/false));
  Config::SetBase(Config::GFX_CACHE_HIRES_TEXTURES,
                 Libretro::GetOption<bool>(gfx_enhancements::CACHE_CUSTOM_TEXTURES, /*def=*/false));
  Config::SetBase(Config::GFX_ENABLE_GPU_TEXTURE_DECODING,
                 Libretro::GetOption<bool>(gfx_settings::GPU_TEXTURE_DECODING, /*def=*/false));
  Config::SetBase(Config::GFX_ENABLE_PIXEL_LIGHTING,
                  Libretro::GetOption<bool>(gfx_settings::ENABLE_PIXEL_LIGHTING, /*def=*/false));
  Config::SetBase(Config::GFX_FAST_DEPTH_CALC,
                 Libretro::GetOption<bool>(gfx_settings::FAST_DEPTH_CALCULATION, /*def=*/true));
  Config::SetBase(Config::GFX_DISABLE_FOG,
                  Libretro::GetOption<bool>(gfx_settings::DISABLE_FOG, /*def=*/false));
  Config::SetBase(Config::GFX_EFB_SCALE,
                 Libretro::GetOption<int>(gfx_settings::EFB_SCALE, /*def=*/1));
  Config::SetBase(Config::GFX_BACKEND_MULTITHREADING, false);
  Config::SetBase(Config::GFX_SHADER_COMPILATION_MODE,
                  static_cast<ShaderCompilationMode>(
                    Libretro::GetOption<int>(
                      gfx_settings::SHADER_COMPILATION_MODE,
                      static_cast<int>(ShaderCompilationMode::Synchronous))));

  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING,
    Libretro::GetOption<bool>(gfx_settings::WAIT_FOR_SHADERS, /*def=*/false));
#ifdef __PROSPERO__
  // Porpoise 3.0: two threads compile the shaders a game asks for (Dolphin's
  // default is one), so its exact shaders replace the slower ubershaders
  // sooner; the PS5 has cores to spare beside the CPU and GPU threads.
  Config::SetBase(Config::GFX_SHADER_COMPILER_THREADS, 2);
  // Porpoise 4.0: and three for "Build Shaders Before Starting". Dolphin's
  // default is every processor but two (eleven here), each with the driver's
  // 32 MiB compile stack out of the title's small flexible memory: a game with
  // a big shader cache could stall on a black screen before its first frame.
  Config::SetBase(Config::GFX_SHADER_PRECOMPILER_THREADS, 3);
#endif

  if (Common::is_uwp())
    Config::SetBase(Config::GFX_SHADER_CACHE, false);

  Config::SetBase(Config::GFX_MODS_ENABLE,
    Libretro::GetOption<bool>(gfx_settings::MODS_ENABLE, /*def=*/false));

  // Graphics.Enhancements
  Config::SetBase(Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                  static_cast<TextureFilteringMode>(
                    Libretro::GetOption<int>(
                      gfx_enhancements::FORCE_TEXTURE_FILTERING_MODE,
                      static_cast<int>(TextureFilteringMode::Default))));

  Config::SetBase(Config::GFX_ENHANCE_MAX_ANISOTROPY,
                  static_cast<AnisotropicFilteringMode>(
                    Libretro::GetOption<int>(
                      gfx_enhancements::MAX_ANISOTROPY,
                      static_cast<int>(AnisotropicFilteringMode::Force1x))));

  Config::SetBase(Config::GFX_ENHANCE_OUTPUT_RESAMPLING,
                  static_cast<OutputResamplingMode>(
                    Libretro::GetOption<int>(
                      gfx_enhancements::GFX_ENHANCE_OUTPUT_RESAMPLING,
                        static_cast<int>(OutputResamplingMode::Default))));

  Config::SetBase(Config::GFX_ENHANCE_FORCE_TRUE_COLOR,
                          Libretro::GetOption<bool>(gfx_enhancements::FORCE_TRUE_COLOR,
                                                    /*def=*/true));

  Config::SetBase(Config::GFX_ENHANCE_DISABLE_COPY_FILTER,
                  Libretro::GetOption<bool>(gfx_enhancements::GFX_ENHANCE_DISABLE_COPY_FILTER,
                    /*def=*/true));
  Config::SetBase(Config::GFX_ENHANCE_HDR_OUTPUT,
                  Libretro::GetOption<bool>(gfx_enhancements::GFX_ENHANCE_HDR_OUTPUT,
                                            /*def=*/false));
  Config::SetBase(Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION,
                  Libretro::GetOption<bool>(gfx_enhancements::GFX_ARBITRARY_MIPMAP_DETECTION, /*def=*/false));

  // Graphics.Hacks
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_ENABLE,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_ACCESS_ENABLE, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_EFB_DEFER_INVALIDATION,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_ACCESS_DEFER_INVALIDATION, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE,
                 Libretro::GetOption<int>(gfx_hacks::EFB_ACCESS_TILE_SIZE, /*def=*/64));
  Config::SetBase(Config::GFX_HACK_BBOX_ENABLE,
                 Libretro::GetOption<bool>(gfx_hacks::BBOX_ENABLED, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_FORCE_PROGRESSIVE,
                 Libretro::GetOption<bool>(gfx_hacks::FORCE_PROGRESSIVE, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_TO_TEXTURE, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                 Libretro::GetOption<bool>(gfx_hacks::XFB_TO_TEXTURE_ENABLE, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_DISABLE_COPY_TO_VRAM,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_TO_VRAM, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_DEFER_EFB_COPIES,
                 Libretro::GetOption<bool>(gfx_hacks::DEFER_EFB_COPIES, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_IMMEDIATE_XFB,
                 Libretro::GetOption<bool>(gfx_hacks::IMMEDIATE_XFB, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_SKIP_DUPLICATE_XFBS,
                 Libretro::GetOption<bool>(gfx_hacks::SKIP_DUPE_FRAMES, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_EARLY_XFB_OUTPUT,
                 Libretro::GetOption<bool>(gfx_hacks::EARLY_XFB_OUTPUT, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_COPY_EFB_SCALED,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_SCALED_COPY, /*def=*/true));
  Config::SetBase(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES,
                 Libretro::GetOption<bool>(gfx_hacks::EFB_EMULATE_FORMAT_CHANGES, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_VERTEX_ROUNDING,
                 Libretro::GetOption<bool>(gfx_hacks::VERTEX_ROUNDING, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_VI_SKIP,
                 Libretro::GetOption<bool>(gfx_hacks::VI_SKIP, /*def=*/false));
  Config::SetBase(Config::GFX_HACK_FAST_TEXTURE_SAMPLING,
                 Libretro::GetOption<bool>(gfx_hacks::FAST_TEXTURE_SAMPLING, /*def=*/true));
  #ifdef __APPLE__
  Config::SetBase(Config::GFX_HACK_NO_MIPMAPPING,
                 Libretro::GetOption<bool>(gfx_hacks::NO_MIPMAPPING, /*def=*/false));
  #endif

  switch (Libretro::GetOption<int>(gfx_settings::ANTI_ALIASING, 0))
  {
    case 1:  // 2x MSAA
      Config::SetBase(Config::GFX_MSAA, 2);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 2:  // 4x MSAA
      Config::SetBase(Config::GFX_MSAA, 4);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 3:  // 8x MSAA
      Config::SetBase(Config::GFX_MSAA, 8);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
    case 4:  // 2x SSAA
      Config::SetBase(Config::GFX_MSAA, 2);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    case 5:  // 4x SSAA
      Config::SetBase(Config::GFX_MSAA, 4);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    case 6:  // 8x SSAA
      Config::SetBase(Config::GFX_MSAA, 8);
      Config::SetBase(Config::GFX_SSAA, true);
      break;
    default: // disabled
      Config::SetBase(Config::GFX_MSAA, 1);
      Config::SetBase(Config::GFX_SSAA, false);
      break;
  }

  // Graphics.GameSpecific
  Config::SetBase(Config::GFX_PERF_QUERIES_ENABLE,
                 Libretro::GetOption<bool>(gfx_gamespecific::GFX_PERF_QUERIES_ENABLE, /*def=*/false));

  /* disable throttling emulation to match GetTargetRefreshRate() */
  Core::SetIsThrottlerTempDisabled(true);
  SConfig::GetInstance().bBootToPause = true;

#ifdef IPHONEOS
  bool can_jit = false;
  {
    struct retro_exec_mem_alloc probe = {};
    probe.version = 1;
    probe.size = 0;
    if (Libretro::environ_cb(RETRO_ENVIRONMENT_EXEC_MEM_ALLOC, &probe))
    {
      if (probe.mode != RETRO_EXEC_MEM_MODE_UNAVAILABLE)
        can_jit = true;
    }
    else if (!Libretro::environ_cb(RETRO_ENVIRONMENT_GET_JIT_CAPABLE, &can_jit))
      can_jit = false;
  }
  if (!can_jit)
  {
    auto current = Config::Get(Config::MAIN_CPU_CORE);
    if (current == PowerPC::CPUCore::JIT64 ||
        current == PowerPC::CPUCore::JITARM64)
    {
      Config::SetBase(Config::MAIN_CPU_CORE, PowerPC::CPUCore::CachedInterpreter);
    }

    Config::SetBase(Config::GFX_VERTEX_LOADER_TYPE, VertexLoaderType::Software);

    OSD::AddMessage("CPU: Just in time compiler disabled as unavailable on your system", OSD::Duration::NORMAL);
  }
#endif
  NOTICE_LOG_FMT(BOOT, "CPU Core: {}", Libretro::Options::CPUCoreToString(Config::Get(Config::MAIN_CPU_CORE)));
  NOTICE_LOG_FMT(BOOT, "Fastmem enabled = {}", (Config::Get(Config::MAIN_FASTMEM)) ? "Yes" : "No");
  NOTICE_LOG_FMT(BOOT, "JIT debug enabled = {}", Config::IsDebuggingEnabled() ? "Yes" : "No");
  // Porpoise: what the JIT may use of this CPU (FMA and AVX make the game's
  // paired-single maths shorter).
  NOTICE_LOG_FMT(BOOT, "CPU: {} | FMA used = {}", cpu_info.Summarize(),
                 Config::Get(Config::SESSION_USE_FMA) ? "Yes" : "No");

  Libretro::FrameTiming::Init();
  Libretro::Audio::Init();
  Libretro::Video::Init();
  WindowSystemInfo wsi(WindowSystemType::Libretro, nullptr, nullptr, nullptr);
  VideoBackendBase::PopulateBackendInfo(wsi);
  NOTICE_LOG_FMT(VIDEO, "Using GFX backend: {}", Config::Get(Config::MAIN_GFX_BACKEND));

  std::vector<std::string> normalized_game_paths;
  normalized_game_paths.push_back(Libretro::VFile::NormalizePath(game->path));
  std::string folder_path_str;
  std::string filename_str;
  std::string extension;
  SplitPath(normalized_game_paths.front(), &folder_path_str, &filename_str, &extension);
  fs::path folder_path(folder_path_str);
  fs::path filename(filename_str);
  std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char c){ return std::tolower(c); });

#ifdef _WIN32
  // If SplitPath only gave us "D:", rebuild the real directory from the full path
  if (folder_path_str.size() == 2 && folder_path_str[1] == ':')
  {
    // take everything up to the last backslash
    size_t last_slash = normalized_game_paths.front().find_last_of("\\/");
    if (last_slash != std::string::npos)
      folder_path_str = normalized_game_paths.front().substr(0, last_slash + 1);
    folder_path = fs::path(folder_path_str);
  }
#endif

  if (extension == ".m3u" || extension == ".m3u8")
  {
    normalized_game_paths = ReadM3UFile(normalized_game_paths.front(), folder_path_str);
    if (normalized_game_paths.empty())
    {
      ERROR_LOG_FMT(BOOT, "Could not boot {}. M3U contains no paths", game->path);
      return false;
    }
  }

  /* Porpoise: the game's other discs (the library's Disc 2 of the same game),
   * after the one chosen: Dolphin changes to the next one itself when the
   * game asks for it, as with a disc list. */
  if (const char* extra = std::getenv("PORPOISE_EXTRA_DISCS");
      extra && *extra && normalized_game_paths.size() == 1 && extension != ".m3u" && extension != ".m3u8")
  {
    const std::string list = extra;
    for (size_t at = 0; at < list.size();)
    {
      size_t end = list.find('\n', at);
      if (end == std::string::npos)
        end = list.size();
      if (end > at)
        normalized_game_paths.push_back(Libretro::VFile::NormalizePath(list.substr(at, end - at)));
      at = end + 1;
    }
    NOTICE_LOG_FMT(BOOT, "Porpoise: {} discs for this game", normalized_game_paths.size());
  }
  // Porpoise: a game on more than one disc changes to the next one itself
  // when it ejects the one in (Dolphin's "Change Discs Automatically"; off
  // in its defaults, so the list alone never moved).
  Config::SetCurrent(Config::MAIN_AUTO_DISC_CHANGE, normalized_game_paths.size() > 1);

  for (auto& normalized_game_path : normalized_game_paths)
    Libretro::disk_paths.push_back(Libretro::VFile::DenormalizePath(normalized_game_path));

  Libretro::Input::Init(wsi);

  // for Wii
  if (Libretro::Input::g_has_microphone_support)
  {
    Config::SetCurrent(Config::MAIN_EMULATE_WII_SPEAK,
      Libretro::GetOption<bool>(sysconf::WII_SPEAK_ENABLE, /*def=*/false));
    Config::SetCurrent(Config::MAIN_WII_SPEAK_MUTED,
      Libretro::GetOption<bool>(sysconf::WII_SPEAK_MUTED, /*def=*/true));

    // logitech
    Config::SetCurrent(Config::MAIN_EMULATE_LOGITECH_MIC[0],
      Libretro::GetOption<bool>(sysconf::WII_LOGI_MICROPHONE_ENABLE, /*def=*/false));
  }

  const bool disc_based_games_boot_to_wii_menu = Libretro::GetOption<bool>(Libretro::Options::core::DISC_BASED_GAMES_BOOT_TO_WII_MENU, false);
  std::unique_ptr<BootParameters> boot_params = BootParameters::GenerateFromFile(normalized_game_paths);

  /* Porpoise: what its menus ask for, in the environment (the frontend sets
   * these before each start): the GameCube Menu (the player's own BIOS) or
   * the Wii Menu, with the chosen game in the drive or none; and channels
   * (the player's WADs) installed for good first, so the Wii Menu has them. */
  auto env = [](const char* key) {
    const char* v = std::getenv(key);
    return std::string(v ? v : "");
  };
  /* The drive's default disc is only ever what this start puts there (it
   * is saved in Dolphin.ini at the end, and a WAD or an app started later
   * would otherwise find the last menu's disc in its drive). */
  Config::SetCurrent(Config::MAIN_DEFAULT_ISO, "");
  const std::string porpoise_menu = env("PORPOISE_BOOT_MENU");
  const bool porpoise_menu_disc = env("PORPOISE_BOOT_DISC") == "1";
  /* Porpoise: whether a menu really boots (a disc the Wii Menu won't take
   * starts directly instead, and keeps its achievements). */
  bool porpoise_menu_boots = false;
  {
    const std::string wads = env("PORPOISE_INSTALL_WADS");
    std::size_t at = 0;
    while (at < wads.size())
    {
      std::size_t end = wads.find('\n', at);
      if (end == std::string::npos)
        end = wads.size();
      const std::string wad = wads.substr(at, end - at);
      at = end + 1;
      if (wad.empty())
        continue;
      const bool installed = WiiUtils::InstallWAD(Libretro::VFile::NormalizePath(wad));
      NOTICE_LOG_FMT(BOOT, "Porpoise: channel {} for the Wii Menu: {}", wad,
                     installed ? "installed" : "couldn't be installed");
    }
  }

  /* Wii System Update: the titles of the list the frontend asked for (the
   * Wii Menu, its IOS and channels), from Nintendo's servers, installed as
   * Dolphin's Online System Update does; the launch screen shows how far. */
  if (const std::string update = env("PORPOISE_SYSTEM_UPDATE"); !update.empty())
  {
    auto report = [](size_t done, size_t total, u64) {
      const std::string text = fmt::format("porpoise-progress:{}/{}", done, total);
      retro_message msg{text.c_str(), 1};
      Libretro::environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
      return true;
    };
    const WiiUtils::UpdateResult result = WiiUtils::DoOnlineUpdate(report, update);
#ifdef __PROSPERO__
    std::fprintf(stderr, "core: Wii System Update returned %d\n", static_cast<int>(result));
#endif
    /* A Wii made by the update has never been set up: its Wii Menu would
     * start with the first-time setup screens. Marked as set up, in the
     * update's country, so it starts at the menu (the language, widescreen
     * and sensor bar are Porpoise's settings as for every Wii game). */
    if (result == WiiUtils::UpdateResult::Succeeded || result == WiiUtils::UpdateResult::AlreadyUpToDate)
    {
      IOS::HLE::Kernel ios;
      SysConf sysconf{ios.GetFS()};
      const SysConf::Entry* done = sysconf.GetEntry("IPL.CD");
      if (!done || done->bytes.empty() || done->bytes[0] == 0)
      {
        sysconf.SetData<u8>("IPL.CD", SysConf::Entry::Type::ByteBool, 1);
        sysconf.SetData<u8>("IPL.CD2", SysConf::Entry::Type::ByteBool, 1);
        const u8 country = update == "USA" ? 49 : update == "JPN" ? 1 : update == "KOR" ? 136 : 0;
        if (SysConf::Entry* sadr = sysconf.GetEntry("IPL.SADR"); sadr && !sadr->bytes.empty() && country)
          sadr->bytes[0] = country;
        NOTICE_LOG_FMT(BOOT, "Porpoise: the new Wii marked as set up ({})", sysconf.Save() ? "saved" : "not saved");
      }
    }
    NOTICE_LOG_FMT(BOOT, "Porpoise: Wii System Update ({}): result {}", update, static_cast<int>(result));
    if (result != WiiUtils::UpdateResult::Succeeded && result != WiiUtils::UpdateResult::AlreadyUpToDate)
    {
      // The frontend tells the player after the session (porpoise_core).
      const std::string text = fmt::format("porpoise-update-failed:{}", static_cast<int>(result));
      retro_message msg{text.c_str(), 1};
      Libretro::environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
  }

  /* The installed Wii Menu's region (from its version), Unknown if none. */
  auto wii_menu_region = []() {
    const std::string tmd_path = Common::GetTMDFileName(Titles::SYSTEM_MENU, Common::FromWhichRoot::Configured);
    File::IOFile f(tmd_path, "rb");
    if (!f)
      return DiscIO::Region::Unknown;
    std::vector<u8> bytes(f.GetSize());
    if (bytes.empty() || !f.ReadBytes(bytes.data(), bytes.size()))
      return DiscIO::Region::Unknown;
    const IOS::ES::TMDReader tmd{std::move(bytes)};
    return tmd.IsValid() ? DiscIO::GetSysMenuRegion(tmd.GetTitleVersion()) : DiscIO::Region::Unknown;
  };
  /* A Wii Menu shows "An incorrect disc has been inserted" for a disc of
   * another region, as a real Wii does: such a disc starts directly. */
  auto menu_takes = [&](const DiscIO::VolumeDisc& volume) {
    const DiscIO::Region menu = wii_menu_region();
    const DiscIO::Region disc = volume.GetRegion();
    return menu == DiscIO::Region::Unknown || disc == DiscIO::Region::Unknown || menu == disc;
  };

  if (porpoise_menu == "gc")
  {
    const std::string r = env("PORPOISE_BOOT_REGION");
    const DiscIO::Region region = r == "EUR" ? DiscIO::Region::PAL :
                                  r == "JAP" ? DiscIO::Region::NTSC_J :
                                               DiscIO::Region::NTSC_U;
    std::unique_ptr<DiscIO::VolumeDisc> volume;
    if (porpoise_menu_disc && !normalized_game_paths.empty())
      volume = DiscIO::CreateDisc(normalized_game_paths.front());
    if (volume && volume->GetVolumeType() == DiscIO::Platform::GameCubeDisc)
      boot_params = std::make_unique<BootParameters>(BootParameters::IPL(
          region, BootParameters::Disc{normalized_game_paths.front(), std::move(volume), {}}));
    else
      boot_params = std::make_unique<BootParameters>(BootParameters::IPL(region));
    porpoise_menu_boots = true;
    NOTICE_LOG_FMT(BOOT, "Porpoise: the GameCube Menu ({}), {}", r.empty() ? "USA" : r,
                   porpoise_menu_disc ? "with a disc" : "no disc");
  }
  else if (porpoise_menu == "wii")
  {
    std::string disc_path;
    if (porpoise_menu_disc && !normalized_game_paths.empty())
    {
      auto volume = DiscIO::CreateDisc(normalized_game_paths.front());
      if (volume && volume->GetVolumeType() == DiscIO::Platform::WiiDisc && menu_takes(*volume))
        disc_path = normalized_game_paths.front();
    }
    if (porpoise_menu_disc && disc_path.empty())
      WARN_LOG_FMT(BOOT, "Porpoise: the disc isn't one this Wii Menu takes; starting it directly");
    else
    {
      Config::SetCurrent(Config::MAIN_DEFAULT_ISO, disc_path);
      boot_params = std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU});
      porpoise_menu_boots = true;
      NOTICE_LOG_FMT(BOOT, "Porpoise: the Wii Menu, {}", disc_path.empty() ? "no disc" : "with a disc");
    }
  }
  else if (disc_based_games_boot_to_wii_menu)
  {
    /* Porpoise: Wii discs only (the Wii Menu here can't start a GameCube
     * disc), and only of the Wii Menu's own region. */
    bool is_disc_title = false;
    if (!normalized_game_paths.empty())
    {
      auto volume = DiscIO::CreateDisc(normalized_game_paths.front());
      if (volume && volume->GetVolumeType() == DiscIO::Platform::WiiDisc)
      {
        is_disc_title = menu_takes(*volume);
        if (!is_disc_title)
          WARN_LOG_FMT(BOOT, "'Disc Based Games Boot to Wii System Menu': the disc's region isn't the Wii Menu's, booting directly");
      }
      else
        WARN_LOG_FMT(BOOT, "'Disc Based Games Boot to Wii System Menu' enabled but content is not a Wii disc, ignoring Wii Menu");
    }

    const std::string wii_menu_tmd_file_name = Common::GetTMDFileName(Titles::SYSTEM_MENU, Common::FromWhichRoot::Configured);
    const bool wii_menu_installed = File::Exists(wii_menu_tmd_file_name);
    if (is_disc_title && !wii_menu_installed)
      WARN_LOG_FMT(BOOT,"'Disc Based Games Boot to Wii System Menu' enabled but Wii Menu not found at save location: {}, booting directly instead", wii_menu_tmd_file_name);

    if (is_disc_title && wii_menu_installed)
    {
      Config::SetCurrent(Config::MAIN_DEFAULT_ISO, normalized_game_paths.front());
      boot_params = std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU});
    }
  }

  /* Porpoise: the Wii Menu asked for but not there (a Wii System Update that
   * didn't finish): no boot at all, rather than one that fails half-way. */
  if (porpoise_menu == "wii" &&
      !File::Exists(Common::GetTMDFileName(Titles::SYSTEM_MENU, Common::FromWhichRoot::Configured)))
  {
    ERROR_LOG_FMT(BOOT, "Porpoise: no Wii Menu installed; not booting");
    Config::Save(); // now, while the core is whole (see below)
    return false;
  }

#ifdef __PROSPERO__
  std::fprintf(stderr, "core: booting (%s)\n", porpoise_menu.empty() ? "the game" : porpoise_menu.c_str());
  std::fprintf(stderr, "core: cpu vendor %d, %s, PEXT/PDEP %s\n", static_cast<int>(cpu_info.vendor),
               cpu_info.Summarize().c_str(), cpu_info.bBMI2FastParallelBitOps ? "used" : "not used");
#endif
  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot_params), wsi))
  {
    // Porpoise: the settings changed above are saved now. Left for the
    // config's own teardown when the core is unloaded, saving the Wii's
    // SYSCONF there made an IOS kernel after the system was gone (a crash
    // after every game that didn't start).
    Config::Save();
    ERROR_LOG_FMT(BOOT, "Could not boot {}", game->path);
    return false;
  }
  /* Porpoise: RetroAchievements after BootCore, which closes any game the
   * manager has (SetRunningGameMetadata -> CloseGame cancels its queue: the
   * token sign-in queued before it was dropped, and the set never loaded).
   * The game itself boots on the first retro_run (EmuThread), which is when
   * its set is identified. */
  if (!normalized_game_paths.empty() && !porpoise_menu_boots)
    Libretro::Achievements::Start(normalized_game_paths.front());

#ifdef __PROSPERO__
  std::fprintf(stderr, "core: booted; input next\n");
#endif
  Libretro::Input::InitStage2();

  const bool importCheats = Libretro::GetOption<bool>(retroarch_core::CHEATS_IMPORT, true);

  if (importCheats)
  {
    Libretro::reload_cheats_from_ini();
    Libretro::generate_cht_from_ini(filename_str);
  }

  return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
  if (game_type != Libretro::g_gbplayer_subsystem_id)
  {
    ERROR_LOG_FMT(BOOT, "retro_load_game_special: unknown game_type {}", game_type);
    return false;
  }
  if (num_info < 1 || !info)
  {
    ERROR_LOG_FMT(BOOT, "retro_load_game_special: no content info");
    return false;
  }

  const retro_game_info* gba_slot = (num_info >= 2 && info[0].path && *info[0].path) ? &info[0] : nullptr;

  const retro_game_info* gc_slot = (info[1].path && *info[1].path) ? &info[1] : nullptr;

  if (!gc_slot)
  {
    ERROR_LOG_FMT(BOOT, "GBPlayer: no GC disc path provided in slot 0");
    return false;
  }

  INFO_LOG_FMT(BOOT, "GBPlayer: GC disc  = '{}'", gc_slot->path);
  if (gba_slot)
  {
    Libretro::GBPlayer_rom_path = gba_slot->path;
    INFO_LOG_FMT(BOOT, "GBPlayer: ROM = '{}'", gba_slot->path);
  }
  else
  {
    Libretro::GBPlayer_rom_path.clear();
    INFO_LOG_FMT(BOOT, "GBPlayer: no ROM");
  }

  Libretro::GBPlayer_active = true;

  return retro_load_game(gc_slot);
}

void retro_unload_game(void)
{
  auto& system = Core::System::GetInstance();

  if (Core::IsRunning(system))
  {
    Core::Stop(system);
#if defined(__LIBUSB__)
    system.ShutdownUSBScanner();
#endif

    Core::Shutdown(system);
  }
  Libretro::Achievements::Stop();

  if (!g_context_status.IsDestroyed() && g_video_backend)
    g_video_backend->Shutdown();

  // these are disabled in Shutdown on fullscreen/window toggle
  system.GetFifo().Shutdown();

  // Rest of shutdown
  g_context_status.MarkUnitialized();
  Libretro::Input::Shutdown();
  Libretro::Log::Shutdown();
  UICommon::ShutdownControllers();
  UICommon::Shutdown();

  if (!system.IsDualCoreMode())
  {
    Core::SingleCorePostRunShutdown();

    // otherwise it is still called CPU-GPU Thread
    Common::SetCurrentThreadName("retroarch");
  }

  Core::UndeclareAsCPUThread();
  Core::UndeclareAsGPUThread();
  Libretro::ResetDiskControlState();
}

namespace Libretro
{
// Disk swapping
static bool retro_set_eject_state(bool ejected)
{
  if (eject_state == ejected)
    return false;

  eject_state = ejected;

  // Porpoise: from the frontend's thread between runs (its menu's Change
  // Disc), the CPU thread is stepping and wouldn't take a queued job until the
  // next run: run it here as the CPU thread, as retro_serialize does.
  const bool was_cpu = Core::IsCPUThread();
  if (!was_cpu)
    Core::DeclareAsCPUThread();
  Core::RunOnCPUThread(Core::System::GetInstance(), [ejected] {
    auto& system = Core::System::GetInstance();
    Core::CPUThreadGuard guard{system};

    if (ejected)
      system.GetDVDInterface().EjectDisc(guard, DVD::EjectCause::User);
    else if (disk_index < disk_paths.size())
    {
      const std::string path = Libretro::VFile::NormalizePath(disk_paths[disk_index]);
      system.GetDVDInterface().ChangeDisc(guard, path);
    }
  }, true); // wait_for_completion = true
  if (!was_cpu)
    Core::UndeclareAsCPUThread();

  return true;
}

static bool retro_get_eject_state()
{
  return eject_state;
}

static unsigned retro_get_image_index()
{
  // Porpoise: Dolphin changes discs by itself from its list (a game's other
  // disc, handed over at boot): the disc it put in is the one in.
  const int automatic = Core::System::GetInstance().GetDVDInterface().PorpoiseAutoDiscIndex();
  if (automatic >= 0 && size_t(automatic) < disk_paths.size())
    return unsigned(automatic);
  return disk_index;
}

static bool retro_set_image_index(unsigned index)
{
  if (eject_state)
    disk_index = index;

  return eject_state;
}

static unsigned retro_get_num_images()
{
  return (unsigned)disk_paths.size();
}

static bool retro_add_image_index()
{
  disk_paths.push_back("");

  return true;
}

static bool retro_replace_image_index(unsigned index, const struct retro_game_info* info)
{
  if (index >= disk_paths.size())
    return false;

  if (!info->path)
  {
    disk_paths.erase(disk_paths.begin() + index);
    if (!disk_paths.size())
      disk_index = -1;
    else if (disk_index > index)
      disk_index--;
  }
  else
    disk_paths[index] = info->path;

  return true;
}

static bool RETRO_CALLCONV retro_set_initial_image(unsigned index, const char* path)
{
  if (index >= disk_paths.size())
    index = 0;

  disk_index = index;

  return true;
}

static bool RETRO_CALLCONV retro_get_image_path(unsigned index, char* path, size_t len)
{
  if (index >= disk_paths.size())
    return false;

  if (disk_paths[index].empty())
    return false;

  strncpy(path, disk_paths[index].c_str(), len);
  return true;
}
static bool RETRO_CALLCONV retro_get_image_label(unsigned index, char* label, size_t len)
{
  if (index >= disk_paths.size())
    return false;

  if (disk_paths[index].empty())
    return false;

  strncpy(label, disk_paths[index].c_str(), len);
  return true;
}

static void InitDiskControlInterface()
{
  static retro_disk_control_ext_callback disk_control = {
      retro_set_eject_state,
      retro_get_eject_state,
      retro_get_image_index,
      retro_set_image_index,
      retro_get_num_images,
      retro_replace_image_index,
      retro_add_image_index,
      retro_set_initial_image,
      retro_get_image_path,
      retro_get_image_label,
  };

  environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control);
}
}  // namespace Libretro
