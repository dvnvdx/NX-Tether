#include "ubt_usbhs_mitm.hpp"
#include "ubt_usbhs_interfaces.hpp"
#include "ubt_virtual_adapter.hpp"
#include "ubt_client_memory.hpp"
#include "ubt_config.hpp"
#include "ubt_log.hpp"
#include "ubt_phone_link.hpp"
#include "ubt_guard.hpp"

/* Status / control service used by the overlay. */
#define AMS_UBT_STATUS_SERVICE_INTERFACE_INFO(C, H)                                                              \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetStatus,        (const sf::OutBuffer &out_status), (out_status))       \
    AMS_SF_METHOD_INFO(C, H, 1, Result, SetBridgeEnabled, (u8 enabled),                      (enabled))          \
    AMS_SF_METHOD_INFO(C, H, 2, Result, SetBootMode,      (u8 mode),                         (mode))             \
    AMS_SF_METHOD_INFO(C, H, 3, Result, Relink,           (),                                ())

AMS_SF_DEFINE_INTERFACE(ams::ubt::impl, IStatusService, AMS_UBT_STATUS_SERVICE_INTERFACE_INFO, 0x7E7B0E04)

namespace ams::ubt::mitm {

    namespace {

        constexpr size_t AvailableEventCount = 3;
        constexpr size_t MaxTrackedSessions  = 4;

        struct AvailableEvent {
            bool active;
            DeviceFilter filter;
            os::SystemEventType ours;
            os::NativeHandle real;
        };

        /* State of one intercepted usb:hs root session. eth normally opens a single one, but */
        /* nothing stops it from using one per driver, so a few are tracked independently.    */
        struct SessionState {
            bool in_use;
            AvailableEvent available[AvailableEventCount];
            bool state_active;
            os::SystemEventType state_ours;
            os::NativeHandle state_real;
        };

        constinit os::SdkMutex g_session_lock;
        constinit SessionState g_sessions[MaxTrackedSessions] = {};
        constinit u32 g_session_count = 0;

        /* Filters are remembered even after a session's events are torn down. */
        constinit DeviceFilter g_seen_filters[8] = {};
        constinit size_t g_seen_filter_count = 0;

        constinit bool g_wake_created = false;
        constinit os::SystemEventType g_wake = {};

        void RememberFilterLocked(const DeviceFilter &filter) {
            for (size_t i = 0; i < g_seen_filter_count; ++i) {
                if (std::memcmp(std::addressof(g_seen_filters[i]), std::addressof(filter), sizeof(filter)) == 0) {
                    return;
                }
            }
            if (g_seen_filter_count < util::size(g_seen_filters)) {
                g_seen_filters[g_seen_filter_count++] = filter;
            }
        }

        void WakeRelay() {
            if (g_wake_created) {
                os::SignalSystemEvent(std::addressof(g_wake));
            }
        }

        void LogFilter(const char *what, const DeviceFilter &f) {
            UBT_LOG_I("mitm: %s filter flags=%04x vid=%04x pid=%04x bcd=%04x..%04x dev=%02x/%02x/%02x if=%02x/%02x/%02x",
                      what, f.flags, f.id_vendor, f.id_product, f.bcd_device_min, f.bcd_device_max,
                      f.device_class, f.device_subclass, f.device_protocol, f.interface_class, f.interface_subclass, f.interface_protocol);
        }

        void CloseAvailableEventLocked(AvailableEvent &event) {
            if (event.active) {
                os::DestroySystemEvent(std::addressof(event.ours));
                R_DISCARD(svc::CloseHandle(event.real));
                event.active = false;
                event.real = os::InvalidNativeHandle;
            }
        }

        void CloseStateEventLocked(SessionState &session) {
            if (session.state_active) {
                os::DestroySystemEvent(std::addressof(session.state_ours));
                R_DISCARD(svc::CloseHandle(session.state_real));
                session.state_active = false;
                session.state_real = os::InvalidNativeHandle;
            }
        }

        SessionState *AllocateSession() {
            std::scoped_lock lk(g_session_lock);
            for (auto &session : g_sessions) {
                if (!session.in_use) {
                    session.in_use = true;
                    ++g_session_count;
                    return std::addressof(session);
                }
            }
            return nullptr;
        }

        void FreeSession(SessionState *session) {
            if (session == nullptr) {
                return;
            }
            std::scoped_lock lk(g_session_lock);
            for (auto &event : session->available) {
                CloseAvailableEventLocked(event);
            }
            CloseStateEventLocked(*session);
            session->in_use = false;
        }

