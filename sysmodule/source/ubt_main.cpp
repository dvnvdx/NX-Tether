/*
 * usb-tether: USB tethering bridge for Horizon OS.
 *
 * The Switch only has drivers for a couple of USB LAN adapter chips, and a tethered phone
 * (RNDIS / CDC-NCM) is not one of them. This module talks to the phone itself, and shows the
 * system's ethernet driver (the "eth" system module) a software ASIX AX88772 LAN adapter by
 * intercepting that driver's usb:hs session. Ethernet frames are carried between the two.
 *
 * Structure follows MissionControl (ndeadly), which intercepts system services the same way.
 */
#include "ubt_common.hpp"
#include "ubt_config.hpp"
#include "ubt_log.hpp"
#include "ubt_guard.hpp"
#include "ubt_usbhs_mitm.hpp"

namespace ams {

    namespace ubt {

        namespace {

            alignas(0x40) constinit u8 g_heap_memory[128_KB];
            constinit lmem::HeapHandle g_heap_handle;
            constinit bool g_heap_initialized;
            constinit os::SdkMutex g_heap_init_mutex;

            lmem::HeapHandle GetHeapHandle() {
                if (AMS_UNLIKELY(!g_heap_initialized)) {
                    std::scoped_lock lk(g_heap_init_mutex);

                    if (AMS_LIKELY(!g_heap_initialized)) {
                        g_heap_handle = lmem::CreateExpHeap(g_heap_memory, sizeof(g_heap_memory), lmem::CreateOption_ThreadSafe);
                        g_heap_initialized = true;
                    }
                }

                return g_heap_handle;
            }

            void *Allocate(size_t size) {
                return lmem::AllocateFromExpHeap(GetHeapHandle(), size);
            }

            void *AllocateWithAlign(size_t size, size_t align) {
                return lmem::AllocateFromExpHeap(GetHeapHandle(), size, align);
            }

            void Deallocate(void *p, size_t size) {
                AMS_UNUSED(size);
                return lmem::FreeToExpHeap(GetHeapHandle(), p);
            }

            const char *ModeName(u8 mode) {
                switch (mode) {
                    case Mode_Off:      return "off";
                    case Mode_LinkTest: return "phone link test";
                    case Mode_Bridge:   return "full bridge";
                    default:            return "?";
                }
            }

        }

    }

    namespace init {

        void InitializeSystemModule() {
            R_ABORT_UNLESS(sm::Initialize());

            fs::InitializeForSystem();
            fs::SetAllocator(ubt::Allocate, ubt::Deallocate);
            fs::SetEnabledAutoAbort(false);

            R_ABORT_UNLESS(pmdmntInitialize());

            R_ABORT_UNLESS(fs::MountSdCard("sdmc"));
        }

        void FinalizeSystemModule() { /* ... */ }

        void Startup() {
            ubt::log::Initialize();
            ubt::config::Load();
            ubt::log::SetLevel(ubt::config::Get().log_level);

            /* If the previous run went down while bridging, do not try again by ourselves. */
            if (ubt::guard::CheckAtStartup() && ubt::config::Get().mode == ubt::Mode_Bridge) {
                UBT_LOG_E("guard: the previous boot did not survive bridge mode. Switching to link-test mode (mode=1).");
                UBT_LOG_E("guard: re-enable the bridge from the overlay (\"Mode after reboot\") or config.ini when ready to retry.");
                R_DISCARD(ubt::config::SaveMode(ubt::Mode_LinkTest));
                ubt::config::SetRunningMode(ubt::Mode_LinkTest);
            }
            if (ubt::config::Get().mode == ubt::Mode_Bridge) {
                ubt::guard::Arm();
            }
        }

    }

    void Main() {
        const u32 fw = static_cast<u32>(hos::GetVersion());
        UBT_LOG_I("============================================================");
        UBT_LOG_I("usb-tether " UBT_VERSION_STRING " starting. firmware %u.%u.%u, mode %u (%s), log level %d",
                  static_cast<unsigned>((fw >> 24) & 0xFF), static_cast<unsigned>((fw >> 16) & 0xFF), static_cast<unsigned>((fw >> 8) & 0xFF),
                  ubt::config::Get().mode, ubt::ModeName(ubt::config::Get().mode), ubt::config::Get().log_level);

        /* Install the usb:hs interception as early as possible: until it is in place, every */
        /* usb:hs client in the system is waiting on us.                                      */
        ubt::mitm::Launch();

        /* Everything else happens on worker threads. */
        while (true) {
            os::SleepThread(TimeSpan::FromSeconds(3600));
        }
    }

}

void *operator new(size_t size) {
    return ams::ubt::Allocate(size);
}

void *operator new(size_t size, const std::nothrow_t &) {
    return ams::ubt::Allocate(size);
}

void operator delete(void *p) {
    return ams::ubt::Deallocate(p, 0);
}

void operator delete(void *p, size_t size) {
    return ams::ubt::Deallocate(p, size);
}

void *operator new[](size_t size) {
    return ams::ubt::Allocate(size);
}

void *operator new[](size_t size, const std::nothrow_t &) {
    return ams::ubt::Allocate(size);
}

void operator delete[](void *p) {
    return ams::ubt::Deallocate(p, 0);
}

void operator delete[](void *p, size_t size) {
    return ams::ubt::Deallocate(p, size);
}

void *operator new(size_t size, std::align_val_t align) {
    return ams::ubt::AllocateWithAlign(size, static_cast<size_t>(align));
}

void operator delete(void *p, std::align_val_t align) {
    AMS_UNUSED(align);
    return ams::ubt::Deallocate(p, 0);
}
