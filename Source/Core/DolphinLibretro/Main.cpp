
#include <atomic>
#include <cstdint>
#include <libretro.h>
#include <string>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <optional>
#include <string>
#include <thread>

#include <fstream>
#include <vector>

#include "AudioCommon/AudioCommon.h"
#include "Common/ChunkFile.h"
#include "Common/Event.h"
#include "Common/IniFile.h"
#include "Common/FileUtil.h"
#include "Common/GL/GLContext.h"
#include "Common/Logging/LogManager.h"
#include "Common/Thread.h"
#include "Common/scmrev.h"
#include "Common/Version.h"
#include "Core/ActionReplay.h"
#include "Core/BootManager.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/GeckoCodeConfig.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/USB/Emulated/Microphone.h"
#include "Core/PatchEngine.h"
#include "Core/State.h"
#include "Core/System.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#ifdef __PROSPERO__
int Ps5DolphinDebugMode();  // VideoCommon/PixelShaderGen.cpp
unsigned Ps5DolphinFifoFrame();  // VideoCommon/PixelShaderGen.cpp
#endif
#include "DolphinLibretro/Audio.h"
#include "DolphinLibretro/Input.h"
#include "DolphinLibretro/Common/Options.h"
#include "DolphinLibretro/Video.h"
#include "VideoBackends/OGL/OGLTexture.h"
#include "VideoBackends/OGL/OGLGfx.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/TextureConfig.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/Widescreen.h"
#include "Core/Boot/Boot.h"

#ifdef PERF_TEST
static struct retro_perf_callback perf_cb;

#define RETRO_PERFORMANCE_INIT(name)                                                               \
  retro_perf_tick_t current_ticks;                                                                 \
  static struct retro_perf_counter name = {#name};                                                 \
  if (!name.registered)                                                                            \
    perf_cb.perf_register(&(name));                                                                \
  current_ticks = name.total

#define RETRO_PERFORMANCE_START(name) perf_cb.perf_start(&(name))
#define RETRO_PERFORMANCE_STOP(name)                                                               \
  perf_cb.perf_stop(&(name));                                                                      \
  current_ticks = name.total - current_ticks;
#else
#define RETRO_PERFORMANCE_INIT(name)
#define RETRO_PERFORMANCE_START(name)
#define RETRO_PERFORMANCE_STOP(name)
#endif

namespace Libretro
{
extern retro_environment_t environ_cb;
static bool widescreen;
double g_core_refresh_rate{0};
// Reset per game (see retro_run): the first refresh-rate settle after boot must not
// trigger a mode-family reinit, only a real in-game 50<->60 switch should.
static bool s_refresh_rate_settled = false;
extern void reload_cheats_from_ini();
extern unsigned msg_interface_version;
}  // namespace Libretro

#ifdef __PROSPERO__
// PS5 test aid: /app0/dolphin-options.txt, when present, holds key=value lines
// that answer RETRO_ENVIRONMENT_GET_VARIABLE for those keys instead of the
// frontend, so a test run sets core options without a person at the pad (the
// console's FTP server cannot reach the frontend's options folder). The file is
// never shipped, and the title deletes it on any launch that is not a test run.
namespace
{
retro_environment_t s_ps5_frontend_environ;
std::vector<std::pair<std::string, std::string>> s_ps5_option_overrides;

bool Ps5EnvironCallback(unsigned cmd, void* data)
{
  if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE && data)
  {
    auto* var = static_cast<retro_variable*>(data);
    for (const auto& [key, value] : s_ps5_option_overrides)
    {
      if (var->key && key == var->key)
      {
        var->value = value.c_str();
        return true;
      }
    }
  }
  return s_ps5_frontend_environ(cmd, data);
}

retro_environment_t Ps5WrapEnvironment(retro_environment_t cb)
{
  s_ps5_frontend_environ = cb;
  s_ps5_option_overrides.clear();
  std::ifstream file("/app0/dolphin-options.txt");
  std::string line;
  while (std::getline(file, line))
  {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    const size_t equals = line.find('=');
    if (equals == std::string::npos || equals == 0)
      continue;
    s_ps5_option_overrides.emplace_back(line.substr(0, equals), line.substr(equals + 1));
  }
  return s_ps5_option_overrides.empty() ? cb : Ps5EnvironCallback;
}
}  // namespace
#endif

extern "C" {

void retro_set_environment(retro_environment_t cb)
{
#ifdef __PROSPERO__
  cb = Ps5WrapEnvironment(cb);
#endif
  Libretro::environ_cb = cb;
#ifdef PERF_TEST
  environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
#endif
static const struct retro_subsystem_memory_info gbp_gba_memory[] = {
  {"srm", RETRO_MEMORY_SAVE_RAM},
};
static const struct retro_subsystem_rom_info gbp_roms[] = {
  {"GBA ROM", "gba|gbc|gb|zip|7z",
    true,
    true,
    false,
    gbp_gba_memory, 1},
  {"GameCube Disc", "rvz|gcm|iso|ciso|wbfs|gcz|tgc|m3u",
    true,
    false,
    true,
    nullptr, 0},
};
static const struct retro_subsystem_info subsystems[] = {
  {
    "Game Boy Player",
    "GBP",
    gbp_roms, 2,
    Libretro::g_gbplayer_subsystem_id
  },
  {nullptr, nullptr, nullptr, 0, 0}};
cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*)subsystems);
}

