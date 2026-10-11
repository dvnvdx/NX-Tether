#include "ubt_virtual_adapter.hpp"
#include "ubt_client_memory.hpp"
#include "ubt_config.hpp"
#include "ubt_log.hpp"
#include "ubt_phone_link.hpp"
#include "ubt_usbhs_mitm.hpp"

namespace ams::ubt {

    namespace {

        constinit util::TypedStorage<VirtualAdapter> g_adapter_storage = {};
        constinit bool g_adapter_constructed = false;

        namespace asix = ::ubtcore::asix;

        constexpr u8 InterfaceDescriptor[9] = { 9, 4, 0, 0, 3, 0xFF, 0xFF, 0x00, 0 };
        constexpr u8 EndpointInterruptIn[7] = { 7, 5, 0x81, 0x03, 0x08, 0x00, 11 };
        constexpr u8 EndpointBulkIn[7]      = { 7, 5, 0x82, 0x02, 0x00, 0x02, 0 };
        constexpr u8 EndpointBulkOut[7]     = { 7, 5, 0x03, 0x02, 0x00, 0x02, 0 };
        constexpr size_t ConfigTotalLength  = 9 + 9 + 7 * 3;

        constexpr u8 InterruptStatusLinkUp[8] = { 0xA1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };

        void BuildDeviceDescriptor(u8 *out, u16 vid, u16 pid) {
            const u8 desc[18] = {
                18, 1,
                0x00, 0x02,             /* bcdUSB 2.00 */
                0xFF, 0xFF, 0x00,       /* vendor specific */
                64,                     /* bMaxPacketSize0 */
                static_cast<u8>(vid), static_cast<u8>(vid >> 8),
                static_cast<u8>(pid), static_cast<u8>(pid >> 8),
                0x01, 0x00,             /* bcdDevice */
                1, 2, 3,                /* strings */
                1,                      /* configurations */
            };
            std::memcpy(out, desc, sizeof(desc));
        }

        void BuildConfigDescriptorHeader(u8 *out) {
            const u8 desc[9] = { 9, 2, static_cast<u8>(ConfigTotalLength), 0, 1, 1, 0, 0xA0, 0x7D };
            std::memcpy(out, desc, sizeof(desc));
        }

        size_t BuildStringDescriptor(u8 *out, const char *text) {
            const size_t len = std::strlen(text);
            out[0] = static_cast<u8>(2 + 2 * len);
            out[1] = 3;
            for (size_t i = 0; i < len; ++i) {
                out[2 + 2 * i] = static_cast<u8>(text[i]);
                out[3 + 2 * i] = 0;
            }
            return 2 + 2 * len;
        }

        /* Object id bookkeeping (see header). */
        constexpr u32 VirtualObjectIdBase  = 0x1C1;
        constexpr u32 VirtualObjectIdCount = 32;
        constinit os::SdkMutex g_object_id_lock;
        constinit u32 g_object_id_used = 0;
        constinit u32 g_object_id_next = 0;

        s64 NowUs() {
            return os::ConvertToTimeSpan(os::GetSystemTick()).GetMicroSeconds();
        }

        /* A bulk IN transfer is never completed sooner than this after it was posted. On real */
        /* hardware a completion cannot arrive before the call that posted the transfer has    */
        /* returned; the client is entitled to rely on that.                                   */
        constexpr s64 MinUrbAgeUs = 150;

        /* How long completion reports may sit unread before it is noted in the log. */
        constexpr s64 StalledReportUs = 500000;

        /* Thread that completes bulk IN transfers that could not be completed on the spot. */
        constexpr size_t PumpThreadStackSize = 0x4000;
        alignas(os::ThreadStackAlignment) constinit u8 g_pump_thread_stack[PumpThreadStackSize];
        constinit os::ThreadType g_pump_thread;
        constinit util::TypedStorage<os::Event> g_pump_event_storage = {};
        constinit std::atomic<bool> g_pump_started = false;

        void PumpThreadFunction(void *) {
            while (true) {
                util::GetReference(g_pump_event_storage).TimedWait(TimeSpan::FromMilliSeconds(250));
                GetVirtualAdapter().KickStalledReports();
                while (true) {
                    const s64 wait_us = GetVirtualAdapter().PumpBulkIn();
                    if (wait_us <= 0) {
                        break;
                    }
                    os::SleepThread(TimeSpan::FromMicroSeconds(wait_us));
                }
            }
        }

        void WakePump() {
            if (g_pump_started.load()) {
                util::GetReference(g_pump_event_storage).Signal();
            }
        }

    }

