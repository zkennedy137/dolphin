// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/MemArena.h"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#ifdef __PROSPERO__
// PS5: guest memory is direct memory, through the platform layer (my payload SDK
// fork, ps5platform/shm.h), rather than a shared-memory object. Every view of a
// shared-memory object was charged to the title's flexible memory (448 MiB for
// the whole title), and Wind Waker's fake-VMEM mirrors - one 32 MiB region
// mapped at up to 28 logical addresses - exhausted it: the mappings failed and a
// fastmem access faulted through a missing view (2026-09-24). A direct-memory
// object is charged to nothing but the 12 GiB direct pool, and can be mapped at
// any number of addresses, which is what a mirror is.
namespace
{
int ps5_protection(bool writeable)
{
  return PS5_SHM_READ | (writeable ? PS5_SHM_WRITE : 0);
}
}  // namespace
#endif

namespace Common
{
MemArena::MemArena() = default;
MemArena::~MemArena() = default;

void MemArena::GrabSHMSegment(size_t size, std::string_view base_name)
{
#ifdef __PROSPERO__
  const int result = ps5_shm_create(size, &m_shm);
  if (result != 0)
    PanicAlertFmt("PS5: allocating {:#x} bytes of direct memory for guest RAM failed: {:#010x}",
                  size, static_cast<u32>(result));
  return;
#endif
  const std::string file_name = fmt::format("/{}.{}", base_name, getpid());
  m_shm_fd = shm_open(file_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (m_shm_fd == -1)
  {
    ERROR_LOG_FMT(MEMMAP, "shm_open failed: {}", strerror(errno));
    return;
  }
  shm_unlink(file_name.c_str());
  if (ftruncate(m_shm_fd, size) < 0)
    ERROR_LOG_FMT(MEMMAP, "Failed to allocate low memory space");
}

void MemArena::ReleaseSHMSegment()
{
#ifdef __PROSPERO__
  ps5_shm_destroy(&m_shm);
  return;
#endif
  close(m_shm_fd);
}

void* MemArena::CreateView(s64 offset, size_t size)
{
#ifdef __PROSPERO__
  // Anywhere outside the Vulkan driver's GPU window.
  void* view = nullptr;
  const int result = ps5_shm_map(&m_shm, static_cast<size_t>(offset), size, nullptr,
                                 ps5_protection(true), 0, &view);
  if (result != 0)
  {
    ERROR_LOG_FMT(MEMMAP, "PS5: mapping a view of {:#x} bytes at offset {:#x} failed: {:#010x}",
                  size, offset, static_cast<u32>(result));
    return nullptr;
  }
  return view;
#endif
  void* retval = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, m_shm_fd, offset);
  if (retval == MAP_FAILED)
  {
    NOTICE_LOG_FMT(MEMMAP, "mmap failed");
    return nullptr;
  }
  else
  {
    return retval;
  }
}

void MemArena::ReleaseView(void* view, size_t size)
{
#ifdef __PROSPERO__
  ps5_shm_unmap(view, size, 0);
  return;
#endif
  munmap(view, size);
}

u8* MemArena::ReserveMemoryRegion(size_t memory_size)
{
#ifdef __PROSPERO__
  // PS5: an anonymous PROT_NONE mapping of this size is charged to the title's
  // small flexible-memory budget and fails. The platform layer reserves the
  // range without backing it, outside the Vulkan driver's GPU window, and the
  // views are mapped into it at fixed addresses as upstream does.
  //
  // RetroArch unloads a core when content closes. A reservation an earlier
  // instance left behind (a core that did not reach ReleaseMemoryRegion) holds
  // the hint; the range is this arena's alone, so it is released and asked for
  // again rather than taken elsewhere.
  void* const ps5_arena_hint = reinterpret_cast<void*>(0x10'0000'0000);
  void* address = nullptr;
  int result = ps5_vrange_reserve(memory_size, ps5_arena_hint, 0x10000, &address);
  if (result == 0 && address != ps5_arena_hint)
  {
    ps5_vrange_release(address, memory_size);
    ps5_vrange_release(ps5_arena_hint, memory_size);
    result = ps5_vrange_reserve(memory_size, ps5_arena_hint, 0x10000, &address);
  }
  if (result != 0)
  {
    PanicAlertFmt("PS5: reserving {:#x} bytes of address space failed: {:#010x}", memory_size,
                  static_cast<u32>(result));
    return nullptr;
  }
  INFO_LOG_FMT(MEMMAP, "PS5: fastmem arena reserved at {} ({:#x} bytes)", address, memory_size);
  m_reserved_region = address;
  m_reserved_region_size = memory_size;
  return static_cast<u8*>(address);
#endif
  const int flags = MAP_ANON | MAP_PRIVATE;
  void* base = mmap(nullptr, memory_size, PROT_NONE, flags, -1, 0);
  if (base == MAP_FAILED)
  {
    PanicAlertFmt("Failed to map enough memory space: {}", LastStrerrorString());
    return nullptr;
  }
  m_reserved_region = base;
  m_reserved_region_size = memory_size;
  return static_cast<u8*>(base);
}

void MemArena::ReleaseMemoryRegion()
{
  if (m_reserved_region)
  {
#ifdef __PROSPERO__
    ps5_vrange_release(m_reserved_region, m_reserved_region_size);
#else
    munmap(m_reserved_region, m_reserved_region_size);
#endif
    m_reserved_region = nullptr;
  }
}

void* MemArena::MapInMemoryRegion(s64 offset, size_t size, void* base, bool writeable)
{
#ifdef __PROSPERO__
  // Fixed, over the reservation (ReserveMemoryRegion) or a view unmapped from it.
  void* view = nullptr;
  const int result = ps5_shm_map(&m_shm, static_cast<size_t>(offset), size, base,
                                 ps5_protection(writeable), PS5_SHM_FIXED, &view);
  if (result != 0)
  {
    ERROR_LOG_FMT(MEMMAP, "PS5: mapping {:#x} bytes of guest RAM at {} failed: {:#010x}", size,
                  base, static_cast<u32>(result));
    return nullptr;
  }
  return view;
#endif
  int prot = PROT_READ;
  if (writeable)
    prot |= PROT_WRITE;

  void* retval = mmap(base, size, prot, MAP_SHARED | MAP_FIXED, m_shm_fd, offset);
  if (retval == MAP_FAILED)
  {
    ERROR_LOG_FMT(MEMMAP, "mmap of {:#x} bytes at {} (offset {:#x}) failed: {}", size, base,
                  offset, strerror(errno));
    return nullptr;
  }
  else
  {
    return retval;
  }
}

bool MemArena::ChangeMappingProtection(void* view, size_t size, bool writeable)
{
  int prot = PROT_READ;
  if (writeable)
    prot |= PROT_WRITE;

  int retval = mprotect(view, size, prot);
  if (retval != 0)
    NOTICE_LOG_FMT(MEMMAP, "mprotect failed");
  return retval == 0;
}

void MemArena::UnmapFromMemoryRegion(void* view, size_t size)
{
#ifdef __PROSPERO__
  // The hole stays reserved, so nothing else is mapped into the arena.
  if (ps5_shm_unmap(view, size, PS5_SHM_KEEP_RESERVED) != 0)
    ERROR_LOG_FMT(MEMMAP, "PS5: unmapping the view at {} failed", view);
  return;
#endif
  void* retval = mmap(view, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  if (retval == MAP_FAILED)
    ERROR_LOG_FMT(MEMMAP, "unmapping {:#x} bytes at {} failed: {}", size, view, strerror(errno));
}

size_t MemArena::GetPageSize() const
{
  return sysconf(_SC_PAGESIZE);
}

LazyMemoryRegion::LazyMemoryRegion() = default;

LazyMemoryRegion::~LazyMemoryRegion()
{
  Release();
}

#if !defined MAP_NORESERVE && (defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__)
// BSD does not implement MAP_NORESERVE, so define the flag to nothing.
// See https://reviews.freebsd.org/rS273250
#define MAP_NORESERVE 0
#endif

void* LazyMemoryRegion::Create(size_t size)
{
  ASSERT(!m_memory);

  if (size == 0)
    return nullptr;

  void* memory = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (memory == MAP_FAILED)
  {
    NOTICE_LOG_FMT(MEMMAP, "Memory allocation of {} bytes failed.", size);
    return nullptr;
  }

  m_memory = memory;
  m_size = size;

  return memory;
}

void LazyMemoryRegion::Clear()
{
  ASSERT(m_memory);

  void* new_memory = mmap(m_memory, m_size, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
  ASSERT(new_memory == m_memory);
}

void LazyMemoryRegion::Release()
{
  if (m_memory)
  {
    munmap(m_memory, m_size);
    m_memory = nullptr;
    m_size = 0;
  }
}

}  // namespace Common