void retro_init(void)
{
  enum retro_pixel_format xrgb888 = RETRO_PIXEL_FORMAT_XRGB8888;
  Libretro::environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &xrgb888);

  Libretro::environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION,
    &Libretro::msg_interface_version);
}

void retro_deinit(void)
{
  Libretro::g_emuthread_launched = false;
#ifdef PERF_TEST
  perf_cb.perf_log();
#endif
}

void retro_get_system_info(retro_system_info* info)
{
  // Build a semver string from SCM_DESC_STR (e.g. "2512-226" -> "2512.0.226")
  // with the short git hash as build metadata (e.g. "+13192dec3d")
  static std::string version_str = []() {
    std::string desc = SCM_DESC_STR;
    std::string major, commits;
    auto dash = desc.find('-');
    if (dash != std::string::npos)
    {
      major = desc.substr(0, dash);
      auto dash2 = desc.find('-', dash + 1);
      commits = (dash2 != std::string::npos)
                    ? desc.substr(dash + 1, dash2 - dash - 1)
                    : desc.substr(dash + 1);
    }
    else
    {
      major = desc;
      commits = "0";
    }
    std::string hash = SCM_REV_STR;
    if (hash.size() > 10)
      hash = hash.substr(0, 10);
    return major + ".0." + commits + "+" + hash;
  }();

  info->need_fullpath = true;
  info->valid_extensions = "elf|dol|gcm|iso|tgc|wbfs|ciso|gcz|wad|wia|rvz|m3u";
  info->library_version = version_str.c_str();
  info->library_name = "dolphin-emu";
  info->block_extract = true;
}

void retro_get_system_av_info(retro_system_av_info* info)
{
  int efbScale = Libretro::Options::GetCached<int>(
    Libretro::Options::gfx_settings::EFB_SCALE);

  int base_height = Libretro::Video::GetAdjustedBaseHeight();

  // Fixed max (>= any runtime height) so base can grow via SET_GEOMETRY alone.
  int max_height = base_height > 576 ? base_height : 576;

  info->geometry.base_width  = EFB_WIDTH * efbScale;
  info->geometry.base_height = base_height * efbScale;

  info->geometry.max_width   = EFB_WIDTH * efbScale;
  info->geometry.max_height  = max_height * efbScale;

  if (g_widescreen)
    Libretro::widescreen = g_widescreen->IsGameWidescreen() || g_Config.bWidescreenHack;
  else if (Core::System::GetInstance().IsWii())
    Libretro::widescreen = Config::Get(Config::SYSCONF_WIDESCREEN);

  info->geometry.aspect_ratio = Libretro::widescreen ? 16.0 / 9.0 : 4.0 / 3.0;

  Core::System& system = Core::System::GetInstance();
  double fps = system.GetVideoInterface().GetTargetRefreshRate();
  if (fps <= 0.0)
    fps = (retro_get_region() == RETRO_REGION_NTSC) ? (60.0 / 1.001) : 50.0;
  info->timing.fps = fps;
  info->timing.sample_rate = Libretro::Audio::GetActiveSampleRate();
}

void retro_reset(void)
{
  Core::System::GetInstance().GetProcessorInterface().ResetButton_Tap();
}

#ifdef __PROSPERO__
// The FIFO-log debug mode (VideoCommon/PixelShaderGen.cpp, "fifo" or "fifo N"):
// 300 frames (or N) after launch, ten frames of GPU commands are recorded and saved as
// /app0/savefiles/ps5-fifo.dff, which desktop Dolphin plays back as a reference
// for the same frames. Test runs only.
static void Ps5FifoRecording()
{
  static unsigned frame = 0;
  if (Ps5DolphinDebugMode() != 2)
    return;
  ++frame;
  // The same frames dumped as images on the console, to compare with desktop
  // Dolphin's playback of the log (a couple of frames either side).
  const unsigned start = Ps5DolphinFifoFrame();
  if (frame + 2 >= start && frame < start + 12)
    Core::SaveScreenShot(fmt::format("ps5-frame-{}", frame));
  if (frame != start)
    return;
  auto& recorder = Core::System::GetInstance().GetFifoRecorder();
  recorder.StartRecording(10, [&recorder] {
    FifoDataFile* const file = recorder.GetRecordedFile();
    const bool saved = file != nullptr && file->Save("/app0/savefiles/ps5-fifo.dff");
    fprintf(stderr, "dolphin ps5: FIFO log of 10 frames %s\n", saved ? "saved" : "NOT saved");
  });
  fprintf(stderr, "dolphin ps5: FIFO recording started at frame %u\n", start);
}
#endif

// Inside the extern "C" block: C++ names, as Core.cpp and Fifo.cpp define them.
extern "C++"
{
namespace Core
{
extern std::atomic<long long> g_porpoise_cpu_ns;
extern std::atomic<long long> g_porpoise_cpu_max_ns;
extern std::atomic<unsigned> g_porpoise_cpu_frames;
}  // namespace Core
namespace Fifo
{
extern std::atomic<long long> g_porpoise_gpu_ns;
}  // namespace Fifo
namespace Vulkan
{
extern std::atomic<long long> g_porpoise_gpu_wait_ns;
}  // namespace Vulkan
namespace VideoCommon
{
extern std::atomic<long long> g_porpoise_shader_ns;
extern std::atomic<unsigned> g_porpoise_shaders;
}  // namespace VideoCommon
}
// Porpoise: the CPU and the GPU on one thread (a game Dolphin's database keeps on one
// core): the whole frame is timed as the CPU's.
static std::atomic<bool> s_porpoise_single_core{false};