    void LaunchVirtualAdapterThread() {
        util::ConstructAt(g_pump_event_storage, os::EventClearMode_AutoClear);
        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_pump_thread), PumpThreadFunction, nullptr, g_pump_thread_stack, sizeof(g_pump_thread_stack), 21));
        os::SetThreadNamePointer(std::addressof(g_pump_thread), "ubt::PumpThread");
        os::StartThread(std::addressof(g_pump_thread));
        g_pump_started = true;
    }

    sf::cmif::DomainObjectId AllocateVirtualObjectId() {
        std::scoped_lock lk(g_object_id_lock);
        for (u32 i = 0; i < VirtualObjectIdCount; ++i) {
            const u32 slot = (g_object_id_next + i) % VirtualObjectIdCount;
            if ((g_object_id_used & (1u << slot)) == 0) {
                g_object_id_used |= (1u << slot);
                g_object_id_next = (slot + 1) % VirtualObjectIdCount;
                return sf::cmif::DomainObjectId{ VirtualObjectIdBase + slot };
            }
        }
        return sf::cmif::InvalidDomainObjectId;
    }

    void FreeVirtualObjectId(sf::cmif::DomainObjectId id) {
        std::scoped_lock lk(g_object_id_lock);
        if (id.value >= VirtualObjectIdBase && id.value < VirtualObjectIdBase + VirtualObjectIdCount) {
            g_object_id_used &= ~(1u << (id.value - VirtualObjectIdBase));
        }
    }

    VirtualAdapter &GetVirtualAdapter() {
        if (AMS_UNLIKELY(!g_adapter_constructed)) {
            util::ConstructAt(g_adapter_storage);
            g_adapter_constructed = true;
        }
        return util::GetReference(g_adapter_storage);
    }

    /* ===================================================================================== */
    /* Presence                                                                              */
    /* ===================================================================================== */

    void VirtualAdapter::ChooseProductLocked() {
        m_vid = asix::VendorId;
        m_pid = asix::SupportedProducts[0];

        const u16 forced = config::Get().force_pid;
        if (forced != 0) {
            m_pid = forced;
            UBT_LOG_I("virt: imitating %04x:%04x (forced by config)%s", m_vid, m_pid, asix::IsSupportedProduct(forced) ? "" : " - note: not an adapter model this module knows");
            return;
        }

        /* Look at what eth registered interest in. */
        DeviceFilter filters[8];
        const size_t count = mitm::GetClientFilters(filters, util::size(filters));

        int best = -1;
        bool any_product_filter = false;
        for (size_t i = 0; i < count; ++i) {
            const DeviceFilter &f = filters[i];
            if ((f.flags & DeviceFilterFlag_IdVendor) != 0 && f.id_vendor != asix::VendorId) {
                continue;
            }
            if ((f.flags & DeviceFilterFlag_IdProduct) == 0) {
                continue;
            }
            any_product_filter = true;
            for (size_t j = 0; j < util::size(asix::SupportedProducts); ++j) {
                if (asix::SupportedProducts[j] == f.id_product && (best < 0 || static_cast<int>(j) < best)) {
                    best = static_cast<int>(j);
                }
            }
        }

        if (best >= 0) {
            m_pid = asix::SupportedProducts[best];
            UBT_LOG_I("virt: imitating %04x:%04x (matches a filter eth registered)", m_vid, m_pid);
        } else if (any_product_filter) {
            UBT_LOG_E("virt: none of eth's %zu filters names an adapter this module can imitate; trying %04x:%04x anyway", count, m_vid, m_pid);
        } else {
            UBT_LOG_I("virt: imitating %04x:%04x (default; eth registered %zu filters)", m_vid, m_pid, count);
        }
    }

    void VirtualAdapter::ResetRegistersLocked() {
        m_model.Reset(m_pid, m_mac);
    }

    void VirtualAdapter::BuildQueryOutputLocked(const ::UsbHsInterface *phone_interface) {
        std::memset(std::addressof(m_query), 0, sizeof(m_query));

        ::UsbHsInterfaceInfo &inf = m_query.inf;
        inf.ID = m_interface_id;
        std::memcpy(std::addressof(inf.interface_desc), InterfaceDescriptor, sizeof(InterfaceDescriptor));

        /* Endpoint arrays are indexed by endpoint number - 1. */
        std::memcpy(std::addressof(inf.input_endpoint_descs[0]), EndpointInterruptIn, sizeof(EndpointInterruptIn));
        std::memcpy(std::addressof(inf.input_endpoint_descs[1]), EndpointBulkIn, sizeof(EndpointBulkIn));
        std::memcpy(std::addressof(inf.output_endpoint_descs[2]), EndpointBulkOut, sizeof(EndpointBulkOut));

        if (phone_interface != nullptr) {
            /* Borrow the bus position of the physical device we are standing in for. */
            inf.unk_x8 = phone_interface->inf.unk_x8;
            std::memcpy(m_query.pathstr, phone_interface->pathstr, sizeof(m_query.pathstr));
            m_query.busID = phone_interface->busID;
            m_query.deviceID = phone_interface->deviceID + 0x100;
        } else {
            util::SNPrintf(m_query.pathstr, sizeof(m_query.pathstr), "usb-tether");
            m_query.busID = 1;
            m_query.deviceID = 0x100;
        }
        inf.deviceID_2 = m_query.deviceID;

        u8 *raw = reinterpret_cast<u8 *>(std::addressof(m_query));
        BuildDeviceDescriptor(raw + offsetof(::UsbHsInterface, device_desc), m_vid, m_pid);
        BuildConfigDescriptorHeader(raw + offsetof(::UsbHsInterface, config_desc));
        m_query.timestamp = static_cast<u64>(os::ConvertToTimeSpan(os::GetSystemTick()).GetNanoSeconds());
    }

    void VirtualAdapter::Attach(const u8 *mac, const ::UsbHsInterface *phone_interface) {
        {
            std::scoped_lock lk(m_lock);
            if (m_attached) {
                return;
            }

            /* If the client never released a previous incarnation, reclaim its resources now. */
            for (auto &ep : m_endpoints) {
                this->CloseEndpointLocked(ep);
            }
            if (m_if_events_created) {
                os::DestroySystemEvent(std::addressof(m_if_state_event));
                os::DestroySystemEvent(std::addressof(m_ctrl_event));
                m_if_events_created = false;
            }

            ++m_generation;
            m_interface_id = InterfaceIdBase + static_cast<s32>(m_generation & 0xFFFF);
            std::memcpy(m_mac, mac, sizeof(m_mac));

            this->ChooseProductLocked();
            this->ResetRegistersLocked();
            this->BuildQueryOutputLocked(phone_interface);

            m_backlog_head = 0;
            m_backlog_count = 0;
            m_acquired = false;
            m_attached = true;
            m_logged_data_transfers = 0;
            m_logged_tx_parse_errors = 0;
            m_logged_ctrl_requests = 0;

            UBT_LOG_I("virt: adapter attached, id=0x%x mac=%02x:%02x:%02x:%02x:%02x:%02x", static_cast<unsigned>(m_interface_id), m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);
        }

        mitm::NotifyVirtualAdapterChanged(true);
    }

    void VirtualAdapter::Detach() {
        {
            std::scoped_lock lk(m_lock);
            if (!m_attached) {
                return;
            }
            m_attached = false;

            if (m_acquired) {
                for (auto &ep : m_endpoints) {
                    if (ep.open) {
                        this->FlushPendingLocked(ep, usb::ResultInterrupted());
                    }
                }
                if (m_if_events_created) {
                    os::SignalSystemEvent(std::addressof(m_if_state_event));
                }
            }
            UBT_LOG_I("virt: adapter detached");
        }

        mitm::NotifyVirtualAdapterChanged(false);
    }

    bool VirtualAdapter::IsAttached() {
        std::scoped_lock lk(m_lock);
        return m_attached;
    }

    bool VirtualAdapter::IsAcquired() {
        std::scoped_lock lk(m_lock);
        return m_attached && m_acquired;
    }

    s32 VirtualAdapter::GetInterfaceId() {
        std::scoped_lock lk(m_lock);
        return m_attached ? m_interface_id : -1;
    }

    bool VirtualAdapter::MatchesFilter(const DeviceFilter &f) {
        std::scoped_lock lk(m_lock);
        if (!m_attached) {
            return false;
        }

        const u16 bcd_device = 0x0001;
        if ((f.flags & DeviceFilterFlag_IdVendor) != 0          && f.id_vendor != m_vid)              { return false; }
        if ((f.flags & DeviceFilterFlag_IdProduct) != 0         && f.id_product != m_pid)             { return false; }
        if ((f.flags & DeviceFilterFlag_BcdDeviceMin) != 0      && bcd_device < f.bcd_device_min)     { return false; }
        if ((f.flags & DeviceFilterFlag_BcdDeviceMax) != 0      && bcd_device > f.bcd_device_max)     { return false; }
        if ((f.flags & DeviceFilterFlag_DeviceClass) != 0       && f.device_class != 0xFF)            { return false; }
        if ((f.flags & DeviceFilterFlag_DeviceSubClass) != 0    && f.device_subclass != 0xFF)         { return false; }
        if ((f.flags & DeviceFilterFlag_DeviceProtocol) != 0    && f.device_protocol != 0x00)         { return false; }
        if ((f.flags & DeviceFilterFlag_InterfaceClass) != 0    && f.interface_class != 0xFF)         { return false; }
        if ((f.flags & DeviceFilterFlag_InterfaceSubClass) != 0 && f.interface_subclass != 0xFF)      { return false; }
        if ((f.flags & DeviceFilterFlag_InterfaceProtocol) != 0 && f.interface_protocol != 0x00)      { return false; }
        return true;
    }

    bool VirtualAdapter::GetQueryOutput(::UsbHsInterface *out) {
        std::scoped_lock lk(m_lock);
        if (!m_attached) {
            return false;
        }
        std::memcpy(out, std::addressof(m_query), sizeof(m_query));
        return true;
    }

    Result VirtualAdapter::Acquire(u32 *out_generation, void *out_buffer0, size_t size0, void *out_buffer1, size_t size1) {
        std::scoped_lock lk(m_lock);

        R_UNLESS(m_attached,  usb::ResultInternalStateError());
        R_UNLESS(!m_acquired, usb::ResultResourceBusy());

        R_TRY(os::CreateSystemEvent(std::addressof(m_if_state_event), os::EventClearMode_ManualClear, true));
        if (const Result rc = os::CreateSystemEvent(std::addressof(m_ctrl_event), os::EventClearMode_ManualClear, true); R_FAILED(rc)) {
            os::DestroySystemEvent(std::addressof(m_if_state_event));
            R_RETURN(rc);
        }
        m_if_events_created = true;

        /* The real service returns the interface description split over the two buffers:   */
        /* the 0x1B8 byte profile, and the 0x70 byte tail (path, ids, device/config desc).   */
        /* libnx passes (tail, profile). Pick by size where that is unambiguous.             */
        const void *profile = std::addressof(m_query.inf);
        const void *tail    = reinterpret_cast<const u8 *>(std::addressof(m_query)) + sizeof(::UsbHsInterfaceInfo);
        constexpr size_t ProfileSize = sizeof(::UsbHsInterfaceInfo);
        constexpr size_t TailSize    = sizeof(::UsbHsInterface) - sizeof(::UsbHsInterfaceInfo);

        UBT_LOG_I("virt: AcquireUsbIf buffers: 0x%zx, 0x%zx", size0, size1);
        if (size0 >= ProfileSize && size1 < ProfileSize) {
            std::memcpy(out_buffer0, profile, ProfileSize);
            if (out_buffer1 != nullptr) { std::memcpy(out_buffer1, tail, std::min(size1, TailSize)); }
        } else {
            if (out_buffer0 != nullptr) { std::memcpy(out_buffer0, tail, std::min(size0, TailSize)); }
            if (out_buffer1 != nullptr) { std::memcpy(out_buffer1, profile, std::min(size1, ProfileSize)); }
        }

        m_acquired = true;
        m_ctrl_xfer_id = 0;
        m_ctrl_report = {};
        *out_generation = m_generation;
        UBT_LOG_I("virt: interface acquired by client");
        R_SUCCEED();
    }

    void VirtualAdapter::ReleaseInterface(u32 generation) {
        std::scoped_lock lk(m_lock);
        if (generation != m_generation || !m_acquired) {
            return;
        }

        UBT_LOG_I("virt: interface released by client");
        this->DumpTraceLocked();

        for (auto &ep : m_endpoints) {
            this->CloseEndpointLocked(ep);
        }
        if (m_if_events_created) {
            os::DestroySystemEvent(std::addressof(m_if_state_event));
            os::DestroySystemEvent(std::addressof(m_ctrl_event));
            m_if_events_created = false;
        }
        m_acquired = false;
        m_backlog_count = 0;
    }

    /* ===================================================================================== */
    /* Interface session                                                                     */
    /* ===================================================================================== */

    Result VirtualAdapter::GetStateChangeEvent(u32 generation, os::NativeHandle *out) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(generation == m_generation && m_acquired && m_if_events_created, usb::ResultInternalStateError());
        *out = os::GetReadableHandleOfSystemEvent(std::addressof(m_if_state_event));
        R_SUCCEED();
    }

    Result VirtualAdapter::GetCtrlXferCompletionEvent(u32 generation, os::NativeHandle *out) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(generation == m_generation && m_acquired && m_if_events_created, usb::ResultInternalStateError());
        *out = os::GetReadableHandleOfSystemEvent(std::addressof(m_ctrl_event));
        R_SUCCEED();
    }

    Result VirtualAdapter::GetProfile(u32 generation, void *out, size_t size, u8 alt_setting) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(generation == m_generation && m_acquired, usb::ResultInternalStateError());
        R_UNLESS(alt_setting == 0,                         usb::ResultInvalidParameter());
        if (out != nullptr) {
            std::memcpy(out, std::addressof(m_query.inf), std::min(size, sizeof(m_query.inf)));
        }
        R_SUCCEED();
    }

    Result VirtualAdapter::ResetDevice(u32 generation) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(this->IsLiveLocked(generation), usb::ResultInternalStateError());
        UBT_LOG_I("virt: ResetDevice");
        this->ResetRegistersLocked();
        R_SUCCEED();
    }

    Result VirtualAdapter::CtrlXferAsync(u32 generation, const CtrlXferIn &in) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(generation == m_generation && m_acquired && m_if_events_created, usb::ResultInternalStateError());

        u32 transferred = 0;
        Result result = usb::ResultInterrupted();
        if (m_attached) {
            result = this->HandleControlLocked(in, std::addressof(transferred));
        }

        m_ctrl_report = {};
        m_ctrl_report.xferId = ++m_ctrl_xfer_id;
        m_ctrl_report.res = result.GetValue();
        m_ctrl_report.requestedSize = in.w_length;
        m_ctrl_report.transferredSize = transferred;
        m_ctrl_report.id = 0;

        os::SignalSystemEvent(std::addressof(m_ctrl_event));
        R_SUCCEED();
    }

    Result VirtualAdapter::GetCtrlXferReport(u32 generation, void *out, size_t size) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(generation == m_generation && m_acquired, usb::ResultInternalStateError());
        if (out != nullptr) {
            std::memcpy(out, std::addressof(m_ctrl_report), std::min(size, sizeof(m_ctrl_report)));
        }
        R_SUCCEED();
    }

    Result VirtualAdapter::HandleControlLocked(const CtrlXferIn &in, u32 *out_transferred) {
        const bool device_to_host = (in.bm_request_type & 0x80) != 0;
        const u8 type = (in.bm_request_type >> 5) & 3;
        u8 *data = m_staging;
        const u32 length = in.w_length;

        ++m_ctrl_requests;

        /* Fetch the payload of host-to-device requests. */
        if (!device_to_host && length > 0) {
            if (const Result rc = GetClientMemory().Read(data, in.buffer, length); R_FAILED(rc)) {
                UBT_LOG_E("virt: ctrl OUT req=0x%02x: cannot read client buffer 0x%llx: 0x%x", in.b_request, static_cast<unsigned long long>(in.buffer), rc.GetValue());
                R_RETURN(usb::ResultTransactionError());
            }
        } else {
            std::memset(data, 0, length);
        }

        u32 size = device_to_host ? length : 0;
        Result result = ResultSuccess();
        if (type == 0) {
            result = this->HandleStandardRequestLocked(in, data, std::addressof(size));
        } else if (type == 2) {
            result = this->HandleVendorRequestLocked(in, data, std::addressof(size));
        } else {
            ++m_unknown_requests;
            UBT_LOG_E("virt: unsupported request type: bmRequestType=0x%02x bRequest=0x%02x", in.bm_request_type, in.b_request);
            result = usb::ResultTransactionError();
        }

        if (R_SUCCEEDED(result) && device_to_host) {
            size = std::min(size, length);
            if (size > 0) {
                if (const Result rc = GetClientMemory().Write(in.buffer, data, size); R_FAILED(rc)) {
                    UBT_LOG_E("virt: ctrl IN req=0x%02x: cannot write client buffer 0x%llx: 0x%x", in.b_request, static_cast<unsigned long long>(in.buffer), rc.GetValue());
                    R_RETURN(usb::ResultTransactionError());
                }
            }
            *out_transferred = size;
        } else if (R_SUCCEEDED(result)) {
            *out_transferred = length;
        }

        /* Log the start of the conversation in full (that is the driver's init sequence, the   */
        /* part worth studying), then go quiet so that periodic link polling does not fill the  */
        /* log. Trace level logs everything.                                                    */
        const bool log_request = log::GetLevel() >= log::Level_Trace || (log::GetLevel() >= log::Level_Debug && m_logged_ctrl_requests < MaxLoggedCtrlRequests);
        if (log_request) {
            if (++m_logged_ctrl_requests == MaxLoggedCtrlRequests) {
                UBT_LOG_D("virt: (further control requests are not logged; set log_level=3 to see all)");
            }
            UBT_LOG_D("virt: ctrl %s type=%02x req=%02x val=%04x idx=%04x len=%u -> 0x%x", device_to_host ? "IN " : "OUT", in.bm_request_type, in.b_request, in.w_value, in.w_index, length, result.GetValue());
            const u32 shown = device_to_host ? *out_transferred : length;
            if (shown > 0) {
                log::Hex(log::Level_Debug, "virt:      data", data, shown, 32);
            }
        }

        R_RETURN(result);
    }

    Result VirtualAdapter::HandleStandardRequestLocked(const CtrlXferIn &in, u8 *data, u32 *out_size) {
        switch (in.b_request) {
            case 0x00: /* GET_STATUS */
                data[0] = 0;
                data[1] = 0;
                *out_size = 2;
                R_SUCCEED();
            case 0x01: /* CLEAR_FEATURE */
            case 0x03: /* SET_FEATURE */
            case 0x09: /* SET_CONFIGURATION */
            case 0x0B: /* SET_INTERFACE */
                R_SUCCEED();
            case 0x08: /* GET_CONFIGURATION */
                data[0] = 1;
                *out_size = 1;
                R_SUCCEED();
            case 0x0A: /* GET_INTERFACE */
                data[0] = 0;
                *out_size = 1;
                R_SUCCEED();
            case 0x06: /* GET_DESCRIPTOR */
            {
                const u8 desc_type = static_cast<u8>(in.w_value >> 8);
                const u8 desc_index = static_cast<u8>(in.w_value);
                switch (desc_type) {
                    case 1: /* device */
                        BuildDeviceDescriptor(data, m_vid, m_pid);
                        *out_size = 18;
                        R_SUCCEED();
                    case 2: /* configuration */
                        BuildConfigDescriptorHeader(data);
                        std::memcpy(data + 9, InterfaceDescriptor, 9);
                        std::memcpy(data + 18, EndpointInterruptIn, 7);
                        std::memcpy(data + 25, EndpointBulkIn, 7);
                        std::memcpy(data + 32, EndpointBulkOut, 7);
                        *out_size = ConfigTotalLength;
                        R_SUCCEED();
                    case 3: /* string */
                        switch (desc_index) {
                            case 0:
                                data[0] = 4; data[1] = 3; data[2] = 0x09; data[3] = 0x04;
                                *out_size = 4;
                                R_SUCCEED();
                            case 1:
                                *out_size = BuildStringDescriptor(data, "ASIX Elec. Corp.");
                                R_SUCCEED();
                            case 2:
                                *out_size = BuildStringDescriptor(data, "AX88772");
                                R_SUCCEED();
                            case 3:
                            {
                                char serial[16];
                                util::SNPrintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", m_mac[0], m_mac[1], m_mac[2], m_mac[3], m_mac[4], m_mac[5]);
                                *out_size = BuildStringDescriptor(data, serial);
                                R_SUCCEED();
                            }
                            default:
                                break;
                        }
                        break;
                    default:
                        break;
                }
                UBT_LOG_I("virt: GET_DESCRIPTOR type=%u index=%u not provided", desc_type, desc_index);
                R_THROW(usb::ResultTransactionError());
            }
            default:
                ++m_unknown_requests;
                UBT_LOG_E("virt: unknown standard request 0x%02x", in.b_request);
                R_THROW(usb::ResultTransactionError());
        }
    }

    Result VirtualAdapter::HandleVendorRequestLocked(const CtrlXferIn &in, u8 *data, u32 *out_size) {
        const bool device_to_host = (in.bm_request_type & 0x80) != 0;

        const asix::RequestResult result = m_model.HandleVendorRequest(in.b_request, device_to_host, in.w_value, in.w_index, in.w_length, data);
        *out_size = result.out_size;

        switch (result.event) {
            case asix::Event_RxCtlChanged:
                UBT_LOG_I("virt: RX control 0x%04x -> 0x%04x%s", result.old_value, m_model.rx_ctl, m_model.IsRxEnabled() ? " (receiver on)" : " (receiver off)");
                /* On the AX88772B these bits select the receive header format; on the older  */
                /* chips the same bits are the maximum burst size and need no handling.       */
                if (asix::IsChecksumCapableProduct(m_pid) && (m_model.rx_ctl & asix::RxCtlIpHeaderAlign) != 0) {
                    UBT_LOG_E("virt: client requested IP header alignment, which is not implemented");
                }
                break;
            case asix::Event_MediumChanged:
                UBT_LOG_I("virt: medium mode 0x%04x -> 0x%04x", result.old_value, m_model.medium);
                break;
            case asix::Event_NodeIdSet:
                UBT_LOG_I("virt: client set node id %02x:%02x:%02x:%02x:%02x:%02x", m_model.node_id[0], m_model.node_id[1], m_model.node_id[2], m_model.node_id[3], m_model.node_id[4], m_model.node_id[5]);
                break;
            case asix::Event_UnknownRequest:
                /* Answered with zeros / accepted; make it obvious in the log. */
                ++m_unknown_requests;
                UBT_LOG_E("virt: UNKNOWN vendor request 0x%02x (%s) val=%04x idx=%04x len=%u", in.b_request, device_to_host ? "IN" : "OUT", in.w_value, in.w_index, in.w_length);
                break;
            default:
                break;
        }

        R_SUCCEED();
    }

    /* ===================================================================================== */
    /* Endpoints                                                                             */
    /* ===================================================================================== */

    Result VirtualAdapter::OpenEndpoint(u32 generation, int *out_index, EndpointDescriptor *out_desc, const OpenUsbEpIn &in) {
        std::scoped_lock lk(m_lock);
        R_UNLESS(this->IsLiveLocked(generation), usb::ResultInternalStateError());

        int index = -1;
        const u8 *desc = nullptr;
        if (in.ep_number == 1 && in.ep_direction == 2) {
            index = EndpointIndex_Interrupt;
            desc = EndpointInterruptIn;
        } else if (in.ep_number == 2 && in.ep_direction == 2) {
            index = EndpointIndex_BulkIn;
            desc = EndpointBulkIn;
        } else if (in.ep_number == 3 && in.ep_direction == 1) {
            index = EndpointIndex_BulkOut;
            desc = EndpointBulkOut;
        }

        UBT_LOG_I("virt: OpenUsbEp num=%u dir=%u type=%u maxUrbCount=%u maxXferSize=0x%x -> %d", in.ep_number, in.ep_direction, in.ep_type, in.max_urb_count, in.max_xfer_size, index);
        R_UNLESS(index >= 0, usb::ResultInvalidParameter());

        Endpoint &ep = m_endpoints[index];
        R_UNLESS(!ep.open, usb::ResultResourceBusy());

        R_TRY(os::CreateSystemEvent(std::addressof(ep.completion_event), os::EventClearMode_ManualClear, true));
        ep.event_created = true;
        ep.open = true;
        ep.ring_mapped = false;
        ep.ring = nullptr;
        ep.ring_capacity = 0;
        ep.max_urb_count = in.max_urb_count;
        ep.max_xfer_size = in.max_xfer_size;
        ep.next_xfer_id = 1;
        ep.smmu_address = 0;
        ep.smmu_size = 0;
        ep.report_head = 0;
        ep.report_count = 0;
        ep.pending_head = 0;
        ep.pending_count = 0;
        ep.seen_read_index = 0;
        ep.seen_read_us = 0;

        std::memcpy(out_desc->data, desc, sizeof(out_desc->data));
        *out_index = index;
        R_SUCCEED();
    }

    void VirtualAdapter::CloseEndpointLocked(Endpoint &ep) {
        if (ep.ring_mapped) {
            os::UnmapTransferMemory(std::addressof(ep.ring_tmem));
            os::DestroyTransferMemory(std::addressof(ep.ring_tmem));
            ep.ring_mapped = false;
            ep.ring = nullptr;
        }
        if (ep.event_created) {
            os::DestroySystemEvent(std::addressof(ep.completion_event));
            ep.event_created = false;
        }
        ep.open = false;
        ep.pending_count = 0;
        ep.report_count = 0;
    }

    void VirtualAdapter::CloseEndpoint(u32 generation, int index) {
        std::scoped_lock lk(m_lock);
        if (generation != m_generation || index < 0 || index >= EndpointIndex_Count) {
            return;
        }
        UBT_LOG_I("virt: endpoint %d closed", index);
        this->CloseEndpointLocked(m_endpoints[index]);
    }

    Result VirtualAdapter::CancelEndpoint(u32 generation, int index) {
        std::scoped_lock lk(m_lock);
        R_SUCCEED_IF(generation != m_generation);
        Endpoint &ep = m_endpoints[index];
        if (ep.open) {
            this->FlushPendingLocked(ep, usb::ResultInterrupted());
        }
        R_SUCCEED();
    }

    Result VirtualAdapter::GetCompletionEvent(u32 generation, int index, os::NativeHandle *out) {
        std::scoped_lock lk(m_lock);
        Endpoint &ep = m_endpoints[index];
        R_UNLESS(generation == m_generation && ep.open && ep.event_created, usb::ResultInternalStateError());
        *out = os::GetReadableHandleOfSystemEvent(std::addressof(ep.completion_event));
        R_SUCCEED();
    }

    void VirtualAdapter::CompleteLocked(Endpoint &ep, u32 xfer_id, u64 id, Result result, u32 requested, u32 transferred) {
        ::UsbHsXferReport report = {};
        report.xferId = xfer_id;
        report.res = result.GetValue();
        report.requestedSize = requested;
        report.transferredSize = transferred;
        report.id = id;

        this->TraceLocked('C', static_cast<u8>(std::addressof(ep) - m_endpoints), static_cast<u16>(ep.pending_count), xfer_id, result.GetValue(), transferred);

        if (ep.ring_mapped && ep.ring != nullptr && ep.ring_capacity > 0) {
            ::UsbHsXferReport *ring_reports = reinterpret_cast<::UsbHsXferReport *>(ep.ring + 1);
            const u64 write_index = ep.ring->write_index;
            if (write_index < ep.ring_capacity) {
                ring_reports[write_index] = report;
                __asm__ __volatile__("dmb sy" ::: "memory");
                ep.ring->write_index = (write_index + 1) % ep.ring_capacity;
            }
        } else {
            if (ep.report_count == ReportQueueSize) {
                /* Should not happen: the client is not collecting its reports. Drop the oldest. */
                ep.report_head = (ep.report_head + 1) % ReportQueueSize;
                --ep.report_count;
            }
            ep.reports[(ep.report_head + ep.report_count) % ReportQueueSize] = report;
            ++ep.report_count;
        }

        if (ep.event_created) {
            os::SignalSystemEvent(std::addressof(ep.completion_event));
        }
    }

    void VirtualAdapter::FlushPendingLocked(Endpoint &ep, Result result) {
        while (ep.pending_count > 0) {
            const PendingUrb urb = ep.pending[ep.pending_head];
            ep.pending_head = (ep.pending_head + 1) % MaxPendingUrbs;
            --ep.pending_count;
            this->CompleteLocked(ep, urb.xfer_id, urb.id, result, urb.size, 0);
        }
    }

    Result VirtualAdapter::GetXferReport(u32 generation, int index, u32 *out_count, void *out, u32 max_count) {
        std::scoped_lock lk(m_lock);
        Endpoint &ep = m_endpoints[index];
        R_UNLESS(generation == m_generation && ep.open, usb::ResultInternalStateError());

        /* The IPC buffer has no guaranteed alignment, so copy bytewise. */
        u32 count = 0;
        while (count < max_count && ep.report_count > 0) {
            std::memcpy(static_cast<u8 *>(out) + static_cast<size_t>(count) * sizeof(::UsbHsXferReport), std::addressof(ep.reports[ep.report_head]), sizeof(::UsbHsXferReport));
            ++count;
            ep.report_head = (ep.report_head + 1) % ReportQueueSize;
            --ep.report_count;
        }
        *out_count = count;
        this->TraceLocked('R', static_cast<u8>(index), static_cast<u16>(count), max_count, ep.report_count, 0);
        R_SUCCEED();
    }

    Result VirtualAdapter::CreateSmmuSpace(u32 generation, int index, u64 address, u32 size) {
        std::scoped_lock lk(m_lock);
        Endpoint &ep = m_endpoints[index];
        R_UNLESS(generation == m_generation && ep.open, usb::ResultInternalStateError());
        UBT_LOG_I("virt: ep %d CreateSmmuSpace addr=0x%llx size=0x%x", index, static_cast<unsigned long long>(address), size);
        ep.smmu_address = address;
        ep.smmu_size = size;
        R_SUCCEED();
    }

    Result VirtualAdapter::ShareReportRing(u32 generation, int index, os::NativeHandle tmem, u32 size) {
        std::scoped_lock lk(m_lock);
        Endpoint &ep = m_endpoints[index];
        if (generation != m_generation || !ep.open || ep.ring_mapped) {
            R_DISCARD(svc::CloseHandle(tmem));
            R_THROW(usb::ResultInternalStateError());
        }

        /* Take ownership of the handle and map the ring into our address space. */
        os::AttachTransferMemory(std::addressof(ep.ring_tmem), size, tmem, true);

        void *address = nullptr;
        Result rc = os::MapTransferMemory(std::addressof(address), std::addressof(ep.ring_tmem), os::MemoryPermission_ReadWrite);
        if (R_FAILED(rc)) {
            rc = os::MapTransferMemory(std::addressof(address), std::addressof(ep.ring_tmem), os::MemoryPermission_None);
        }
        if (R_FAILED(rc)) {
            rc = os::MapTransferMemory(std::addressof(address), std::addressof(ep.ring_tmem), os::MemoryPermission_ReadOnly);
        }
        if (R_FAILED(rc)) {
            UBT_LOG_E("virt: ep %d ShareReportRing size=0x%x: cannot map: 0x%x", index, size, rc.GetValue());
            os::DestroyTransferMemory(std::addressof(ep.ring_tmem));
            R_RETURN(rc);
        }

        ep.ring = static_cast<::UsbHsRingHeader *>(address);
        /* The client wraps its read position after 0x20 reports per URB plus one. This was   */
        /* measured against the LAN driver on 22.5.0 (33 for one URB, 129 for four); using    */
        /* 0x21 per URB makes the two sides disagree from the 130th report on, and the driver */
        /* aborts.                                                                            */
        ep.ring_capacity = static_cast<u64>(ep.max_urb_count) * 0x20 + 1;
        const u64 fits = size > sizeof(::UsbHsRingHeader) ? (size - sizeof(::UsbHsRingHeader)) / sizeof(::UsbHsXferReport) : 0;
        ep.ring_capacity = std::min(ep.ring_capacity, fits);
        ep.ring_mapped = true;

        UBT_LOG_I("virt: ep %d ShareReportRing size=0x%x capacity=%llu", index, size, static_cast<unsigned long long>(ep.ring_capacity));
        R_SUCCEED();
    }

    void VirtualAdapter::TraceLocked(char kind, u8 ep, u16 a, u32 b, u32 c, u32 d) {
        TraceEntry &e = m_trace[m_trace_next];
        e.time_us = static_cast<u32>(NowUs());
        e.kind = kind;
        e.ep = ep;
        e.a = a;
        e.b = b;
        e.c = c;
        e.d = d;
        e.ring_read = 0xFFFF;
        e.ring_write = 0xFFFF;
        if (ep < EndpointIndex_Count && m_endpoints[ep].ring_mapped && m_endpoints[ep].ring != nullptr) {
            e.ring_read = static_cast<u16>(m_endpoints[ep].ring->read_index);
            e.ring_write = static_cast<u16>(m_endpoints[ep].ring->write_index);
        }
        m_trace_next = (m_trace_next + 1) % TraceSize;
        ++m_trace_total;
    }

    void VirtualAdapter::DumpTraceLocked() {
        /* Newest first and not too many: after a crash there is little time left to write. */
        const u32 count = std::min<u32>(std::min<u32>(m_trace_total, TraceSize), 300);
        for (int i = 0; i < EndpointIndex_Count; ++i) {
            const Endpoint &ep = m_endpoints[i];
            UBT_LOG_I("trace: ep%d open=%d pending=%u next_xfer=%x ring read=%lld write=%lld capacity=%llu", i, ep.open, ep.pending_count, ep.next_xfer_id,
                      (ep.ring_mapped && ep.ring != nullptr) ? static_cast<long long>(ep.ring->read_index) : -1ll,
                      (ep.ring_mapped && ep.ring != nullptr) ? static_cast<long long>(ep.ring->write_index) : -1ll,
                      static_cast<unsigned long long>(ep.ring_capacity));
        }
        UBT_LOG_I("trace: backlog=%u. last %u of %u events, newest first; columns: time_us kind ep a b c d [ring read/write before the event]", m_backlog_count, count, m_trace_total);
        UBT_LOG_I("trace: P=post(urbs, xfer, size, addr) C=complete(pending, xfer, result, bytes) K=unread reports(count, read idx) X=drop(len, backlog, pending)");
        for (u32 i = 0; i < count; ++i) {
            const TraceEntry &e = m_trace[(m_trace_next + TraceSize - 1 - i) % TraceSize];
            UBT_LOG_I("trace: %10u %c ep%u %u %x %x %x [%x/%x]", e.time_us, e.kind, e.ep, e.a, e.b, e.c, e.d, e.ring_read, e.ring_write);
        }
        m_trace_total = 0;
        m_trace_next = 0;
    }

    void VirtualAdapter::KickStalledReports() {
        std::scoped_lock lk(m_lock);
        if (!m_attached || !m_acquired) {
            return;
        }

        /* Diagnostic only: note when the client stops collecting its completion reports. */
        const s64 now_us = NowUs();
        for (int i = 0; i < EndpointIndex_Count; ++i) {
            Endpoint &ep = m_endpoints[i];
            if (!ep.open || !ep.event_created || !ep.ring_mapped || ep.ring == nullptr) {
                continue;
            }
            const u64 read_index = ep.ring->read_index;
            const u64 write_index = ep.ring->write_index;
            if (read_index == write_index || read_index != ep.seen_read_index) {
                ep.seen_read_index = read_index;
                ep.seen_read_us = now_us;
                continue;
            }
            if (now_us - ep.seen_read_us >= StalledReportUs) {
                const u64 unread = (write_index + ep.ring_capacity - read_index) % std::max<u64>(ep.ring_capacity, 1);
                this->TraceLocked('K', static_cast<u8>(i), static_cast<u16>(unread), static_cast<u32>(read_index), 0, 0);
                if (m_logged_kicks < 20) {
                    ++m_logged_kicks;
                    UBT_LOG_I("virt: ep %d: client has left %llu report(s) unread for %lld ms (ring read=%llu write=%llu)", i, static_cast<unsigned long long>(unread), static_cast<long long>((now_us - ep.seen_read_us) / 1000),
                              static_cast<unsigned long long>(read_index), static_cast<unsigned long long>(write_index));
                }
                ep.seen_read_us = now_us;
            }
        }
    }

    s64 VirtualAdapter::PumpBulkIn() {
        std::scoped_lock lk(m_lock);
        if (!m_attached || !m_acquired) {
            return 0;
        }
        return this->FillBulkInLocked();
    }

    s64 VirtualAdapter::FillBulkInLocked() {
        Endpoint &ep = m_endpoints[EndpointIndex_BulkIn];
        if (!ep.open) {
            return 0;
        }

        while (m_backlog_count > 0 && ep.pending_count > 0) {
            const PendingUrb urb = ep.pending[ep.pending_head];

            /* Too fresh: the client may not even have seen the reply to its post yet. */
            const s64 age_us = NowUs() - urb.posted_us;
            if (age_us < MinUrbAgeUs) {
                return MinUrbAgeUs - age_us;
            }
            /* Never hand over more than the real chip would put into one transfer. */
            const bool single_frame = config::Get().rx_single_frame;
            const size_t capacity = std::min<size_t>(std::min<size_t>(urb.size, StagingSize), m_model.MaxBurstSize());

            /* Pack as many queued frames as fit into this transfer. */
            size_t used = 0;
            u32 frames = 0;
            while (m_backlog_count > 0) {
                const size_t packed = m_model.PackFrame(m_staging + used, capacity - used, m_backlog[m_backlog_head], m_backlog_length[m_backlog_head]);
                if (packed == 0) {
                    break;
                }
                used += packed;
                ++frames;
                m_backlog_head = (m_backlog_head + 1) % BacklogSlots;
                --m_backlog_count;
                if (single_frame) {
                    break;
                }
            }

            if (frames == 0) {
                /* The client's buffer cannot hold even one frame. Drop the frame rather than stall. */
                UBT_LOG_E("virt: bulk IN buffer of 0x%x bytes is too small for a %u byte frame; dropping it", urb.size, m_backlog_length[m_backlog_head]);
                m_backlog_head = (m_backlog_head + 1) % BacklogSlots;
                --m_backlog_count;
                ++m_rx_dropped;
                continue;
            }

            ep.pending_head = (ep.pending_head + 1) % MaxPendingUrbs;
            --ep.pending_count;

            Result result = GetClientMemory().Write(urb.address, m_staging, used);
            if (R_FAILED(result)) {
                result = usb::ResultTransactionError();
                used = 0;
            }

            if (m_logged_data_transfers < 6) {
                ++m_logged_data_transfers;
                UBT_LOG_D("virt: bulk IN  -> client: %u frame(s), %zu bytes into buffer 0x%llx (size 0x%x)", frames, used, static_cast<unsigned long long>(urb.address), urb.size);
                log::Hex(log::Level_Debug, "virt:      data", m_staging, used, 48);
            }

            this->CompleteLocked(ep, urb.xfer_id, urb.id, result, urb.size, static_cast<u32>(used));
        }
        return 0;
    }

    void VirtualAdapter::ProcessBulkOutLocked(u64 address, u32 size) {
        const size_t length = std::min<size_t>(size, StagingSize);
        if (R_FAILED(GetClientMemory().Read(m_staging, address, length))) {
            return;
        }

        if (m_logged_data_transfers < 6) {
            ++m_logged_data_transfers;
            UBT_LOG_D("virt: bulk OUT <- client: %zu bytes from buffer 0x%llx", length, static_cast<unsigned long long>(address));
            log::Hex(log::Level_Debug, "virt:      data", m_staging, length, 48);
        }

        const long bad_offset = asix::Model::ParseTransmit(m_staging, length, [](const u8 *frame, size_t frame_length) {
            link::SendFrame(frame, frame_length);
        });
        if (bad_offset >= 0 && m_logged_tx_parse_errors < 4) {
            ++m_logged_tx_parse_errors;
            UBT_LOG_E("virt: bulk OUT: bad frame header at offset %ld of %zu", bad_offset, length);
            log::Hex(log::Level_Error, "virt:      data", m_staging + bad_offset, length - static_cast<size_t>(bad_offset), 32);
        }
    }

    Result VirtualAdapter::PostBuffer(u32 generation, int index, u32 *out_xfer_id, u64 address, const u32 *urb_sizes, u32 urb_count, u64 id) {
        std::scoped_lock lk(m_lock);
        Endpoint &ep = m_endpoints[index];
        R_UNLESS(generation == m_generation && m_acquired && ep.open, usb::ResultInternalStateError());

        const u32 xfer_id = ep.next_xfer_id++;
        *out_xfer_id = xfer_id;

        const s64 now_us = NowUs();
        u64 urb_address = address;
        for (u32 i = 0; i < urb_count; ++i) {
            const u32 size = urb_sizes[i];
            this->TraceLocked('P', static_cast<u8>(index), static_cast<u16>(urb_count), xfer_id, size, static_cast<u32>(urb_address));

            if (!m_attached) {
                /* The adapter went away: fail the transfer through its report. */
                this->CompleteLocked(ep, xfer_id, id, usb::ResultInterrupted(), size, 0);
            } else if (index == EndpointIndex_BulkOut) {
                this->ProcessBulkOutLocked(urb_address, size);
                this->CompleteLocked(ep, xfer_id, id, ResultSuccess(), size, size);
            } else if (ep.pending_count == MaxPendingUrbs) {
                this->CompleteLocked(ep, xfer_id, id, usb::ResultResourceBusy(), size, 0);
            } else {
                PendingUrb &urb = ep.pending[(ep.pending_head + ep.pending_count) % MaxPendingUrbs];
                urb.xfer_id = xfer_id;
                urb.size = size;
                urb.id = id;
                urb.address = urb_address;
                urb.posted_us = now_us;
                ++ep.pending_count;
            }

            urb_address += size;
        }

        /* Waiting frames are handed over by the completion thread, after this call returned. */
        if (index == EndpointIndex_BulkIn && m_attached && m_backlog_count > 0) {
            WakePump();
        }
        R_SUCCEED();
    }

    void VirtualAdapter::DeliverFrame(const u8 *frame, size_t length) {
        std::scoped_lock lk(m_lock);

        if (!m_attached) {
            /* Nobody to deliver to (link test mode, or the bridge is switched off). */
            return;
        }
        if (!m_acquired || !m_endpoints[EndpointIndex_BulkIn].open || !m_model.IsRxEnabled() || length < 14 || length > EthernetFrameMax) {
            ++m_rx_dropped;
            return;
        }
        if (m_backlog_count == BacklogSlots) {
            ++m_rx_dropped;
            this->TraceLocked('X', EndpointIndex_BulkIn, static_cast<u16>(length), m_backlog_count, m_endpoints[EndpointIndex_BulkIn].pending_count, 0);
            return;
        }

        const u32 slot = (m_backlog_head + m_backlog_count) % BacklogSlots;
        std::memcpy(m_backlog[slot], frame, length);
        m_backlog_length[slot] = static_cast<u16>(length);
        ++m_backlog_count;
    }

    void VirtualAdapter::FlushFrames() {
        std::scoped_lock lk(m_lock);
        if (!m_attached || !m_acquired || m_backlog_count == 0) {
            return;
        }
        if (this->FillBulkInLocked() > 0) {
            WakePump();
        }
    }

    void VirtualAdapter::Tick() {
        std::scoped_lock lk(m_lock);
        if (!m_attached || !m_acquired) {
            return;
        }

        /* Answer one pending interrupt transfer with the link status, like the real chip does */
        /* at its polling interval.                                                            */
        Endpoint &ep = m_endpoints[EndpointIndex_Interrupt];
        if (ep.open && ep.pending_count > 0) {
            const PendingUrb urb = ep.pending[ep.pending_head];
            ep.pending_head = (ep.pending_head + 1) % MaxPendingUrbs;
            --ep.pending_count;

            const u32 size = std::min<u32>(urb.size, sizeof(InterruptStatusLinkUp));
            Result result = GetClientMemory().Write(urb.address, InterruptStatusLinkUp, size);
            if (R_FAILED(result)) {
                result = usb::ResultTransactionError();
            }
            this->CompleteLocked(ep, urb.xfer_id, urb.id, result, urb.size, R_SUCCEEDED(result) ? size : 0);
        }
        ++m_tick_counter;
    }

    void VirtualAdapter::FillStatus(Status *status) {
        std::scoped_lock lk(m_lock);
        status->virt_attached = m_attached;
        status->virt_acquired = m_attached && m_acquired;
        status->virt_rx_enabled = m_attached && m_acquired && m_model.IsRxEnabled();
        status->emu_vid = m_vid;
        status->emu_pid = m_pid;
        status->ctrl_requests = m_ctrl_requests;
        status->unknown_requests = m_unknown_requests;
        status->rx_dropped += m_rx_dropped;
        status->memory_mode = GetClientMemory().GetMode();
    }

    /* ===================================================================================== */
    /* IPC objects                                                                           */
    /* ===================================================================================== */

    ClientIfSessionImpl::~ClientIfSessionImpl() {
        GetVirtualAdapter().ReleaseInterface(m_generation);
        FreeVirtualObjectId(m_object_id);
    }

    Result ClientIfSessionImpl::GetStateChangeEvent(sf::OutCopyHandle out_event) {
        os::NativeHandle handle = os::InvalidNativeHandle;
        R_TRY(GetVirtualAdapter().GetStateChangeEvent(m_generation, std::addressof(handle)));
        out_event.SetValue(handle, false);
        R_SUCCEED();
    }

    Result ClientIfSessionImpl::SetInterface(const sf::OutBuffer &out_profile, u8 id) {
        UBT_LOG_D("virt: SetInterface(%u)", id);
        R_RETURN(GetVirtualAdapter().GetProfile(m_generation, out_profile.GetPointer(), out_profile.GetSize(), id));
    }

    Result ClientIfSessionImpl::GetInterface(const sf::OutBuffer &out_profile) {
        R_RETURN(GetVirtualAdapter().GetProfile(m_generation, out_profile.GetPointer(), out_profile.GetSize(), 0));
    }

    Result ClientIfSessionImpl::GetAlternateInterface(const sf::OutBuffer &out_profile, u8 id) {
        UBT_LOG_D("virt: GetAlternateInterface(%u)", id);
        R_RETURN(GetVirtualAdapter().GetProfile(m_generation, out_profile.GetPointer(), out_profile.GetSize(), id));
    }

    Result ClientIfSessionImpl::GetCurrentFrame(sf::Out<u32> out_frame) {
        out_frame.SetValue(static_cast<u32>(os::ConvertToTimeSpan(os::GetSystemTick()).GetMilliSeconds()));
        R_SUCCEED();
    }

    Result ClientIfSessionImpl::CtrlXferAsync(CtrlXferIn in) {
        R_RETURN(GetVirtualAdapter().CtrlXferAsync(m_generation, in));
    }

    Result ClientIfSessionImpl::GetCtrlXferCompletionEvent(sf::OutCopyHandle out_event) {
        os::NativeHandle handle = os::InvalidNativeHandle;
        R_TRY(GetVirtualAdapter().GetCtrlXferCompletionEvent(m_generation, std::addressof(handle)));
        out_event.SetValue(handle, false);
        R_SUCCEED();
    }

    Result ClientIfSessionImpl::GetCtrlXferReport(const sf::OutBuffer &out_report) {
        R_RETURN(GetVirtualAdapter().GetCtrlXferReport(m_generation, out_report.GetPointer(), out_report.GetSize()));
    }

    Result ClientIfSessionImpl::ResetDevice() {
        R_RETURN(GetVirtualAdapter().ResetDevice(m_generation));
    }

    Result ClientIfSessionImpl::OpenUsbEp(sf::Out<EndpointDescriptor> out_desc, sf::Out<sf::SharedPointer<impl::IClientEpSession>> out, OpenUsbEpIn in) {
        int index = -1;
        EndpointDescriptor desc = {};
        R_TRY(GetVirtualAdapter().OpenEndpoint(m_generation, std::addressof(index), std::addressof(desc), in));

        const auto object_id = AllocateVirtualObjectId();
        out_desc.SetValue(desc);
        out.SetValue(sf::CreateSharedObjectEmplaced<impl::IClientEpSession, ClientEpSessionImpl>(m_generation, index, object_id), object_id);
        R_SUCCEED();
    }

    ClientEpSessionImpl::~ClientEpSessionImpl() {
        GetVirtualAdapter().CloseEndpoint(m_generation, m_index);
        FreeVirtualObjectId(m_object_id);
    }

    Result ClientEpSessionImpl::ReOpen() {
        UBT_LOG_D("virt: ep %d ReOpen", m_index);
        R_SUCCEED();
    }

    Result ClientEpSessionImpl::Close() {
        UBT_LOG_D("virt: ep %d Close", m_index);
        R_RETURN(GetVirtualAdapter().CancelEndpoint(m_generation, m_index));
    }

    Result ClientEpSessionImpl::GetCompletionEvent(sf::OutCopyHandle out_event) {
        os::NativeHandle handle = os::InvalidNativeHandle;
        R_TRY(GetVirtualAdapter().GetCompletionEvent(m_generation, m_index, std::addressof(handle)));
        out_event.SetValue(handle, false);
        R_SUCCEED();
    }

    Result ClientEpSessionImpl::PopulateRing() {
        R_SUCCEED();
    }

    Result ClientEpSessionImpl::PostBufferAsync(sf::Out<u32> out_xfer_id, PostBufferIn in) {
        u32 xfer_id = 0;
        const u32 size = in.size;
        R_TRY(GetVirtualAdapter().PostBuffer(m_generation, m_index, std::addressof(xfer_id), in.buffer, std::addressof(size), 1, in.id));
        out_xfer_id.SetValue(xfer_id);
        R_SUCCEED();
    }

    Result ClientEpSessionImpl::GetXferReport(sf::Out<u32> out_count, const sf::OutAutoSelectBuffer &out_reports, u32 count) {
        const u32 max_count = std::min<u32>(count, out_reports.GetSize() / sizeof(::UsbHsXferReport));
        u32 written = 0;
        R_TRY(GetVirtualAdapter().GetXferReport(m_generation, m_index, std::addressof(written), out_reports.GetPointer(), max_count));
        out_count.SetValue(written);
        R_SUCCEED();
    }

    Result ClientEpSessionImpl::BatchBufferAsync(sf::Out<u32> out_xfer_id, const sf::InAutoSelectBuffer &urb_sizes, BatchBufferIn in) {
        /* Copy the size list: the IPC buffer is not guaranteed to be aligned. */
        u32 sizes[0x10];
        const u32 count = std::min<u32>(in.urb_count, util::size(sizes));
        R_UNLESS(urb_sizes.GetSize() >= count * sizeof(u32), usb::ResultInvalidParameter());
        std::memcpy(sizes, urb_sizes.GetPointer(), count * sizeof(u32));

        u32 xfer_id = 0;
        R_TRY(GetVirtualAdapter().PostBuffer(m_generation, m_index, std::addressof(xfer_id), in.buffer, sizes, count, in.id));
        out_xfer_id.SetValue(xfer_id);
        R_SUCCEED();
    }

    Result ClientEpSessionImpl::CreateSmmuSpace(CreateSmmuSpaceIn in) {
        R_RETURN(GetVirtualAdapter().CreateSmmuSpace(m_generation, m_index, in.buffer, in.size));
    }

    Result ClientEpSessionImpl::ShareReportRing(sf::CopyHandle &&tmem, u32 size) {
        /* The adapter takes ownership of the handle. */
        const os::NativeHandle handle = tmem.GetOsHandle();
        tmem.Detach();
        R_RETURN(GetVirtualAdapter().ShareReportRing(m_generation, m_index, handle, size));
    }

}