        /* Events handed out by the real usb:hs cannot be signaled by us, so the client gets  */
        /* our own events and this thread forwards the real signals onto them.                */
        constexpr size_t RelayThreadStackSize = 0x2000;
        alignas(os::ThreadStackAlignment) constinit u8 g_relay_thread_stack[RelayThreadStackSize];
        constinit os::ThreadType g_relay_thread;

        void RelayThreadFunction(void *) {
            constexpr size_t MaxHandles = 1 + MaxTrackedSessions * (AvailableEventCount + 1);
            constexpr int KindWake  = -1;
            constexpr int KindState = static_cast<int>(AvailableEventCount);

            while (true) {
                svc::Handle handles[MaxHandles];
                u8 owner[MaxHandles];
                int kinds[MaxHandles];
                s32 count = 0;

                {
                    std::scoped_lock lk(g_session_lock);
                    handles[count] = os::GetReadableHandleOfSystemEvent(std::addressof(g_wake));
                    owner[count] = 0;
                    kinds[count++] = KindWake;
                    for (size_t si = 0; si < MaxTrackedSessions; ++si) {
                        const SessionState &session = g_sessions[si];
                        if (!session.in_use) {
                            continue;
                        }
                        for (size_t i = 0; i < AvailableEventCount; ++i) {
                            if (session.available[i].active) {
                                handles[count] = session.available[i].real;
                                owner[count] = static_cast<u8>(si);
                                kinds[count++] = static_cast<int>(i);
                            }
                        }
                        if (session.state_active) {
                            handles[count] = session.state_real;
                            owner[count] = static_cast<u8>(si);
                            kinds[count++] = KindState;
                        }
                    }
                }

                s32 index = -1;
                const Result rc = svc::WaitSynchronization(std::addressof(index), handles, count, TimeSpan::FromSeconds(1).GetNanoSeconds());
                if (R_FAILED(rc)) {
                    if (!svc::ResultTimedOut::Includes(rc)) {
                        /* A handle was closed under us; rebuild the list after a short pause. */
                        os::SleepThread(TimeSpan::FromMilliSeconds(10));
                    }
                    continue;
                }
                if (index < 0 || index >= count) {
                    continue;
                }

                std::scoped_lock lk(g_session_lock);
                const int kind = kinds[index];
                if (kind == KindWake) {
                    os::ClearSystemEvent(std::addressof(g_wake));
                    continue;
                }

                SessionState &session = g_sessions[owner[index]];
                if (!session.in_use) {
                    continue;
                }
                if (kind == KindState) {
                    if (session.state_active && session.state_real == handles[index]) {
                        R_DISCARD(svc::ResetSignal(session.state_real));
                        os::SignalSystemEvent(std::addressof(session.state_ours));
                        UBT_LOG_D("mitm: real interface-state-change event fired");
                    }
                } else {
                    AvailableEvent &event = session.available[kind];
                    if (event.active && event.real == handles[index]) {
                        R_DISCARD(svc::ResetSignal(event.real));
                        os::SignalSystemEvent(std::addressof(event.ours));
                        UBT_LOG_D("mitm: real interface-available event %d fired", kind);
                    }
                }
            }
        }

        /* ------------------------------------------------------------------------------- */

        class UsbHsMitmService : public sf::MitmServiceImplBase {
            private:
                static constexpr u32 MaxLoggedQueries = 60;
            private:
                SessionState *m_state;
                u32 m_logged_queries = 0;
            public:
                UsbHsMitmService(std::shared_ptr<::Service> &&s, const sm::MitmProcessInfo &c) : sf::MitmServiceImplBase(std::forward<std::shared_ptr<::Service>>(s), c), m_state(AllocateSession()) {
                    UBT_LOG_I("mitm: program %016llx (pid %llu) opened usb:hs through us", static_cast<unsigned long long>(c.program_id.value), static_cast<unsigned long long>(c.process_id.value));
                    if (m_state == nullptr) {
                        UBT_LOG_E("mitm: too many intercepted sessions; this one is passed through untouched");
                    }
                    WakeRelay();
                    R_DISCARD(GetClientMemory().Attach(c.process_id));
                }