// Porpoise: where a frame's time goes, since the last call. cpu_ms: the CPU thread's run of
// a frame step on average; cpu_max_ms: the longest; gpu_ms: the video loop's busy time per
// frame. Returns the frames counted.
extern "C" __attribute__((visibility("default"))) unsigned porpoise_perf_take(double* cpu_ms,
                                                                            double* cpu_max_ms,
                                                                            double* gpu_ms)
{
  const unsigned frames = Core::g_porpoise_cpu_frames.exchange(0, std::memory_order_relaxed);
  const long long cpu = Core::g_porpoise_cpu_ns.exchange(0, std::memory_order_relaxed);
  const long long cpu_max = Core::g_porpoise_cpu_max_ns.exchange(0, std::memory_order_relaxed);
  const long long gpu = Fifo::g_porpoise_gpu_ns.exchange(0, std::memory_order_relaxed);
  if (cpu_ms)
    *cpu_ms = frames ? cpu / 1e6 / frames : 0.0;
  if (cpu_max_ms)
    *cpu_max_ms = cpu_max / 1e6;
  if (gpu_ms)
    *gpu_ms = frames ? gpu / 1e6 / frames : 0.0;
  return frames;
}

// Porpoise 4.6: a game's own settings (Dolphin's Sys/GameSettings fixes, and the player's
// per-game ones in its LocalGame ini) win over the frontend's options, as they do at boot.
// The options were written straight into g_Config whenever any of them changed (and at the
// first frame), quietly undoing those fixes: Pokemon XD's safe texture cache, Eternal
// Darkness's XFB copies, Punch-Out!!'s texture cache.
extern "C++"
{
template <typename T>
static bool PorpoiseGameOwns(const Config::Info<T>& info)
{
  const Config::LayerType layer = Config::GetActiveLayerForConfig(info);
  return layer == Config::LayerType::GlobalGame || layer == Config::LayerType::LocalGame;
}
}

// Porpoise 4.6: everything above, and the PS5's GPU (time the video thread waited for it to
// finish a frame), the shaders made on the spot while the game waited, and whether the
// game runs on one core. out_size: sizeof what the frontend passes (fields only grow).
struct PorpoisePerf
{
  double cpu_ms, cpu_max_ms, gpu_ms, gpu_wait_ms, shader_ms;
  unsigned frames, shaders;
  int single_core;
};
extern "C" __attribute__((visibility("default"))) unsigned porpoise_perf_take2(PorpoisePerf* out,
                                                                             unsigned out_size)
{
  PorpoisePerf p{};
  const unsigned frames = porpoise_perf_take(&p.cpu_ms, &p.cpu_max_ms, &p.gpu_ms);
  const long long wait = Vulkan::g_porpoise_gpu_wait_ns.exchange(0, std::memory_order_relaxed);
  p.gpu_wait_ms = frames ? wait / 1e6 / frames : 0.0;
  p.shader_ms = VideoCommon::g_porpoise_shader_ns.exchange(0, std::memory_order_relaxed) / 1e6;
  p.shaders = VideoCommon::g_porpoise_shaders.exchange(0, std::memory_order_relaxed);
  p.frames = frames;
  p.single_core = s_porpoise_single_core.load(std::memory_order_relaxed) ? 1 : 0;
  if (out)
    std::memcpy(out, &p, std::min<std::size_t>(out_size, sizeof p));
  return frames;
}

