#include "ubt_client_memory.hpp"
#include "ubt_log.hpp"

namespace ams::ubt {

    namespace {

        constexpr size_t PageSize = 0x1000;
        constexpr u64 MaxWholeRegionMapSize = 16_MB;

        constinit util::TypedStorage<ClientMemory> g_client_memory_storage = {};
        constinit bool g_client_memory_constructed = false;

        u64 GenerateRandom(u64 max) {
            u64 value = 0;
            os::GenerateRandomBytes(std::addressof(value), sizeof(value));
            if (max == std::numeric_limits<u64>::max()) {
                return value;
            }
            return value % (max + 1);
        }

        /* Thread that keeps a debugged client running by acknowledging its debug events. */
        constexpr size_t DebugThreadStackSize = 0x2000;
        alignas(os::ThreadStackAlignment) constinit u8 g_debug_thread_stack[DebugThreadStackSize];
        constinit os::ThreadType g_debug_thread;
        constinit bool g_debug_thread_started = false;

        void DebugThreadFunction(void *) {
            while (true) {
                GetClientMemory().Service();
                os::SleepThread(TimeSpan::FromMilliSeconds(20));
            }
        }

        void EnsureDebugThread() {
            if (g_debug_thread_started) {
                return;
            }
            if (R_SUCCEEDED(os::CreateThread(std::addressof(g_debug_thread), DebugThreadFunction, nullptr, g_debug_thread_stack, sizeof(g_debug_thread_stack), 20))) {
                os::SetThreadNamePointer(std::addressof(g_debug_thread), "ubt::DebugThread");
                os::StartThread(std::addressof(g_debug_thread));
                g_debug_thread_started = true;
            }
        }

    }

    ClientMemory &GetClientMemory() {
        if (AMS_UNLIKELY(!g_client_memory_constructed)) {
            util::ConstructAt(g_client_memory_storage);
            g_client_memory_constructed = true;
        }
        return util::GetReference(g_client_memory_storage);
    }

    Result ClientMemory::Attach(os::ProcessId process_id) {
        std::scoped_lock lk(m_lock);

        if (m_process_handle != os::InvalidNativeHandle && m_process_id == process_id) {
            R_SUCCEED();
        }

        /* A different client: drop everything tied to the old one. */
        this->UnmapAllLocked();
        if (m_debug_handle != os::InvalidNativeHandle) {
            R_DISCARD(svc::CloseHandle(m_debug_handle));
            m_debug_handle = os::InvalidNativeHandle;
        }
        if (m_process_handle != os::InvalidNativeHandle) {
            R_DISCARD(svc::CloseHandle(m_process_handle));
            m_process_handle = os::InvalidNativeHandle;
        }
        m_mode = MemoryAccessMode_Unknown;
        m_logged_regions = 0;
        m_logged_failures = 0;

        os::NativeHandle handle = os::InvalidNativeHandle;
        ncm::ProgramLocation loc;
        cfg::OverrideStatus status;
        const Result rc = pm::dmnt::AtmosphereGetProcessInfo(std::addressof(handle), std::addressof(loc), std::addressof(status), process_id);
        if (R_FAILED(rc)) {
            UBT_LOG_E("mem: could not get a process handle for pid %llu: 0x%x", static_cast<unsigned long long>(process_id.value), rc.GetValue());
            R_RETURN(rc);
        }

        m_process_id = process_id;
        m_process_handle = handle;
        UBT_LOG_I("mem: attached to pid %llu (program %016llx)", static_cast<unsigned long long>(process_id.value), static_cast<unsigned long long>(loc.program_id.value));
        R_SUCCEED();
    }

    void ClientMemory::Detach() {
        std::scoped_lock lk(m_lock);
        this->UnmapAllLocked();
        if (m_debug_handle != os::InvalidNativeHandle) {
            R_DISCARD(svc::CloseHandle(m_debug_handle));
            m_debug_handle = os::InvalidNativeHandle;
        }
        if (m_process_handle != os::InvalidNativeHandle) {
            R_DISCARD(svc::CloseHandle(m_process_handle));
            m_process_handle = os::InvalidNativeHandle;
        }
        m_process_id = os::InvalidProcessId;
        m_mode = MemoryAccessMode_Unknown;
    }

    void ClientMemory::UnmapAllLocked() {
        for (auto &mapping : m_mappings) {
            if (mapping.local != nullptr) {
                os::UnmapProcessMemory(mapping.local, m_process_handle, mapping.client_address, mapping.size);
                mapping = {};
            }
        }
        m_next_mapping = 0;
    }