                ~UsbHsMitmService() {
                    UBT_LOG_I("mitm: intercepted usb:hs session closed");
                    FreeSession(m_state);
                    WakeRelay();
                }
            public:
                static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                    /* Only the ethernet driver, and only when the bridge is enabled. Every other */
                    /* usb:hs client (hid, audio, ...) talks to the real service untouched.       */
                    const bool intercept = config::Get().mode == Mode_Bridge && client_info.program_id == ncm::SystemProgramId::Eth;
                    UBT_LOG_I("mitm: usb:hs requested by program %016llx: %s", static_cast<unsigned long long>(client_info.program_id.value), intercept ? "intercepting" : "not intercepted");
                    return intercept;
                }
            private:
                Result QueryImpl(u32 cmd_id, bool available_only, sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces, const DeviceFilter &filter) {
                    R_UNLESS(m_state != nullptr, sm::mitm::ResultShouldForwardToSession());

                    s32 total = 0;
                    R_TRY(serviceDispatchInOut(m_forward_service.get(), cmd_id, filter, total,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                        .buffers = { { out_interfaces.GetPointer(), out_interfaces.GetSize() } },
                    ));

                    auto &adapter = GetVirtualAdapter();
                    bool added = false;
                    if (adapter.MatchesFilter(filter) && !(available_only && adapter.IsAcquired())) {
                        const size_t capacity = out_interfaces.GetSize() / sizeof(::UsbHsInterface);
                        if (total >= 0 && static_cast<size_t>(total) < capacity) {
                            ::UsbHsInterface entry;
                            if (adapter.GetQueryOutput(std::addressof(entry))) {
                                std::memcpy(out_interfaces.GetPointer() + static_cast<size_t>(total) * sizeof(::UsbHsInterface), std::addressof(entry), sizeof(entry));
                                ++total;
                                added = true;
                            }
                        }
                    }

                    /* Clients may poll; log the first calls and every call that returns our adapter early on. */
                    if (m_logged_queries < MaxLoggedQueries || log::GetLevel() >= log::Level_Trace) {
                        ++m_logged_queries;
                        UBT_LOG_D("mitm: Query%sInterfaces(flags=%04x vid=%04x pid=%04x) -> %d%s", available_only ? "Available" : "All", filter.flags, filter.id_vendor, filter.id_product, total, added ? " (including the virtual adapter)" : "");
                    }
                    out_total.SetValue(total);
                    R_SUCCEED();
                }
            public:
                Result QueryAllInterfaces(sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces, DeviceFilter filter) {
                    R_RETURN(this->QueryImpl(1, false, out_total, out_interfaces, filter));
                }

                Result QueryAvailableInterfaces(sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces, DeviceFilter filter) {
                    R_RETURN(this->QueryImpl(2, true, out_total, out_interfaces, filter));
                }

                Result QueryAcquiredInterfaces(sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces) {
                    R_UNLESS(m_state != nullptr, sm::mitm::ResultShouldForwardToSession());

                    s32 total = 0;
                    R_TRY(serviceDispatchOut(m_forward_service.get(), 3, total,
                        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                        .buffers = { { out_interfaces.GetPointer(), out_interfaces.GetSize() } },
                    ));

                    auto &adapter = GetVirtualAdapter();
                    if (adapter.IsAcquired()) {
                        const size_t capacity = out_interfaces.GetSize() / sizeof(::UsbHsInterface);
                        if (total >= 0 && static_cast<size_t>(total) < capacity) {
                            ::UsbHsInterface entry;
                            if (adapter.GetQueryOutput(std::addressof(entry))) {
                                std::memcpy(out_interfaces.GetPointer() + static_cast<size_t>(total) * sizeof(::UsbHsInterface), std::addressof(entry), sizeof(entry));
                                ++total;
                            }
                        }
                    }

                    if (m_logged_queries < MaxLoggedQueries || log::GetLevel() >= log::Level_Trace) {
                        ++m_logged_queries;
                        UBT_LOG_D("mitm: QueryAcquiredInterfaces -> %d", total);
                    }
                    out_total.SetValue(total);
                    R_SUCCEED();
                }