void retro_run(void)
{
#ifdef __PROSPERO__
  Ps5FifoRecording();
#endif
  Libretro::Input::InitSensors();
  Libretro::Options::CheckForUpdatedVariables();
  Libretro::FrameTiming::CheckForFastForwarding();
  if (Libretro::Options::IsUpdated(Libretro::Options::main_interface::LOG_LEVEL))
  {
#if defined(_DEBUG)
    Common::Log::LogManager::GetInstance()->SetConfigLogLevel(Common::Log::LogLevel::LDEBUG);
#else
    Common::Log::LogManager::GetInstance()->SetConfigLogLevel(
      static_cast<Common::Log::LogLevel>(
        Libretro::Options::GetCached<int>(
          Libretro::Options::main_interface::LOG_LEVEL, static_cast<int>(Common::Log::LogLevel::LINFO))));
#endif
  }

  if (Libretro::Options::IsUpdated(Libretro::Options::core::CPU_CLOCK_RATE))
  {
    double cpuClock = Libretro::Options::GetCached<double>(
      Libretro::Options::core::CPU_CLOCK_RATE);
    Config::SetCurrent(Config::MAIN_OVERCLOCK, cpuClock);
    Config::SetCurrent(Config::MAIN_OVERCLOCK_ENABLE, cpuClock != 1.0);
  }

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_settings::WIDESCREEN_HACK))
  {
    g_Config.bWidescreenHack = Libretro::Options::GetCached<bool>(
      Libretro::Options::gfx_settings::WIDESCREEN_HACK);
  }

  // Crop to 4:3 when advertising 480-line geometry (NTSC / PAL60), tracked per frame.
  g_Config.bCropToAspectRatio = (Libretro::Video::GetAdjustedBaseHeight() == 480);

  Libretro::Input::Update();

  Core::System& system = Core::System::GetInstance();

  if (Core::GetState(system) == Core::State::Starting &&
      !Libretro::g_emuthread_launched)
  {
    WindowSystemInfo wsi(WindowSystemType::Libretro, nullptr, nullptr, nullptr);
    if (system.IsDualCoreMode())
    {
      Core::s_emu_thread = std::thread(Core::EmuThread,
        std::ref(system), std::move(Core::g_boot_params), wsi);

      // Wait until CPU thread has reached Run()
      auto& cpu_manager = system.GetCPU();
      // Porpoise: the boot can take a while (the disc's RetroAchievements hash
      // is read here, from a network share too): the frontend redraws its
      // launch screen meanwhile ("porpoise-wait"), so it doesn't look frozen.
      auto wait_from = std::chrono::steady_clock::now();
      while (!cpu_manager.HasCPURunStateBeenReached())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() - wait_from > std::chrono::milliseconds(100))
        {
          wait_from = std::chrono::steady_clock::now();
          retro_message msg = {"porpoise-wait", 0};
          Libretro::environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
        }
      }
    }
    else
      Core::EmuThread(system, std::move(Core::g_boot_params), wsi);

    Libretro::g_emuthread_launched = true;

    if(Config::Get(Config::MAIN_GFX_BACKEND) == "Software Renderer")
    {
      g_gfx.reset();
      g_gfx = std::make_unique<Libretro::Video::SWGfx>();
    }
    else if (Config::Get(Config::MAIN_GFX_BACKEND) == "Null")
    {
      g_gfx.reset();
      g_gfx = std::make_unique<Libretro::Video::NullGfx>();
    }

    while (!Core::IsRunningOrStarting(system))
      Common::SleepCurrentThread(100);

    Libretro::g_core_refresh_rate = system.GetVideoInterface().GetTargetRefreshRate();
    Libretro::s_refresh_rate_settled = false;

    // Expose GameCube and Wii memory maps to libretro
    {
      auto& memory = system.GetMemory();
      struct retro_memory_descriptor descs[2] = {};
      unsigned num_descs = 0;

      descs[num_descs].ptr = memory.GetRAM();
      descs[num_descs].start = 0x80000000;
      descs[num_descs].len = memory.GetRamSizeReal();
      descs[num_descs].flags = RETRO_MEMDESC_BIGENDIAN | RETRO_MEMORY_SYSTEM_RAM;
      descs[num_descs].addrspace = "MEM1";

      num_descs++;

      if (system.IsWii())
      {
        descs[num_descs].ptr = memory.GetEXRAM();
        descs[num_descs].start = 0x90000000;
        descs[num_descs].len = memory.GetExRamSizeReal();
        descs[num_descs].flags = RETRO_MEMDESC_BIGENDIAN | RETRO_MEMORY_SYSTEM_RAM;
        descs[num_descs].addrspace = "MEM2";
        num_descs++;
      }

      struct retro_memory_map mmap = {descs, num_descs};
      Libretro::environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmap);
    }
  }

  if(!Libretro::g_emuthread_launched)
  {
    DEBUG_LOG_FMT(BOOT, "retro_run() - waiting for g_emuthread_launched");
    return;
  }

  if (g_gfx && Config::Get(Config::MAIN_GFX_BACKEND) == "OGL")
  {
    static_cast<OGL::OGLGfx*>(g_gfx.get())
        ->SetSystemFrameBuffer((GLuint)Libretro::Video::hw_render.get_current_framebuffer());
  }

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_settings::EFB_SCALE))
  {
    g_Config.iEFBScale = Libretro::Options::GetCached<int>(
      Libretro::Options::gfx_settings::EFB_SCALE);

    unsigned cmd = RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO;
    if (Libretro::Video::hw_render.context_type == RETRO_HW_CONTEXT_D3D11 ||
        Libretro::Video::hw_render.context_type == RETRO_HW_CONTEXT_D3D12)
      cmd = RETRO_ENVIRONMENT_SET_GEOMETRY;
    retro_system_av_info info;
    retro_get_system_av_info(&info);
    Libretro::environ_cb(cmd, &info);
  }

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_settings::CROP_OVERSCAN))
  {
    retro_system_av_info info;
    retro_get_system_av_info(&info);
    // SET_GEOMETRY, not SET_SYSTEM_AV_INFO: the reinit would drop KMS to 240p.
    Libretro::environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info);
  }

  if (g_widescreen &&
      Libretro::widescreen != (g_widescreen->IsGameWidescreen() || g_Config.bWidescreenHack))
  {
    retro_system_av_info info;
    retro_get_system_av_info(&info);
    Libretro::environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info);
  }

  // target refresh rate has changed - e.g. user has chosen 60 Hz for a PAL game
  double new_rate = system.GetVideoInterface().GetTargetRefreshRate();

  if (round(Libretro::g_core_refresh_rate * 1e6) != round(new_rate * 1e6))
  {
    const double old_rate = Libretro::g_core_refresh_rate;

    NOTICE_LOG_FMT(VIDEO, "Target refresh rate changed: {:.4f} Hz -> {:.4f} Hz",
                   old_rate, new_rate);

    Libretro::g_core_refresh_rate = new_rate;

    // Re-pace the audio-callback frame timing to the new rate (PAL60 50->60).
    if (Libretro::FrameTiming::IsEnabled() && new_rate > 1.0)
      Libretro::FrameTiming::target_frame_duration_usec.store(
          static_cast<retro_usec_t>(lround(1e6 / new_rate)), std::memory_order_relaxed);

    retro_system_av_info info;
    retro_get_system_av_info(&info);

    // 50<->60 switch needs a new CRT mode family: only SET_SYSTEM_AV_INFO carries
    // the fps switchres uses, then SET_GEOMETRY rebuilds the surface its reinit drops.
    // First settle after boot keeps the initial mode, so geometry-only (no reinit flash).
    const bool crosses_50_60 =
      Libretro::s_refresh_rate_settled && ((old_rate > 55.0) != (new_rate > 55.0));
    Libretro::s_refresh_rate_settled = true;

    if (crosses_50_60)
      Libretro::environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);
    Libretro::environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &info);
  }

  WiimoteUpdateFlags flags;

  if (system.IsWii())
  {
    flags.irMode  = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_MODE);
    flags.irOffset = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_OFFSET);
    flags.irYaw = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_YAW);
    flags.irPitch = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_PITCH);
    flags.irDeadzone = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_DEADZONE);
    flags.irModifier = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_MODIFIER);
    flags.swingModifier = Libretro::Options::IsUpdated(Libretro::Options::wiimote::SWING_MODIFIER);
    flags.swingAngle = Libretro::Options::IsUpdated(Libretro::Options::wiimote::SWING_ANGLE);
    flags.sideways = Libretro::Options::IsUpdated(Libretro::Options::wiimote::HOTKEY_SIDEWAYS_TOGGLE);
    flags.upright = Libretro::Options::IsUpdated(Libretro::Options::wiimote::HOTKEY_UPRIGHT_TOGGLE);
    flags.irPassthrough = Libretro::Options::IsUpdated(Libretro::Options::wiimote::IR_PASSTHROUGH);
  }

  flags.rumble = Libretro::Options::IsUpdated(Libretro::Options::sysconf::ENABLE_RUMBLE);
  flags.gcMicBtn = Libretro::Options::IsUpdated(Libretro::Options::sysconf_gc::HOTKEY_ACTIVATE_MICROPHONE);

  if (flags.any())
    Libretro::Input::ResetControllers(flags);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_settings::ASPECT_RATIO))
    Config::SetCurrent(
      Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(
        Libretro::GetOption<int>(
          Libretro::Options::gfx_settings::ASPECT_RATIO, static_cast<int>(AspectMode::Stretch)
        )
      )
    );

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_TO_TEXTURE))
    if (!PorpoiseGameOwns(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM))
      g_Config.bSkipEFBCopyToRam = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::EFB_TO_TEXTURE, true);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_ACCESS_ENABLE))
    if (!PorpoiseGameOwns(Config::GFX_HACK_EFB_ACCESS_ENABLE))
      g_Config.bEFBAccessEnable = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::EFB_ACCESS_ENABLE, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_ACCESS_DEFER_INVALIDATION))
    if (!PorpoiseGameOwns(Config::GFX_HACK_EFB_DEFER_INVALIDATION))
      g_Config.bEFBAccessDeferInvalidation = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::EFB_ACCESS_DEFER_INVALIDATION, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_ACCESS_TILE_SIZE))
    if (!PorpoiseGameOwns(Config::GFX_HACK_EFB_ACCESS_TILE_SIZE))
      g_Config.iEFBAccessTileSize = Libretro::Options::GetCached<int>(Libretro::Options::gfx_hacks::EFB_ACCESS_TILE_SIZE, 64);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::BBOX_ENABLED))
    if (!PorpoiseGameOwns(Config::GFX_HACK_BBOX_ENABLE))
      g_Config.bBBoxEnable = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::BBOX_ENABLED, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::XFB_TO_TEXTURE_ENABLE))
    if (!PorpoiseGameOwns(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM))
      g_Config.bSkipXFBCopyToRam = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::XFB_TO_TEXTURE_ENABLE, true);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_TO_VRAM))
    if (!PorpoiseGameOwns(Config::GFX_HACK_DISABLE_COPY_TO_VRAM))
      g_Config.bDisableCopyToVRAM = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::EFB_TO_VRAM, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::DEFER_EFB_COPIES))
    if (!PorpoiseGameOwns(Config::GFX_HACK_DEFER_EFB_COPIES))
      g_Config.bDeferEFBCopies = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::DEFER_EFB_COPIES, true);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::IMMEDIATE_XFB))
    if (!PorpoiseGameOwns(Config::GFX_HACK_IMMEDIATE_XFB))
      g_Config.bImmediateXFB = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::IMMEDIATE_XFB, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::SKIP_DUPE_FRAMES))
    if (!PorpoiseGameOwns(Config::GFX_HACK_SKIP_DUPLICATE_XFBS))
      g_Config.bSkipPresentingDuplicateXFBs = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::SKIP_DUPE_FRAMES, true);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_SCALED_COPY))
    if (!PorpoiseGameOwns(Config::GFX_HACK_COPY_EFB_SCALED))
      g_Config.bCopyEFBScaled = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::EFB_SCALED_COPY, true);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::EFB_EMULATE_FORMAT_CHANGES))
    if (!PorpoiseGameOwns(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES))
      g_Config.bEFBEmulateFormatChanges = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::EFB_EMULATE_FORMAT_CHANGES, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::VERTEX_ROUNDING))
    if (!PorpoiseGameOwns(Config::GFX_HACK_VERTEX_ROUNDING))
      g_Config.bVertexRounding = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::VERTEX_ROUNDING, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::VI_SKIP))
    if (!PorpoiseGameOwns(Config::GFX_HACK_VI_SKIP))
      g_Config.bVISkip = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::VI_SKIP, false);

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::FAST_TEXTURE_SAMPLING))
    if (!PorpoiseGameOwns(Config::GFX_HACK_FAST_TEXTURE_SAMPLING))
      g_Config.bFastTextureSampling = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::FAST_TEXTURE_SAMPLING, true);