    Result ClientMemory::Read(void *dst, u64 client_address, size_t size) {
        std::scoped_lock lk(m_lock);
        R_RETURN(this->AccessLocked(client_address, dst, size, false));
    }

    Result ClientMemory::Write(u64 client_address, const void *src, size_t size) {
        std::scoped_lock lk(m_lock);
        R_RETURN(this->AccessLocked(client_address, const_cast<void *>(src), size, true));
    }

    void ClientMemory::LogRegionLocked(u64 client_address, size_t size) {
        /* Describe the first few distinct buffers the client hands us; this tells us which */
        /* access method the client's buffers allow. */
        if (m_logged_regions >= 8) {
            return;
        }
        ++m_logged_regions;

        svc::MemoryInfo info = {};
        svc::PageInfo page_info = {};
        const Result rc = svc::QueryProcessMemory(std::addressof(info), std::addressof(page_info), m_process_handle, client_address);
        if (R_FAILED(rc)) {
            UBT_LOG_E("mem: QueryProcessMemory(0x%llx) failed: 0x%x", static_cast<unsigned long long>(client_address), rc.GetValue());
            return;
        }
        UBT_LOG_I("mem: buffer 0x%llx+0x%zx lies in region base=0x%llx size=0x%llx state=0x%x perm=0x%x attr=0x%x",
                  static_cast<unsigned long long>(client_address), size,
                  static_cast<unsigned long long>(info.base_address), static_cast<unsigned long long>(info.size),
                  static_cast<unsigned>(info.state), static_cast<unsigned>(info.permission), static_cast<unsigned>(info.attribute));
    }

    Result ClientMemory::AccessLocked(u64 client_address, void *buffer, size_t size, bool write) {
        R_UNLESS(m_process_handle != os::InvalidNativeHandle, svc::ResultInvalidHandle());
        R_SUCCEED_IF(size == 0);
        R_UNLESS(client_address != 0, svc::ResultInvalidAddress());

        /* Preferred: map the client's pages into our address space (no side effects). */
        if (m_mode != MemoryAccessMode_Debug) {
            const Result map_rc = this->AccessMappedLocked(client_address, buffer, size, write);
            if (R_SUCCEEDED(map_rc)) {
                if (m_mode != MemoryAccessMode_Map) {
                    m_mode = MemoryAccessMode_Map;
                    UBT_LOG_I("mem: using mapped access");
                }
                R_SUCCEED();
            }
            if (m_logged_regions < 8) {
                this->LogRegionLocked(client_address, size);
                UBT_LOG_I("mem: mapping 0x%llx+0x%zx failed (0x%x); falling back to debug access", static_cast<unsigned long long>(client_address), size, map_rc.GetValue());
            }
        }

        /* Fallback: debug read/write. */
        const Result dbg_rc = this->AccessDebugLocked(client_address, buffer, size, write);
        if (R_SUCCEEDED(dbg_rc)) {
            if (m_mode != MemoryAccessMode_Debug) {
                m_mode = MemoryAccessMode_Debug;
                UBT_LOG_I("mem: using debug access");
            }
            R_SUCCEED();
        }

        if (m_logged_failures < 8) {
            ++m_logged_failures;
            UBT_LOG_E("mem: %s 0x%llx+0x%zx failed: 0x%x", write ? "write" : "read", static_cast<unsigned long long>(client_address), size, dbg_rc.GetValue());
        }
        m_mode = MemoryAccessMode_Failed;
        R_RETURN(dbg_rc);
    }

