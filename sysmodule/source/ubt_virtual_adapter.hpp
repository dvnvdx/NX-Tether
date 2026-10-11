#pragma once
#include "ubt_common.hpp"
#include "ubt_usbhs_interfaces.hpp"
#include "ubt_asix_model.hpp"

namespace ams::ubt {

    /* A software ASIX AX88772 USB LAN adapter. It is presented to the eth system module through */
    /* the intercepted usb:hs session; ethernet frames are exchanged with the phone link.        */
    class VirtualAdapter {
        public:
            enum EndpointIndex : int {
                EndpointIndex_Interrupt = 0, /* EP1 IN  */
                EndpointIndex_BulkIn    = 1, /* EP2 IN  */
                EndpointIndex_BulkOut   = 2, /* EP3 OUT */
                EndpointIndex_Count     = 3,
            };

            static constexpr s32 InterfaceIdBase = 0x7E7B0000;
        private:
            static constexpr size_t MaxPendingUrbs  = 96;
            static constexpr size_t ReportQueueSize = 0x21 * 0x10;
            static constexpr size_t BacklogSlots    = 96;
            static constexpr size_t StagingSize     = 0x10000;
            static constexpr u32 MaxLoggedCtrlRequests = 400;

            struct PendingUrb {
                u32 xfer_id;
                u32 size;
                u64 id;
                u64 address;
                s64 posted_us;
            };

            /* Recent transfer activity, kept in memory and written to the log when the client */
            /* lets go of the adapter (which is also what happens when the client crashes).    */
            static constexpr size_t TraceSize = 1024;
            struct TraceEntry {
                u32 time_us;
                char kind;
                u8 ep;
                u16 a;
                u32 b;
                u32 c;
                u32 d;
                u16 ring_read;   /* The endpoint's report ring as the client left it. */
                u16 ring_write;
            };

            struct Endpoint {
                bool open;
                bool event_created;
                bool ring_mapped;
                u16 max_urb_count;
                u32 max_xfer_size;
                u32 next_xfer_id;
                os::SystemEventType completion_event;
                os::TransferMemoryType ring_tmem;
                ::UsbHsRingHeader *ring;
                u64 ring_capacity;
                u64 smmu_address;
                u32 smmu_size;
                u32 report_head;
                u32 report_count;
                u32 pending_head;
                u32 pending_count;
                u64 seen_read_index;   /* Ring read position at the last check... */
                s64 seen_read_us;      /* ...and when it was first seen there. */
                ::UsbHsXferReport reports[ReportQueueSize];
                PendingUrb pending[MaxPendingUrbs];
            };
        private:
            os::SdkMutex m_lock{};

            /* Presence. */
            bool m_attached = false;
            bool m_acquired = false;
            u32  m_generation = 0;
            s32  m_interface_id = InterfaceIdBase;
            u16  m_vid = 0x0B95;
            u16  m_pid = 0x7720;
            u8   m_mac[6] = {};
            ::UsbHsInterface m_query = {};

            /* Interface session. */
            bool m_if_events_created = false;
            os::SystemEventType m_if_state_event;
            os::SystemEventType m_ctrl_event;
            ::UsbHsXferReport m_ctrl_report = {};
            u32 m_ctrl_xfer_id = 0;
            Endpoint m_endpoints[EndpointIndex_Count] = {};

            /* ASIX register file and framing. */
            ::ubtcore::asix::Model m_model{};

            /* Frames from the phone waiting for the client to post a bulk IN transfer. */
            u32 m_backlog_head = 0;
            u32 m_backlog_count = 0;
            u16 m_backlog_length[BacklogSlots] = {};
            u8  m_backlog[BacklogSlots][FrameSlotSize];

            /* Scratch space for assembling/parsing a transfer. Only used under m_lock. */
            u8 m_staging[StagingSize];

            /* Statistics. */
            u32 m_ctrl_requests = 0;
            u32 m_unknown_requests = 0;
            u64 m_rx_dropped = 0;
            u32 m_logged_tx_parse_errors = 0;
            u32 m_logged_data_transfers = 0;
            u32 m_logged_ctrl_requests = 0;
            u32 m_tick_counter = 0;
            u32 m_logged_kicks = 0;
            u32 m_trace_next = 0;
            u32 m_trace_total = 0;
            TraceEntry m_trace[TraceSize] = {};
        public:
            /* Called by the completion thread. Returns how long to wait before calling again */
            /* (microseconds), or 0 when there is nothing left to do.                         */
            s64 PumpBulkIn();

            /* Note in the log when the client has left completion reports unread for a while. */
            void KickStalledReports();

            /* Phone link side. */
            void Attach(const u8 *mac, const ::UsbHsInterface *phone_interface);
            void Detach();
            void DeliverFrame(const u8 *frame, size_t length);
            void Tick();

            /* Root session side. */
            bool IsAttached();
            bool IsAcquired();
            s32  GetInterfaceId();
            bool MatchesFilter(const DeviceFilter &filter);
            bool GetQueryOutput(::UsbHsInterface *out);
            Result Acquire(u32 *out_generation, void *out_buffer0, size_t size0, void *out_buffer1, size_t size1);