#ifdef __APPLE__
  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_hacks::NO_MIPMAPPING))
    if (!PorpoiseGameOwns(Config::GFX_HACK_NO_MIPMAPPING))
      g_Config.bNoMipmapping = Libretro::Options::GetCached<bool>(Libretro::Options::gfx_hacks::NO_MIPMAPPING, false);
#endif

  if (Libretro::Options::IsUpdated(Libretro::Options::gfx_settings::TEXTURE_CACHE_ACCURACY))
    if (!PorpoiseGameOwns(Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES))
      g_Config.iSafeTextureCache_ColorSamples = Libretro::Options::GetCached<int>(Libretro::Options::gfx_settings::TEXTURE_CACHE_ACCURACY, 128);

  if (Libretro::Options::IsUpdated(Libretro::Options::sysconf::WII_SPEAK_MUTED))
    Config::SetCurrent(Config::MAIN_WII_SPEAK_MUTED,
      Libretro::Options::GetCached<bool>(Libretro::Options::sysconf::WII_SPEAK_MUTED));

  if (Libretro::Options::IsUpdated(Libretro::Options::sysconf::WIIMOTE_CONTINUOUS_SCANNING))
  {
    Config::SetCurrent(Config::MAIN_WIIMOTE_CONTINUOUS_SCANNING,
      Libretro::Options::GetCached<bool>(Libretro::Options::sysconf::WIIMOTE_CONTINUOUS_SCANNING));
    WiimoteReal::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
  }

  if (Config::Get(Config::MAIN_BLUETOOTH_PASSTHROUGH_ENABLED))
    Libretro::Input::BluetoothPassthroughBind();

  RETRO_PERFORMANCE_INIT(dolphin_main_func);
  RETRO_PERFORMANCE_START(dolphin_main_func);

  Libretro::Video::BeginRun();
  if (system.IsDualCoreMode())
  {
    Core::DoFrameStep(system);
    system.GetFifo().RunGpuLoop();
  }
  else
  {
    const auto porpoise_from = std::chrono::steady_clock::now();
    system.GetCPU().RunSingleFrame();
    const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - porpoise_from)
                             .count();
    Core::g_porpoise_cpu_ns.fetch_add(ns, std::memory_order_relaxed);
    if (ns > Core::g_porpoise_cpu_max_ns.load(std::memory_order_relaxed))
      Core::g_porpoise_cpu_max_ns.store(ns, std::memory_order_relaxed);
    Core::g_porpoise_cpu_frames.fetch_add(1, std::memory_order_relaxed);
    s_porpoise_single_core.store(true, std::memory_order_relaxed);
  }
  Libretro::Video::EndRun();

  RETRO_PERFORMANCE_STOP(dolphin_main_func);

  if (auto* sound_stream = system.GetSoundStream())
  {
    auto* libretro_stream = static_cast<Libretro::Audio::Stream*>(sound_stream);
    if (libretro_stream)
    {
      libretro_stream->PushAudioForFrame();
    }
  }

  if (Libretro::Input::g_has_microphone_support)
    poll_microphone();
}