                Result CreateInterfaceAvailableEvent(sf::OutCopyHandle out_event, CreateInterfaceAvailableEventIn in) {
                    LogFilter("CreateInterfaceAvailableEvent", in.filter);
                    {
                        std::scoped_lock lk(g_session_lock);
                        RememberFilterLocked(in.filter);
                    }
                    R_UNLESS(m_state != nullptr && in.index < AvailableEventCount, sm::mitm::ResultShouldForwardToSession());

                    ::Handle real = INVALID_HANDLE;
                    const Result rc = serviceDispatchIn(m_forward_service.get(), 4, in,
                        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                        .out_handles = std::addressof(real),
                    );
                    if (R_FAILED(rc)) {
                        UBT_LOG_E("mitm: real CreateInterfaceAvailableEvent(%u) failed: 0x%x", in.index, rc.GetValue());
                        R_RETURN(rc);
                    }

                    os::NativeHandle client_handle = os::InvalidNativeHandle;
                    {
                        std::scoped_lock lk(g_session_lock);
                        AvailableEvent &event = m_state->available[in.index];
                        CloseAvailableEventLocked(event);
                        if (const Result create_rc = os::CreateSystemEvent(std::addressof(event.ours), os::EventClearMode_ManualClear, true); R_FAILED(create_rc)) {
                            R_DISCARD(svc::CloseHandle(real));
                            R_RETURN(create_rc);
                        }
                        event.filter = in.filter;
                        event.real = real;
                        event.active = true;
                        client_handle = os::GetReadableHandleOfSystemEvent(std::addressof(event.ours));
                    }
                    WakeRelay();

                    /* If the virtual adapter is already present, the client must hear about it. */
                    if (GetVirtualAdapter().MatchesFilter(in.filter)) {
                        std::scoped_lock lk(g_session_lock);
                        if (m_state->available[in.index].active) {
                            os::SignalSystemEvent(std::addressof(m_state->available[in.index].ours));
                        }
                    }

                    out_event.SetValue(client_handle, false);
                    R_SUCCEED();
                }

                Result DestroyInterfaceAvailableEvent(u8 index) {
                    UBT_LOG_I("mitm: DestroyInterfaceAvailableEvent(%u)", index);
                    R_UNLESS(m_state != nullptr && index < AvailableEventCount, sm::mitm::ResultShouldForwardToSession());

                    const Result rc = serviceDispatchIn(m_forward_service.get(), 5, index);
                    {
                        std::scoped_lock lk(g_session_lock);
                        CloseAvailableEventLocked(m_state->available[index]);
                    }
                    WakeRelay();
                    R_RETURN(rc);
                }

                Result GetInterfaceStateChangeEvent(sf::OutCopyHandle out_event) {
                    R_UNLESS(m_state != nullptr, sm::mitm::ResultShouldForwardToSession());

                    ::Handle real = INVALID_HANDLE;
                    const Result rc = serviceDispatch(m_forward_service.get(), 6,
                        .out_handle_attrs = { SfOutHandleAttr_HipcCopy },
                        .out_handles = std::addressof(real),
                    );
                    if (R_FAILED(rc)) {
                        UBT_LOG_E("mitm: real GetInterfaceStateChangeEvent failed: 0x%x", rc.GetValue());
                        R_RETURN(rc);
                    }

                    os::NativeHandle client_handle = os::InvalidNativeHandle;
                    {
                        std::scoped_lock lk(g_session_lock);
                        CloseStateEventLocked(*m_state);
                        if (const Result create_rc = os::CreateSystemEvent(std::addressof(m_state->state_ours), os::EventClearMode_ManualClear, true); R_FAILED(create_rc)) {
                            R_DISCARD(svc::CloseHandle(real));
                            R_RETURN(create_rc);
                        }
                        m_state->state_real = real;
                        m_state->state_active = true;
                        client_handle = os::GetReadableHandleOfSystemEvent(std::addressof(m_state->state_ours));
                    }
                    WakeRelay();

                    UBT_LOG_I("mitm: GetInterfaceStateChangeEvent");
                    out_event.SetValue(client_handle, false);
                    R_SUCCEED();
                }

                Result AcquireUsbIf(sf::Out<sf::SharedPointer<impl::IClientIfSession>> out, const sf::OutBuffer &out_buffer0, const sf::OutBuffer &out_buffer1, s32 id) {
                    auto &adapter = GetVirtualAdapter();
                    if (m_state == nullptr || id != adapter.GetInterfaceId()) {
                        /* A real device: let the real service handle it. The client then talks to */
                        /* that interface directly; we are not involved any further.                */
                        UBT_LOG_I("mitm: AcquireUsbIf(0x%x): real interface, passing through", static_cast<unsigned>(id));
                        R_THROW(sm::mitm::ResultShouldForwardToSession());
                    }

                    UBT_LOG_I("mitm: AcquireUsbIf(0x%x): virtual adapter", static_cast<unsigned>(id));
                    u32 generation = 0;
                    R_TRY(adapter.Acquire(std::addressof(generation), out_buffer0.GetPointer(), out_buffer0.GetSize(), out_buffer1.GetPointer(), out_buffer1.GetSize()));

                    const auto object_id = AllocateVirtualObjectId();
                    out.SetValue(sf::CreateSharedObjectEmplaced<impl::IClientIfSession, ClientIfSessionImpl>(generation, object_id), object_id);
                    R_SUCCEED();
                }
        };
        static_assert(impl::IsIClientRootSessionMitm<UsbHsMitmService>);