    Result ClientMemory::AccessMappedLocked(u64 client_address, void *buffer, size_t size, bool write) {
        /* Look for an existing mapping that covers the range. */
        for (auto &mapping : m_mappings) {
            if (mapping.local != nullptr && client_address >= mapping.client_address && client_address + size <= mapping.client_address + mapping.size) {
                u8 *p = static_cast<u8 *>(mapping.local) + (client_address - mapping.client_address);
                if (write) {
                    std::memcpy(p, buffer, size);
                } else {
                    std::memcpy(buffer, p, size);
                }
                R_SUCCEED();
            }
        }

        /* The kernel only lets another process map code/data segment memory (not heap). Check */
        /* first: asking for something it refuses costs dozens of failed attempts, and the     */
        /* library helper treats unexpected kernel errors as fatal.                            */
        u64 map_base = util::AlignDown(client_address, PageSize);
        u64 map_size = util::AlignUp(client_address + size, PageSize) - map_base;
        {
            svc::MemoryInfo info = {};
            svc::PageInfo page_info = {};
            R_TRY(svc::QueryProcessMemory(std::addressof(info), std::addressof(page_info), m_process_handle, client_address));

            const bool mappable_state = info.state == svc::MemoryState_Code || info.state == svc::MemoryState_CodeData ||
                                        info.state == svc::MemoryState_AliasCode || info.state == svc::MemoryState_AliasCodeData;
            const bool read_write = (static_cast<u32>(info.permission) & 3) == 3;
            const bool covers = info.base_address <= map_base && map_base + map_size <= info.base_address + info.size;
            R_UNLESS(mappable_state && read_write && covers && info.attribute == static_cast<svc::MemoryAttribute>(0), os::ResultInvalidCurrentMemoryState());

            /* Map the whole region when it is reasonably small: the client's transfer buffers */
            /* usually live together, so one mapping then serves every transfer.               */
            if (info.size <= MaxWholeRegionMapSize) {
                map_base = info.base_address;
                map_size = info.size;
            }
        }

        void *local = nullptr;
        R_TRY(os::MapProcessMemory(std::addressof(local), m_process_handle, map_base, map_size, GenerateRandom));

        /* Replace the oldest cache slot. */
        Mapping &slot = m_mappings[m_next_mapping];
        m_next_mapping = (m_next_mapping + 1) % MaxMappings;
        if (slot.local != nullptr) {
            os::UnmapProcessMemory(slot.local, m_process_handle, slot.client_address, slot.size);
        }
        slot = { .client_address = map_base, .size = static_cast<size_t>(map_size), .local = local };

        u8 *p = static_cast<u8 *>(local) + (client_address - map_base);
        if (write) {
            std::memcpy(p, buffer, size);
        } else {
            std::memcpy(buffer, p, size);
        }
        R_SUCCEED();
    }

    Result ClientMemory::EnsureDebugLocked() {
        R_SUCCEED_IF(m_debug_handle != os::InvalidNativeHandle);

        svc::Handle handle = svc::InvalidHandle;
        const Result rc = svc::DebugActiveProcess(std::addressof(handle), m_process_id.value);
        if (R_FAILED(rc)) {
            if (m_logged_failures < 8) {
                UBT_LOG_E("mem: DebugActiveProcess failed: 0x%x", rc.GetValue());
            }
            R_RETURN(rc);
        }
        m_debug_handle = handle;
        UBT_LOG_I("mem: attached as debugger");

        /* The client is stopped until its pending debug events are acknowledged. */
        this->ContinueDebugLocked();
        EnsureDebugThread();
        R_SUCCEED();
    }

    void ClientMemory::ContinueDebugLocked() {
        if (m_debug_handle == os::InvalidNativeHandle) {
            return;
        }

        bool detach = false;
        svc::DebugEventInfo event = {};
        while (R_SUCCEEDED(svc::GetDebugEvent(std::addressof(event), m_debug_handle))) {
            if (event.type == svc::DebugEvent_ExitProcess) {
                UBT_LOG_I("mem: client process exited");
                detach = true;
            } else if (event.type == svc::DebugEvent_Exception) {
                const auto type = event.info.exception.type;
                if (type != svc::DebugException_DebuggerAttached && type != svc::DebugException_DebuggerBreak) {
                    /* The client crashed for real. Step aside so that the system's crash handling runs. */
                    UBT_LOG_E("mem: client raised exception %u at 0x%llx; detaching debugger", static_cast<unsigned>(type), static_cast<unsigned long long>(event.info.exception.address));
                    detach = true;
                }
            }
        }

        if (detach) {
            R_DISCARD(svc::CloseHandle(m_debug_handle));
            m_debug_handle = os::InvalidNativeHandle;
            return;
        }

        R_DISCARD(svc::ContinueDebugEvent(m_debug_handle, svc::ContinueFlag_ExceptionHandled | svc::ContinueFlag_ContinueAll, nullptr, 0));
    }

    void ClientMemory::Service() {
        std::scoped_lock lk(m_lock);
        if (m_debug_handle == os::InvalidNativeHandle) {
            return;
        }

        /* The debug handle is signaled while events are pending. */
        s32 index = -1;
        const svc::Handle handle = m_debug_handle;
        if (R_SUCCEEDED(svc::WaitSynchronization(std::addressof(index), std::addressof(handle), 1, 0))) {
            this->ContinueDebugLocked();
        }
    }

    Result ClientMemory::AccessDebugLocked(u64 client_address, void *buffer, size_t size, bool write) {
        R_TRY(this->EnsureDebugLocked());

        if (write) {
            R_RETURN(svc::WriteDebugProcessMemory(m_debug_handle, reinterpret_cast<uintptr_t>(buffer), client_address, size));
        } else {
            R_RETURN(svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(buffer), m_debug_handle, client_address, size));
        }
    }

}