size_t retro_serialize_size(void)
{
  size_t size = 0;

  Core::System& system = Core::System::GetInstance();
  AsyncRequests* ar = AsyncRequests::GetInstance();

  if (system.IsDualCoreMode())
    ar->SetPassthrough(true);

  Core::RunOnCPUThread(Core::System::GetInstance(), [&] {
    PointerWrap p((u8**)&size, sizeof(size_t), PointerWrap::Mode::Measure);
    State::DoState(Core::System::GetInstance(), p);
    }, true);  // wait = true

  if (system.IsDualCoreMode())
    ar->SetPassthrough(false);

  return size;
}

bool retro_serialize(void* data, size_t size)
{
  Core::System& system = Core::System::GetInstance();
  AsyncRequests* ar = AsyncRequests::GetInstance();

  if (system.IsDualCoreMode())
    ar->SetPassthrough(true);

  const bool was_cpu = Core::IsCPUThread();
  if (!was_cpu)
    Core::DeclareAsCPUThread();

  Core::RunOnCPUThread(Core::System::GetInstance(), [&] {
    PointerWrap p((u8**)&data, size, PointerWrap::Mode::Write);
    State::DoState(Core::System::GetInstance(), p);
  }, true);

  if (!was_cpu)
    Core::UndeclareAsCPUThread();

  if (system.IsDualCoreMode())
    ar->SetPassthrough(false);

  return true;
}