        /* ------------------------------------------------------------------------------- */

        class StatusService {
            public:
                Result GetStatus(const sf::OutBuffer &out_status) {
                    Status status = {};
                    status.struct_version = 1;
                    status.module_version = Version;
                    status.mode = config::Get().mode;

                    link::FillStatus(std::addressof(status));
                    GetVirtualAdapter().FillStatus(std::addressof(status));
                    mitm::FillStatus(std::addressof(status));
                    if (guard::WasTripped()) {
                        util::SNPrintf(status.message, sizeof(status.message), "bridge off: last boot failed (see log)");
                    }

                    std::memcpy(out_status.GetPointer(), std::addressof(status), std::min(out_status.GetSize(), sizeof(status)));
                    R_SUCCEED();
                }

                Result SetBridgeEnabled(u8 enabled) {
                    UBT_LOG_I("ctl: bridge %s by overlay", enabled != 0 ? "enabled" : "disabled");
                    link::SetBridgeEnabled(enabled != 0);
                    R_SUCCEED();
                }

                Result SetBootMode(u8 mode) {
                    UBT_LOG_I("ctl: boot mode set to %u by overlay", mode);
                    R_RETURN(config::SaveMode(mode));
                }

                Result Relink() {
                    UBT_LOG_I("ctl: relink requested by overlay");
                    link::RequestRelink();
                    R_SUCCEED();
                }
        };
        static_assert(impl::IsIStatusService<StatusService>);

        /* ------------------------------------------------------------------------------- */

        enum PortIndex {
            PortIndex_UsbHsMitm,
            PortIndex_Status,
            PortIndex_Count,
        };

        constexpr sm::ServiceName UsbHsServiceName  = sm::ServiceName::Encode("usb:hs");
        constexpr sm::ServiceName StatusServiceName = sm::ServiceName::Encode("usbtet");
        constexpr size_t StatusMaxSessions = 4;

        struct ServerOptions {
            static constexpr size_t PointerBufferSize   = 0x1000;
            static constexpr size_t MaxDomains          = 8;
            static constexpr size_t MaxDomainObjects    = 0x200;
            static constexpr bool CanDeferInvokeRequest = false;
            static constexpr bool CanManageMitmServers  = true;
        };

        /* The intercepted root session, its interface and endpoint sessions (when the client */
        /* does not use a domain), plus the overlay.                                           */
        constexpr size_t MaxSessions = 16 + StatusMaxSessions;

        class ServerManager final : public sf::hipc::ServerManager<PortIndex_Count, ServerOptions, MaxSessions> {
            private:
                virtual Result OnNeedsToAccept(int port_index, Server *server) override;
        };

        constinit util::TypedStorage<ServerManager> g_server_manager_storage = {};
        sf::UnmanagedServiceObject<impl::IStatusService, StatusService> g_status_service;

        Result ServerManager::OnNeedsToAccept(int port_index, Server *server) {
            switch (port_index) {
                case PortIndex_UsbHsMitm:
                {
                    std::shared_ptr<::Service> fsrv;
                    sm::MitmProcessInfo client_info;
                    server->AcknowledgeMitmSession(std::addressof(fsrv), std::addressof(client_info));
                    R_RETURN(this->AcceptMitmImpl(server, sf::CreateSharedObjectEmplaced<impl::IClientRootSessionMitm, UsbHsMitmService>(decltype(fsrv)(fsrv), client_info), fsrv));
                }
                case PortIndex_Status:
                    R_RETURN(this->AcceptImpl(server, g_status_service.GetShared()));
                AMS_UNREACHABLE_DEFAULT_CASE();
            }
        }

        constexpr size_t ServerThreadStackSize = 0x8000;
        alignas(os::ThreadStackAlignment) constinit u8 g_server_thread_stack[ServerThreadStackSize];
        constinit os::ThreadType g_server_thread;

