#pragma once
#include "ubt_common.hpp"

namespace ams::ubt {

    /* Read/write access to the address space of the process we are intercepting (eth). */
    /* usb:hs passes transfer buffers as raw client addresses, so a virtual device has to */
    /* touch the client's memory directly, the same way the real usb sysmodule does.      */
    class ClientMemory {
        private:
            static constexpr size_t MaxMappings = 6;

            struct Mapping {
                u64 client_address;
                size_t size;
                void *local;
            };
        private:
            os::SdkMutex m_lock{};
            os::ProcessId m_process_id = os::InvalidProcessId;
            os::NativeHandle m_process_handle = os::InvalidNativeHandle;
            os::NativeHandle m_debug_handle = os::InvalidNativeHandle;
            MemoryAccessMode m_mode = MemoryAccessMode_Unknown;
            Mapping m_mappings[MaxMappings] = {};
            size_t m_next_mapping = 0;
            u32 m_logged_regions = 0;
            u32 m_logged_failures = 0;
        public:
            Result Attach(os::ProcessId process_id);
            void Detach();

            Result Read(void *dst, u64 client_address, size_t size);
            Result Write(u64 client_address, const void *src, size_t size);

            MemoryAccessMode GetMode() const { return m_mode; }

            /* Called periodically; keeps a debugged client running. */
            void Service();
        private:
            Result AccessLocked(u64 client_address, void *buffer, size_t size, bool write);
            Result AccessMappedLocked(u64 client_address, void *buffer, size_t size, bool write);
            Result AccessDebugLocked(u64 client_address, void *buffer, size_t size, bool write);
            Result EnsureDebugLocked();
            void ContinueDebugLocked();
            void LogRegionLocked(u64 client_address, size_t size);
            void UnmapAllLocked();
    };

    ClientMemory &GetClientMemory();

}