            /* Interface session side. */
            void ReleaseInterface(u32 generation);
            Result GetStateChangeEvent(u32 generation, os::NativeHandle *out);
            Result GetCtrlXferCompletionEvent(u32 generation, os::NativeHandle *out);
            Result GetProfile(u32 generation, void *out, size_t size, u8 alt_setting);
            Result CtrlXferAsync(u32 generation, const CtrlXferIn &in);
            Result GetCtrlXferReport(u32 generation, void *out, size_t size);
            Result ResetDevice(u32 generation);
            Result OpenEndpoint(u32 generation, int *out_index, EndpointDescriptor *out_desc, const OpenUsbEpIn &in);

            /* Endpoint session side. */
            void CloseEndpoint(u32 generation, int index);
            Result CancelEndpoint(u32 generation, int index);
            Result GetCompletionEvent(u32 generation, int index, os::NativeHandle *out);
            Result PostBuffer(u32 generation, int index, u32 *out_xfer_id, u64 address, const u32 *urb_sizes, u32 urb_count, u64 id);
            Result GetXferReport(u32 generation, int index, u32 *out_count, void *out, u32 max_count);
            Result CreateSmmuSpace(u32 generation, int index, u64 address, u32 size);
            Result ShareReportRing(u32 generation, int index, os::NativeHandle tmem, u32 size);

            /* Status. */
            void FillStatus(Status *status);
        private:
            bool IsLiveLocked(u32 generation) const { return m_attached && m_acquired && generation == m_generation; }
            void ResetRegistersLocked();
            void BuildQueryOutputLocked(const ::UsbHsInterface *phone_interface);
            void ChooseProductLocked();

            Result HandleControlLocked(const CtrlXferIn &in, u32 *out_transferred);
            Result HandleStandardRequestLocked(const CtrlXferIn &in, u8 *data, u32 *out_size);
            Result HandleVendorRequestLocked(const CtrlXferIn &in, u8 *data, u32 *out_size);

            void CompleteLocked(Endpoint &ep, u32 xfer_id, u64 id, Result result, u32 requested, u32 transferred);
            void FlushPendingLocked(Endpoint &ep, Result result);
            void CloseEndpointLocked(Endpoint &ep);
            void ProcessBulkOutLocked(u64 address, u32 size);
            s64  FillBulkInLocked();
            void TraceLocked(char kind, u8 ep, u16 a, u32 b, u32 c, u32 d);
            void DumpTraceLocked();
    };

    VirtualAdapter &GetVirtualAdapter();
    void LaunchVirtualAdapterThread();

    /* IPC objects handed to the client. */
    class ClientEpSessionImpl {
        private:
            u32 m_generation;
            int m_index;
            sf::cmif::DomainObjectId m_object_id;
        public:
            ClientEpSessionImpl(u32 generation, int index, sf::cmif::DomainObjectId object_id) : m_generation(generation), m_index(index), m_object_id(object_id) { /* ... */ }
            ~ClientEpSessionImpl();
        public:
            Result ReOpen();
            Result Close();
            Result GetCompletionEvent(sf::OutCopyHandle out_event);
            Result PopulateRing();
            Result PostBufferAsync(sf::Out<u32> out_xfer_id, PostBufferIn in);
            Result GetXferReport(sf::Out<u32> out_count, const sf::OutAutoSelectBuffer &out_reports, u32 count);
            Result BatchBufferAsync(sf::Out<u32> out_xfer_id, const sf::InAutoSelectBuffer &urb_sizes, BatchBufferIn in);
            Result CreateSmmuSpace(CreateSmmuSpaceIn in);
            Result ShareReportRing(sf::CopyHandle &&tmem, u32 size);
    };
    static_assert(impl::IsIClientEpSession<ClientEpSessionImpl>);

    class ClientIfSessionImpl {
        private:
            u32 m_generation;
            sf::cmif::DomainObjectId m_object_id;
        public:
            ClientIfSessionImpl(u32 generation, sf::cmif::DomainObjectId object_id) : m_generation(generation), m_object_id(object_id) { /* ... */ }
            ~ClientIfSessionImpl();
        public:
            Result GetStateChangeEvent(sf::OutCopyHandle out_event);
            Result SetInterface(const sf::OutBuffer &out_profile, u8 id);
            Result GetInterface(const sf::OutBuffer &out_profile);
            Result GetAlternateInterface(const sf::OutBuffer &out_profile, u8 id);
            Result GetCurrentFrame(sf::Out<u32> out_frame);
            Result CtrlXferAsync(CtrlXferIn in);
            Result GetCtrlXferCompletionEvent(sf::OutCopyHandle out_event);
            Result GetCtrlXferReport(const sf::OutBuffer &out_report);
            Result ResetDevice();
            Result OpenUsbEp(sf::Out<EndpointDescriptor> out_desc, sf::Out<sf::SharedPointer<impl::IClientEpSession>> out, OpenUsbEpIn in);
    };
    static_assert(impl::IsIClientIfSession<ClientIfSessionImpl>);

    /* Domain object ids for our own objects inside the intercepted session. They are taken from */
    /* the top of the table so that they can never collide with ids the real service hands out.  */
    sf::cmif::DomainObjectId AllocateVirtualObjectId();
    void FreeVirtualObjectId(sf::cmif::DomainObjectId id);

}