bool retro_unserialize(const void* data, size_t size)
{
  Core::System& system = Core::System::GetInstance();
  AsyncRequests* ar = AsyncRequests::GetInstance();

  if (system.IsDualCoreMode())
    ar->SetPassthrough(true);

  const bool was_cpu = Core::IsCPUThread();
  if (!was_cpu)
    Core::DeclareAsCPUThread();

  // Porpoise: a state that doesn't fit (another version, or 2609a's FIFO
  // check) ends in measure mode: the frontend is told it wasn't loaded.
  bool loaded = true;
  Core::RunOnCPUThread(Core::System::GetInstance(), [&] {
    PointerWrap p((u8**)&data, size, PointerWrap::Mode::Read);
    State::DoState(Core::System::GetInstance(), p);
    loaded = p.IsReadMode();
  }, true);

  if (!was_cpu)
    Core::UndeclareAsCPUThread();

  if (system.IsDualCoreMode())
    ar->SetPassthrough(false);

  return loaded;
}

unsigned retro_get_region()
{
  Core::System& system = Core::System::GetInstance();

  if (DiscIO::IsNTSC(SConfig::GetInstance().m_region) ||
      (Core::System::GetInstance().IsWii() && Config::Get(Config::SYSCONF_PAL60)) ||
       system.IsTriforce())
    return RETRO_REGION_NTSC;

  return RETRO_REGION_PAL;
}

unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

size_t retro_get_memory_size(unsigned id)
{
  auto& memory = Core::System::GetInstance().GetMemory();
  if (id == RETRO_MEMORY_SYSTEM_RAM)
    return memory.GetRamSizeReal();
  return 0;
}

void* retro_get_memory_data(unsigned id)
{
  auto& memory = Core::System::GetInstance().GetMemory();
  if (id == RETRO_MEMORY_SYSTEM_RAM)
    return memory.GetRAM();
  return NULL;
}

static void enable_cheat_by_code(bool enabled, const char* code)
{
  std::string incoming(code);
  bool found = false;

  // Match Action Replay codes
  for (auto& c : Libretro::g_ar_codes)
  {
    std::string serialized;
    serialized.reserve(64);

    for (const auto& op : c.ops)
    {
      if (!serialized.empty())
          serialized += "+";

      serialized += fmt::format("{:08X} {:08X}", op.cmd_addr, op.value);
    }

    if (serialized == incoming)
    {
      c.enabled = enabled;
      found = true;
      break;
    }
  }

  // Match Gecko codes only if not already matched
  if (!found)
  {
    for (auto& c : Libretro::g_gecko_codes)
    {
      std::string serialized;
      serialized.reserve(64);

      for (const auto& line : c.codes)
      {
        if (!serialized.empty())
            serialized += "+";

        serialized += line.original_line;
      }

      if (serialized == incoming)
      {
        c.enabled = enabled;
        found = true;
        break;
      }
    }
  }
}

// reset cheats to off
void retro_cheat_reset(void)
{
  const bool importCheats = Libretro::GetOption<bool>(Libretro::Options::retroarch_core::CHEATS_IMPORT, true);

  if (!Config::AreCheatsEnabled() || !importCheats)
    return;

  Core::RunOnCPUThread(Core::System::GetInstance(), [&] {
    PatchEngine::Shutdown();
  }, true);

  Libretro::reload_cheats_from_ini();
}

// Enable/disable a cheat
void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
  if (!Config::AreCheatsEnabled())
  {
    OSD::AddMessage("You cannot apply any cheats until you have enabled the following option:\n"
      "Core > Internal Cheats - set this to ON", OSD::Duration::VERY_LONG, OSD::Color::RED);
    return;
  }

  const bool importCheats = Libretro::GetOption<bool>(Libretro::Options::retroarch_core::CHEATS_IMPORT, true);
  if (!importCheats)
  {
    OSD::AddMessage("You cannot apply any cheats until you enabled the following option:\n"
      "Core > Automatically Import Cheats into RetroArch - set this to ON", OSD::Duration::VERY_LONG, OSD::Color::RED);
    return;
  }

  const std::string& game_id = SConfig::GetInstance().GetGameID();
  const u16 game_revision = SConfig::GetInstance().GetRevision();

  // Look up by code as index is unreliable
  enable_cheat_by_code(enabled, code);

  Core::RunOnCPUThread(Core::System::GetInstance(), [&] {
    // Apply Action Replay
    ActionReplay::ApplyCodes(Libretro::g_ar_codes, game_id, game_revision);

    // Apply Gecko
    Gecko::SetActiveCodes(Libretro::g_gecko_codes, game_id, game_revision);
  }, true);
}
} // extern "C"

// Porpoise's Performance report: the host addresses the sampler found busiest
// on the emulated CPU's thread, told as the game's own code (PowerPC) - which
// JIT block each falls in, its address and its instructions - so a slow game's
// hot loop can be read. Called from the frontend's thread between frames; the
// CPU thread is held while the block table is read.
#include <algorithm>
#include "Common/GekkoDisassembler.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