        void ServerThreadFunction(void *) {
            auto &manager = util::GetReference(g_server_manager_storage);

            /* The interception is installed in every mode: Atmosphere holds back all usb:hs    */
            /* clients at boot (because of our mitm.lst) until it is. In the non-bridge modes   */
            /* ShouldMitm() simply declines every client.                                        */
            R_ABORT_UNLESS(manager.RegisterMitmServer<UsbHsMitmService>(PortIndex_UsbHsMitm, UsbHsServiceName));
            UBT_LOG_I("mitm: usb:hs interception installed");

            if (const Result rc = manager.RegisterServer(PortIndex_Status, StatusServiceName, StatusMaxSessions); R_FAILED(rc)) {
                UBT_LOG_E("ctl: could not register the status service: 0x%x", rc.GetValue());
            }

            /* The phone link opens its own usb:hs session; that can only happen once the      */
            /* interception is in place.                                                        */
            link::Launch();

            manager.LoopProcess();
        }

    }

    void Launch() {
        /* Construct the shared objects before any thread can race to do it. */
        GetVirtualAdapter();
        GetClientMemory();
        util::ConstructAt(g_server_manager_storage);

        R_ABORT_UNLESS(os::CreateSystemEvent(std::addressof(g_wake), os::EventClearMode_ManualClear, true));
        g_wake_created = true;

        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_relay_thread), RelayThreadFunction, nullptr, g_relay_thread_stack, sizeof(g_relay_thread_stack), 19));
        os::SetThreadNamePointer(std::addressof(g_relay_thread), "ubt::RelayThread");
        os::StartThread(std::addressof(g_relay_thread));

        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_server_thread), ServerThreadFunction, nullptr, g_server_thread_stack, sizeof(g_server_thread_stack), 18));
        os::SetThreadNamePointer(std::addressof(g_server_thread), "ubt::IpcServer");
        os::StartThread(std::addressof(g_server_thread));
    }

    size_t GetClientFilters(DeviceFilter *out, size_t max_count) {
        std::scoped_lock lk(g_session_lock);
        const size_t count = std::min(max_count, g_seen_filter_count);
        for (size_t i = 0; i < count; ++i) {
            out[i] = g_seen_filters[i];
        }
        return count;
    }

    void NotifyVirtualAdapterChanged(bool attached) {
        constexpr size_t Total = MaxTrackedSessions * AvailableEventCount;

        /* Work out which of the clients' filters the adapter satisfies without holding our  */
        /* lock (the adapter has its own).                                                    */
        DeviceFilter filters[Total];
        bool active[Total] = {};
        {
            std::scoped_lock lk(g_session_lock);
            for (size_t si = 0; si < MaxTrackedSessions; ++si) {
                for (size_t i = 0; i < AvailableEventCount; ++i) {
                    const AvailableEvent &event = g_sessions[si].available[i];
                    active[si * AvailableEventCount + i] = g_sessions[si].in_use && event.active;
                    filters[si * AvailableEventCount + i] = event.filter;
                }
            }
        }

        bool matches[Total] = {};
        if (attached) {
            for (size_t i = 0; i < Total; ++i) {
                matches[i] = active[i] && GetVirtualAdapter().MatchesFilter(filters[i]);
            }
        }

        std::scoped_lock lk(g_session_lock);
        u32 signaled = 0;
        bool any_session = false;
        for (size_t si = 0; si < MaxTrackedSessions; ++si) {
            SessionState &session = g_sessions[si];
            if (!session.in_use) {
                continue;
            }
            any_session = true;
            for (size_t i = 0; i < AvailableEventCount; ++i) {
                if (matches[si * AvailableEventCount + i] && session.available[i].active) {
                    os::SignalSystemEvent(std::addressof(session.available[i].ours));
                    ++signaled;
                }
            }
            if (!attached && session.state_active) {
                os::SignalSystemEvent(std::addressof(session.state_ours));
            }
        }

        if (attached) {
            if (!any_session) {
                UBT_LOG_I("mitm: virtual adapter is ready, but eth has not opened usb:hs through us");
            } else if (signaled == 0) {
                UBT_LOG_E("mitm: virtual adapter is ready, but it matches none of the filters eth registered");
            } else {
                UBT_LOG_I("mitm: told eth about the virtual adapter (%u event(s))", signaled);
            }
        }
    }

    void FillStatus(Status *status) {
        std::scoped_lock lk(g_session_lock);
        bool any_session = false;
        for (const auto &session : g_sessions) {
            any_session = any_session || session.in_use;
        }
        status->eth_session_open = any_session;
        status->eth_filter_count = static_cast<u8>(g_seen_filter_count);
    }

}