extern "C" __attribute__((visibility("default"))) void
porpoise_jit_describe(const unsigned long long* rips, const unsigned* counts, int n)
{
  if (!rips || !counts || n <= 0 || !Core::IsRunning(Core::System::GetInstance()))
    return;
  Core::System& system = Core::System::GetInstance();
  // The CPU thread is held only while the report is gathered; it is written
  // after (a report written while held cost the game a quarter second).
  std::optional<Core::CPUThreadGuard> held(std::in_place, system);
  const Core::CPUThreadGuard& guard = *held;
  std::string report;
  const auto say = [&report](const char* format, auto... args) {
    char line[512];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#pragma clang diagnostic ignored "-Wformat-security"
    const int length = std::snprintf(line, sizeof(line), format, args...);
#pragma clang diagnostic pop
    if (length > 0)
      report.append(line, std::min<std::size_t>(std::size_t(length), sizeof(line) - 1));
  };
  struct Hot
  {
    u32 address = 0, size = 0;
    unsigned samples = 0;
    const u8* begin = nullptr;
    const u8* end = nullptr;
  };
  std::vector<Hot> hot;
  unsigned outside = 0;
  // Every block's host code once (near and far), sorted, then each address
  // looked up in it: one pass over the blocks rather than one per address.
  struct Span
  {
    const u8* begin;
    const u8* end;
    u32 address, size;
    const u8* near_begin;
    const u8* near_end;
  };
  std::vector<Span> spans;
  system.GetJitInterface().RunOnBlocks(guard, [&](const JitBlock& b) {
    if (b.near_end > b.near_begin)
      spans.push_back({b.near_begin, b.near_end, b.effectiveAddress, b.originalSize, b.near_begin,
                       b.near_end});
    if (b.far_end > b.far_begin)
      spans.push_back({b.far_begin, b.far_end, b.effectiveAddress, b.originalSize, b.near_begin,
                       b.near_end});
  });
  std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
  for (int i = 0; i < n; ++i)
  {
    const u8* const rip = reinterpret_cast<const u8*>(static_cast<uintptr_t>(rips[i]));
    auto it = std::upper_bound(spans.begin(), spans.end(), rip,
                               [](const u8* r, const Span& span) { return r < span.begin; });
    if (it == spans.begin() || rip >= (it - 1)->end)
    {
      outside += counts[i];
      continue;
    }
    const Span& b = *(it - 1);
    bool merged = false;
    for (Hot& h : hot)
      if (h.address == b.address)
      {
        h.samples += counts[i];
        merged = true;
        break;
      }
    if (!merged)
      hot.push_back({b.address, b.size, counts[i], b.near_begin, b.near_end});
  }
  std::sort(hot.begin(), hot.end(), [](const Hot& a, const Hot& b) { return a.samples > b.samples; });
  say("jit: %zu hot blocks; %u samples outside any block\n", hot.size(), outside);
  for (std::size_t k = 0; k < hot.size() && k < 8; ++k)
  {
    const Hot& h = hot[k];
    say("jit: block %08x, %u instructions, host %p..%p, samples %u\n", h.address,
                 h.size, static_cast<const void*>(h.begin), static_cast<const void*>(h.end),
                 h.samples);
    for (u32 i = 0; i < h.size && i < 64; ++i)
    {
      const u32 at = h.address + i * 4;
      const u32 op = PowerPC::MMU::HostRead_Instruction(guard, at);
      say("jit:   %08x  %08x  %s\n", at, op,
                   Common::GekkoDisassembler::Disassemble(op, at).c_str());
    }
    /* 3.0: the block's own machine code, for reading offline (the two
     * hottest), 32 bytes a line. */
    if (k < 2 && h.begin && h.end > h.begin && h.end - h.begin < 16384)
      for (const u8* p = h.begin; p < h.end; p += 32)
      {
        say("jit: host %p ", static_cast<const void*>(p));
        for (const u8* q = p; q < p + 32 && q < h.end; ++q)
          say("%02x", *q);
        say("\n");
      }
  }
  /* The whole game function around each of the three hottest blocks, once a
   * session: from its stack-frame prologue (stwu r1) to the first blr after
   * the block, so a hot routine can be read in full offline. */
  {
    static std::vector<u32> s_dumped;
    for (std::size_t k = 0; k < hot.size() && k < 3; ++k)
    {
      const u32 at = hot[k].address;
      u32 start = 0;
      for (u32 back = 0; back < 0x1000 && at >= back * 4; ++back)
      {
        const u32 a = at - back * 4;
        if (!PowerPC::MMU::HostIsInstructionRAMAddress(guard, a))
          break;
        if ((PowerPC::MMU::HostRead_Instruction(guard, a) & 0xFFFF8000u) == 0x94218000u)
        {
          start = a;
          break;
        }
      }
      if (!start)
        start = at >= 0x400 ? at - 0x400 : at;
      if (std::find(s_dumped.begin(), s_dumped.end(), start) != s_dumped.end())
        continue;
      s_dumped.push_back(start);
      u32 end = start;
      bool past = false;
      for (u32 i = 0; i < 0x1000; ++i)
      {
        end = start + i * 4;
        if (!PowerPC::MMU::HostIsInstructionRAMAddress(guard, end))
          break;
        if (end >= at)
          past = true;
        if (past && PowerPC::MMU::HostRead_Instruction(guard, end) == 0x4E800020u)
          break;
      }
      say("jit: function %08x..%08x around block %08x\n", start, end, at);
      for (u32 a = start; a <= end; a += 4)
      {
        const u32 op = PowerPC::MMU::HostRead_Instruction(guard, a);
        say("jit:   f %08x  %08x  %s\n", a, op,
                     Common::GekkoDisassembler::Disassemble(op, a).c_str());
      }
    }
  }
  /* The quantizers and the paired-single state the hot code runs with. */
  {
    const auto& ppc = system.GetPPCState();
    say("jit: gqr");
    for (int i = 0; i < 8; ++i)
      say(" %08x", ppc.spr[SPR_GQR0 + i]);
    say(" hid2 %08x msr %08x\n", ppc.spr[SPR_HID2], ppc.msr.Hex);
  }
  held.reset();
  std::fputs(report.c_str(), stderr);
  std::fflush(stderr);
}
