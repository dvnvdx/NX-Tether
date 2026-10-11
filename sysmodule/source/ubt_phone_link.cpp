#include "ubt_phone_link.hpp"
#include "ubt_virtual_adapter.hpp"
#include "ubt_config.hpp"
#include "ubt_log.hpp"
#include "ubt_framing.hpp"
#include "ubt_guard.hpp"
#include <atomic>

namespace ams::ubt::link {

    namespace {

        /* ------------------------------------------------------------------------------- */
        /* Constants                                                                       */
        /* ------------------------------------------------------------------------------- */

        constexpr size_t PageSize       = 0x1000;
        constexpr size_t CtrlBufferSize = 0x1000;
        constexpr size_t RxBufferSize   = 0x8000;
        constexpr size_t MaxRxUrbs      = 4;
        constexpr size_t TxBufferSize   = 0x1000;
        constexpr size_t TxQueueSlots   = 128;
        constexpr size_t MaxInterfaces  = 24;

        constexpr u64 MsToNs = 1'000'000ul;
        constexpr u64 ControlTimeoutMs = 3000;

        /* CDC */
        constexpr u8 UsbClassComm         = 0x02;
        constexpr u8 UsbClassCdcData      = 0x0A;
        constexpr u8 UsbClassWireless     = 0xE0;
        constexpr u8 UsbClassMisc         = 0xEF;
        constexpr u8 CdcSubclassAcm       = 0x02;
        constexpr u8 CdcSubclassEthernet  = 0x06;
        constexpr u8 CdcSubclassNcm       = 0x0D;

        constexpr u8 CdcSendEncapsulatedCommand = 0x00;
        constexpr u8 CdcGetEncapsulatedResponse = 0x01;
        constexpr u8 CdcSetEthernetPacketFilter = 0x43;
        constexpr u8 NcmGetNtbParameters        = 0x80;
        constexpr u8 NcmSetNtbFormat            = 0x84;
        constexpr u8 NcmSetNtbInputSize         = 0x86;

        /* RNDIS */
        constexpr u32 RndisMsgInitialize    = 0x00000002;
        constexpr u32 RndisMsgQuery         = 0x00000004;
        constexpr u32 RndisMsgSet           = 0x00000005;
        constexpr u32 RndisMsgIndicate      = 0x00000007;
        constexpr u32 RndisMsgKeepalive     = 0x00000008;
        constexpr u32 RndisMsgCompletion    = 0x80000000;
        constexpr u32 RndisOidCurrentPacketFilter = 0x0001010E;
        constexpr u32 RndisOidPermanentAddress    = 0x01010101;
        constexpr u32 RndisOidCurrentAddress      = 0x01010102;
        constexpr u32 RndisPacketFilter           = 0x0000002D; /* directed | all multicast | broadcast | promiscuous (same as Linux) */

        namespace framing = ::ubtcore::framing;

        constexpr Result ResultLinkTimeout  = MAKERESULT(Module_Libnx, LibnxError_Timeout);
        constexpr Result ResultLinkBadData  = MAKERESULT(Module_Libnx, LibnxError_BadInput);
        constexpr Result ResultLinkNotFound = MAKERESULT(Module_Libnx, LibnxError_NotFound);

        /* ------------------------------------------------------------------------------- */
        /* State                                                                           */
        /* ------------------------------------------------------------------------------- */

        struct LinkContext {
            LinkProtocol protocol;
            ::UsbHsInterface ctrl_info;
            ::UsbHsInterface data_info;
            ::UsbHsClientIfSession ctrl_if;
            ::UsbHsClientIfSession data_if;
            ::UsbHsClientEpSession ep_in;
            ::UsbHsClientEpSession ep_out;
            bool ctrl_open;
            bool data_open;
            bool ep_in_open;
            bool ep_out_open;
            size_t rx_urb_count;
            u8  ctrl_interface_number;
            u16 out_max_packet;
            u8  mac[6];

            u32 rndis_request_id;
            u32 rndis_max_transfer;

            u32 ncm_out_max_size;
            u16 ncm_out_divisor;
            u16 ncm_out_remainder;
            u16 ncm_out_alignment;
            u16 ncm_sequence;
        };

        alignas(PageSize) constinit u8 g_ctrl_buffer[CtrlBufferSize];
        alignas(PageSize) constinit u8 g_rx_buffers[MaxRxUrbs][RxBufferSize];
        alignas(PageSize) constinit u8 g_tx_buffer[TxBufferSize];
        constinit ::UsbHsInterface g_interfaces[MaxInterfaces];
        constinit ::UsbHsInterface g_data_interfaces[MaxInterfaces];

        constinit LinkContext g_ctx = {};

        /* g_io_lock guards the endpoint/interface sessions against teardown while in use. */
        constinit os::SdkMutex g_io_lock;
        constinit std::atomic<bool> g_link_up = false;
        constinit std::atomic<bool> g_link_failed = false;
        constinit std::atomic<bool> g_relink_requested = false;
        constinit std::atomic<bool> g_bridge_enabled = true;

        /* Status, guarded by g_status_lock. */
        constinit os::SdkMutex g_status_lock;
        constinit LinkState g_state = LinkState_Idle;
        constinit LinkProtocol g_protocol = LinkProtocol_None;
        constinit u16 g_phone_vid = 0;
        constinit u16 g_phone_pid = 0;
        constinit u8  g_mac[6] = {};
        constinit u32 g_last_error = 0;
        constinit u32 g_dhcp_offer_ip = 0;
        constinit char g_message[64] = "starting";

        constinit std::atomic<u64> g_rx_packets = 0;
        constinit std::atomic<u64> g_rx_bytes = 0;
        constinit std::atomic<u64> g_tx_packets = 0;
        constinit std::atomic<u64> g_tx_bytes = 0;
        constinit std::atomic<u64> g_rx_dropped = 0;
        constinit std::atomic<u64> g_tx_dropped = 0;

        /* Transmit queue (Switch -> phone). */
        constinit os::SdkMutex g_tx_lock;
        constinit u32 g_tx_head = 0;
        constinit u32 g_tx_count = 0;
        constinit u16 g_tx_length[TxQueueSlots] = {};
        constinit u8  g_tx_queue[TxQueueSlots][FrameSlotSize];
        constinit u8  g_tx_frame[FrameSlotSize];
        constinit util::TypedStorage<os::Event> g_tx_event_storage = {};

        /* DHCP probe. */
        constinit bool g_probe_sent = false;
        constinit u32  g_probe_xid = 0;
        constinit s64  g_link_up_time_ms = 0;

        /* Bus logging. */
        constinit u64 g_last_bus_signature = 0;
        constinit u32 g_logged_rx_parse_errors = 0;
        constinit bool g_bus_query_failure_logged = false;

        os::Event &GetTxEvent() {
            return util::GetReference(g_tx_event_storage);
        }

        s64 NowMs() {
            return os::ConvertToTimeSpan(os::GetSystemTick()).GetMilliSeconds();
        }

        void SetState(LinkState state, const char *message) {
            std::scoped_lock lk(g_status_lock);
            g_state = state;
            util::SNPrintf(g_message, sizeof(g_message), "%s", message);
        }

        void SetLastError(Result rc) {
            std::scoped_lock lk(g_status_lock);
            g_last_error = rc.GetValue();
        }

        /* ------------------------------------------------------------------------------- */
        /* USB helpers                                                                     */
        /* ------------------------------------------------------------------------------- */

        const ::usb_endpoint_descriptor *FindBulkEndpoint(const ::UsbHsInterfaceInfo &info, bool input) {
            const ::usb_endpoint_descriptor *descs = input ? info.input_endpoint_descs : info.output_endpoint_descs;
            for (size_t i = 0; i < 15; ++i) {
                if (descs[i].bLength != 0 && (descs[i].bmAttributes & 0x03) == 0x02) {
                    return descs + i;
                }
            }
            return nullptr;
        }

        /* Control transfer on the control interface, with a timeout (libnx's helper waits forever). */
        Result ControlTransfer(LinkContext &ctx, u8 request_type, u8 request, u16 value, u16 index, u16 length, u32 *out_transferred) {
            ::UsbHsClientIfSession *s = std::addressof(ctx.ctrl_if);

            const struct {
                u8 bmRequestType;
                u8 bRequest;
                u16 wValue;
                u16 wIndex;
                u16 wLength;
                u64 buffer;
            } in = { request_type, request, value, index, length, reinterpret_cast<u64>(g_ctrl_buffer) };

            eventClear(std::addressof(s->eventCtrlXfer));
            serviceAssumeDomain(std::addressof(s->s));
            R_TRY(serviceDispatchIn(std::addressof(s->s), 5, in));

            const ::Result wait_rc = eventWait(std::addressof(s->eventCtrlXfer), ControlTimeoutMs * MsToNs);
            if (R_FAILED(wait_rc)) {
                UBT_LOG_E("link: control request 0x%02x/0x%02x timed out", request_type, request);
                R_THROW(ResultLinkTimeout);
            }
            eventClear(std::addressof(s->eventCtrlXfer));

            ::UsbHsXferReport report = {};
            serviceAssumeDomain(std::addressof(s->s));
            R_TRY(serviceDispatch(std::addressof(s->s), 7,
                .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
                .buffers = { { std::addressof(report), sizeof(report) } },
            ));
            R_TRY(report.res);

            if (out_transferred != nullptr) {
                *out_transferred = report.transferredSize;
            }
            R_SUCCEED();
        }

        /* Bulk transfer with a timeout. On timeout the transfer is left pending; the caller */
        /* must tear the link down (closing the endpoint cancels it).                        */
        Result EndpointTransfer(::UsbHsClientEpSession *ep, void *buffer, u32 size, u32 *out_transferred, s64 timeout_ms) {
            u32 xfer_id = 0;
            R_TRY(usbHsEpPostBufferAsync(ep, buffer, size, 0, std::addressof(xfer_id)));

            const s64 deadline = NowMs() + timeout_ms;
            while (true) {
                const ::Result rc = eventWait(std::addressof(ep->eventXfer), 100 * MsToNs);
                if (R_SUCCEEDED(rc)) {
                    break;
                }
                R_UNLESS(NowMs() < deadline, ResultLinkTimeout);
            }
            eventClear(std::addressof(ep->eventXfer));

            ::UsbHsXferReport report = {};
            u32 count = 0;
            R_TRY(usbHsEpGetXferReport(ep, std::addressof(report), 1, std::addressof(count)));
            R_UNLESS(count >= 1, ResultLinkBadData);
            R_TRY(report.res);

            *out_transferred = report.transferredSize;
            R_SUCCEED();
        }

        void GenerateLocalMac(u8 *mac) {
            os::GenerateRandomBytes(mac, 6);
            mac[0] = static_cast<u8>((mac[0] & 0xFC) | 0x02); /* locally administered, unicast */
        }

        int HexValue(u16 c) {
            if (c >= '0' && c <= '9') { return c - '0'; }
            if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
            if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
            return -1;
        }

        /* ------------------------------------------------------------------------------- */
        /* RNDIS                                                                           */
        /* ------------------------------------------------------------------------------- */

        Result RndisCommand(LinkContext &ctx, const u8 *message, u32 length, u8 *out_response, size_t response_capacity, u32 *out_response_length) {
            const u32 type = ReadLe32(message);
            const u32 request_id = ReadLe32(message + 8);

            std::memcpy(g_ctrl_buffer, message, length);
            R_TRY(ControlTransfer(ctx, 0x21, CdcSendEncapsulatedCommand, 0, ctx.ctrl_interface_number, static_cast<u16>(length), nullptr));

            for (int attempt = 0; attempt < 50; ++attempt) {
                os::SleepThread(TimeSpan::FromMilliSeconds(attempt == 0 ? 5 : 20));

                std::memset(g_ctrl_buffer, 0, 0x400);
                u32 transferred = 0;
                if (R_FAILED(ControlTransfer(ctx, 0xA1, CdcGetEncapsulatedResponse, 0, ctx.ctrl_interface_number, 0x400, std::addressof(transferred)))) {
                    /* The device stalls while it has nothing to say. */
                    continue;
                }
                if (transferred < 8) {
                    continue;
                }

                const u32 response_type = ReadLe32(g_ctrl_buffer);
                const u32 response_length = ReadLe32(g_ctrl_buffer + 4);

                if (response_type == (type | RndisMsgCompletion) && transferred >= 16 && ReadLe32(g_ctrl_buffer + 8) == request_id) {
                    const u32 status = ReadLe32(g_ctrl_buffer + 12);
                    if (status != 0) {
                        UBT_LOG_E("link: RNDIS message 0x%x was refused by the phone, status 0x%x", type, status);
                        R_THROW(ResultLinkBadData);
                    }
                    const u32 copy = std::min<u32>(std::min<u32>(response_length, transferred), static_cast<u32>(response_capacity));
                    std::memcpy(out_response, g_ctrl_buffer, copy);
                    *out_response_length = copy;
                    R_SUCCEED();
                }

                if (response_type == RndisMsgKeepalive && transferred >= 12) {
                    /* Answer the phone's keepalive, then keep waiting for our response. */
                    u8 reply[16];
                    WriteLe32(reply + 0, RndisMsgKeepalive | RndisMsgCompletion);
                    WriteLe32(reply + 4, sizeof(reply));
                    WriteLe32(reply + 8, ReadLe32(g_ctrl_buffer + 8));
                    WriteLe32(reply + 12, 0);
                    std::memcpy(g_ctrl_buffer, reply, sizeof(reply));
                    R_DISCARD(ControlTransfer(ctx, 0x21, CdcSendEncapsulatedCommand, 0, ctx.ctrl_interface_number, sizeof(reply), nullptr));
                } else if (response_type == RndisMsgIndicate) {
                    UBT_LOG_D("link: RNDIS status indication 0x%x", ReadLe32(g_ctrl_buffer + 8));
                } else {
                    UBT_LOG_D("link: unexpected RNDIS message 0x%x (%u bytes) while waiting for 0x%x", response_type, transferred, type | RndisMsgCompletion);
                }
            }

            R_THROW(ResultLinkTimeout);
        }

        Result RndisQuery(LinkContext &ctx, u32 oid, u8 *out, size_t capacity, u32 *out_length) {
            /* Like Linux, include (zeroed) space for the answer in the query; some devices want it. */
            constexpr u32 QueryBufferSize = 48;
            u8 message[28 + QueryBufferSize] = {};
            WriteLe32(message + 0,  RndisMsgQuery);
            WriteLe32(message + 4,  sizeof(message));
            WriteLe32(message + 8,  ++ctx.rndis_request_id);
            WriteLe32(message + 12, oid);
            WriteLe32(message + 16, QueryBufferSize); /* information buffer length */
            WriteLe32(message + 20, 20);              /* information buffer offset */
            WriteLe32(message + 24, 0);               /* device handle */

            u8 response[0x100];
            u32 response_length = 0;
            R_TRY(RndisCommand(ctx, message, sizeof(message), response, sizeof(response), std::addressof(response_length)));
            R_UNLESS(response_length >= 24, ResultLinkBadData);

            const u32 info_length = ReadLe32(response + 16);
            const u32 info_offset = ReadLe32(response + 20) + 8;
            R_UNLESS(info_offset <= response_length && info_length <= response_length - info_offset, ResultLinkBadData);

            const u32 copy = std::min<u32>(info_length, static_cast<u32>(capacity));
            std::memcpy(out, response + info_offset, copy);
            *out_length = copy;
            R_SUCCEED();
        }

        Result RndisSet(LinkContext &ctx, u32 oid, const void *data, u32 size) {
            u8 message[28 + 16] = {};
            AMS_ABORT_UNLESS(size <= 16);
            WriteLe32(message + 0,  RndisMsgSet);
            WriteLe32(message + 4,  28 + size);
            WriteLe32(message + 8,  ++ctx.rndis_request_id);
            WriteLe32(message + 12, oid);
            WriteLe32(message + 16, size);
            WriteLe32(message + 20, 20);
            WriteLe32(message + 24, 0);
            std::memcpy(message + 28, data, size);

            u8 response[0x40];
            u32 response_length = 0;
            R_RETURN(RndisCommand(ctx, message, 28 + size, response, sizeof(response), std::addressof(response_length)));
        }

        Result RndisInitialize(LinkContext &ctx) {
            /* INITIALIZE */
            {
                u8 message[24] = {};
                WriteLe32(message + 0,  RndisMsgInitialize);
                WriteLe32(message + 4,  sizeof(message));
                WriteLe32(message + 8,  ++ctx.rndis_request_id);
                WriteLe32(message + 12, 1); /* major */
                WriteLe32(message + 16, 0); /* minor */
                WriteLe32(message + 20, RxBufferSize);

                u8 response[0x40];
                u32 response_length = 0;
                R_TRY(RndisCommand(ctx, message, sizeof(message), response, sizeof(response), std::addressof(response_length)));
                R_UNLESS(response_length >= 44, ResultLinkBadData);

                ctx.rndis_max_transfer = ReadLe32(response + 36);
                UBT_LOG_I("link: RNDIS initialized: version %u.%u, medium %u, max packets/transfer %u, max transfer %u, alignment 2^%u",
                          ReadLe32(response + 16), ReadLe32(response + 20), ReadLe32(response + 28), ReadLe32(response + 32), ctx.rndis_max_transfer, ReadLe32(response + 40));
            }

            /* MAC address the phone assigned to our end of the link. */
            {
                u32 length = 0;
                Result rc = RndisQuery(ctx, RndisOidPermanentAddress, ctx.mac, sizeof(ctx.mac), std::addressof(length));
                if (R_FAILED(rc) || length != 6) {
                    rc = RndisQuery(ctx, RndisOidCurrentAddress, ctx.mac, sizeof(ctx.mac), std::addressof(length));
                }
                if (R_FAILED(rc) || length != 6) {
                    UBT_LOG_I("link: phone did not report a MAC address (0x%x); generating one", rc.GetValue());
                    GenerateLocalMac(ctx.mac);
                }
            }

            /* Start receiving. */
            {
                u8 filter[4];
                WriteLe32(filter, RndisPacketFilter);
                R_TRY(RndisSet(ctx, RndisOidCurrentPacketFilter, filter, sizeof(filter)));
            }

            R_SUCCEED();
        }

        /* ------------------------------------------------------------------------------- */
        /* CDC NCM / ECM                                                                   */
        /* ------------------------------------------------------------------------------- */

        Result ReadCdcMacAddress(LinkContext &ctx) {
            /* The MAC lives in a string descriptor named by the CDC ethernet functional descriptor. */
            u32 transferred = 0;
            std::memset(g_ctrl_buffer, 0, 0x400);
            R_TRY(ControlTransfer(ctx, 0x80, 0x06, 0x0200, 0, 0x400, std::addressof(transferred)));

            u8 mac_string_index = 0;
            int current_interface = -1;
            for (size_t pos = 0; pos + 2 <= transferred; ) {
                const u8 length = g_ctrl_buffer[pos];
                const u8 type = g_ctrl_buffer[pos + 1];
                if (length < 2 || pos + length > transferred) {
                    break;
                }
                if (type == 0x04 && length >= 9) {
                    current_interface = g_ctrl_buffer[pos + 2];
                } else if (type == 0x24 && length >= 13 && g_ctrl_buffer[pos + 2] == 0x0F && current_interface == ctx.ctrl_interface_number) {
                    mac_string_index = g_ctrl_buffer[pos + 3];
                }
                pos += length;
            }
            R_UNLESS(mac_string_index != 0, ResultLinkNotFound);

            std::memset(g_ctrl_buffer, 0, 0x100);
            R_TRY(ControlTransfer(ctx, 0x80, 0x06, static_cast<u16>(0x0300 | mac_string_index), 0x0409, 0xFF, std::addressof(transferred)));
            R_UNLESS(transferred >= 2 + 24 && g_ctrl_buffer[1] == 0x03, ResultLinkBadData);

            for (int i = 0; i < 6; ++i) {
                const int hi = HexValue(ReadLe16(g_ctrl_buffer + 2 + 4 * i));
                const int lo = HexValue(ReadLe16(g_ctrl_buffer + 4 + 4 * i));
                R_UNLESS(hi >= 0 && lo >= 0, ResultLinkBadData);
                ctx.mac[i] = static_cast<u8>((hi << 4) | lo);
            }
            R_SUCCEED();
        }

        Result CdcInitialize(LinkContext &ctx) {
            if (const Result rc = ReadCdcMacAddress(ctx); R_FAILED(rc)) {
                UBT_LOG_I("link: could not read the CDC MAC address (0x%x); generating one", rc.GetValue());
                GenerateLocalMac(ctx.mac);
            }

            if (ctx.protocol == LinkProtocol_Ncm) {
                u32 transferred = 0;
                std::memset(g_ctrl_buffer, 0, 0x40);
                R_TRY(ControlTransfer(ctx, 0xA1, NcmGetNtbParameters, 0, ctx.ctrl_interface_number, 28, std::addressof(transferred)));
                R_UNLESS(transferred >= 28, ResultLinkBadData);

                const u16 formats        = ReadLe16(g_ctrl_buffer + 2);
                const u32 in_max_size    = ReadLe32(g_ctrl_buffer + 4);
                ctx.ncm_out_max_size     = ReadLe32(g_ctrl_buffer + 16);
                ctx.ncm_out_divisor      = ReadLe16(g_ctrl_buffer + 20);
                ctx.ncm_out_remainder    = ReadLe16(g_ctrl_buffer + 22);
                ctx.ncm_out_alignment    = ReadLe16(g_ctrl_buffer + 24);
                UBT_LOG_I("link: NCM parameters: formats=0x%x in_max=%u out_max=%u out_divisor=%u out_remainder=%u out_alignment=%u",
                          formats, in_max_size, ctx.ncm_out_max_size, ctx.ncm_out_divisor, ctx.ncm_out_remainder, ctx.ncm_out_alignment);

                if (ctx.ncm_out_divisor == 0)   { ctx.ncm_out_divisor = 1; }
                if (ctx.ncm_out_alignment < 4 || !util::IsPowerOfTwo(ctx.ncm_out_alignment)) { ctx.ncm_out_alignment = 4; }
                ctx.ncm_out_remainder %= ctx.ncm_out_divisor;

                /* Use 16 bit NTBs; make sure the phone never sends more than our buffer holds. */
                if ((formats & 0x02) != 0) {
                    R_DISCARD(ControlTransfer(ctx, 0x21, NcmSetNtbFormat, 0, ctx.ctrl_interface_number, 0, nullptr));
                }
                if (in_max_size > RxBufferSize) {
                    WriteLe32(g_ctrl_buffer, RxBufferSize);
                    if (const Result rc = ControlTransfer(ctx, 0x21, NcmSetNtbInputSize, 0, ctx.ctrl_interface_number, 4, nullptr); R_FAILED(rc)) {
                        UBT_LOG_E("link: phone refused SET_NTB_INPUT_SIZE (0x%x); large transfers may be truncated", rc.GetValue());
                    }
                }
            }

            /* The data interface only has endpoints in alternate setting 1. */
            R_TRY(usbHsIfSetInterface(std::addressof(ctx.data_if), nullptr, 1));

            /* Optional: ask for directed, broadcast and multicast frames. */
            R_DISCARD(ControlTransfer(ctx, 0x21, CdcSetEthernetPacketFilter, 0x000E, ctx.ctrl_interface_number, 0, nullptr));
            R_SUCCEED();
        }

        /* ------------------------------------------------------------------------------- */
        /* Framing                                                                         */
        /* ------------------------------------------------------------------------------- */

        size_t BuildTransmitBuffer(LinkContext &ctx, const u8 *frame, size_t length) {
            const size_t max_packet = ctx.out_max_packet != 0 ? ctx.out_max_packet : 512;

            switch (ctx.protocol) {
                case LinkProtocol_Rndis:
                    return framing::BuildRndisPacket(g_tx_buffer, TxBufferSize, frame, length, max_packet);
                case LinkProtocol_Ncm:
                {
                    const framing::NcmOutParameters params = { ctx.ncm_out_divisor, ctx.ncm_out_remainder, ctx.ncm_out_alignment };
                    return framing::BuildNcmBlock(g_tx_buffer, TxBufferSize, frame, length, params, ctx.ncm_sequence++, max_packet);
                }
                case LinkProtocol_Ecm:
                {
                    /* Raw frame; one pad byte if it would end on a packet boundary (harmless */
                    /* trailing padding) so that the transfer ends with a short packet.        */
                    std::memcpy(g_tx_buffer, frame, length);
                    size_t total = length;
                    if ((total % max_packet) == 0) {
                        g_tx_buffer[total++] = 0;
                    }
                    return total;
                }
                default:
                    return 0;
            }
        }

        void SendDhcpDiscover(const u8 *mac);
        void InspectForDhcpOffer(const u8 *frame, size_t length);

        void HandleReceivedFrame(const u8 *frame, size_t length) {
            if (length < 14 || length > EthernetFrameMax) {
                g_rx_dropped.fetch_add(1);
                return;
            }
            g_rx_packets.fetch_add(1);
            g_rx_bytes.fetch_add(length);

            if (log::GetLevel() >= log::Level_Trace) {
                log::Hex(log::Level_Trace, "link: rx frame", frame, length, 48);
            }

            if (g_probe_sent) {
                InspectForDhcpOffer(frame, length);
            }

            GetVirtualAdapter().DeliverFrame(frame, length);
        }

        void ProcessReceivedTransfer(LinkContext &ctx, const u8 *data, size_t size) {
            const auto on_frame = [](const u8 *frame, size_t length) { HandleReceivedFrame(frame, length); };

            unsigned bad = 0;
            switch (ctx.protocol) {
                case LinkProtocol_Rndis:
                    bad = framing::ParseRndisTransfer(data, size, on_frame);
                    break;
                case LinkProtocol_Ncm:
                    bad = framing::ParseNcmBlock(data, size, on_frame);
                    break;
                case LinkProtocol_Ecm:
                    HandleReceivedFrame(data, size);
                    break;
                default:
                    break;
            }

            /* Hand the whole batch to the console at once, so that it can be packed together. */
            GetVirtualAdapter().FlushFrames();

            if (bad != 0) {
                g_rx_dropped.fetch_add(bad);
                if (g_logged_rx_parse_errors < 4) {
                    ++g_logged_rx_parse_errors;
                    UBT_LOG_E("link: could not parse a %zu byte transfer from the phone", size);
                    log::Hex(log::Level_Error, "link:      data", data, size, 64);
                }
            }
        }

        /* ------------------------------------------------------------------------------- */
        /* DHCP probe: proves that data flows end to end even when nothing uses the link.  */
        /* ------------------------------------------------------------------------------- */

        void SendDhcpDiscover(const u8 *mac) {
            os::GenerateRandomBytes(std::addressof(g_probe_xid), sizeof(g_probe_xid));
            g_probe_xid |= 0x100; /* never 0 or 1, which have a meaning of their own here */

            u8 frame[framing::DhcpDiscoverFrameSize];
            framing::BuildDhcpDiscover(frame, mac, g_probe_xid);

            g_probe_sent = true;
            UBT_LOG_I("link: sending a DHCP DISCOVER to check that data flows");
            SendFrame(frame, sizeof(frame));
        }

        void InspectForDhcpOffer(const u8 *frame, size_t length) {
            framing::DhcpReply reply;
            if (!framing::ParseDhcpReply(frame, length, g_probe_xid, std::addressof(reply))) {
                return;
            }

            u32 offered = 0;
            std::memcpy(std::addressof(offered), reply.your_ip, 4);
            {
                std::scoped_lock lk(g_status_lock);
                g_dhcp_offer_ip = offered;
            }
            g_probe_sent = false;
            UBT_LOG_I("link: DHCP reply received: the phone offers %u.%u.%u.%u (server %u.%u.%u.%u). Data path works.",
                      reply.your_ip[0], reply.your_ip[1], reply.your_ip[2], reply.your_ip[3],
                      reply.server_ip[0], reply.server_ip[1], reply.server_ip[2], reply.server_ip[3]);
        }

        /* ------------------------------------------------------------------------------- */
        /* Discovery                                                                       */
        /* ------------------------------------------------------------------------------- */

        LinkProtocol ClassifyControlInterface(const ::usb_interface_descriptor &desc) {
            const u8 cls = desc.bInterfaceClass, sub = desc.bInterfaceSubClass, proto = desc.bInterfaceProtocol;
            if ((cls == UsbClassWireless && sub == 0x01 && proto == 0x03) ||
                (cls == UsbClassComm && sub == CdcSubclassAcm && proto == 0xFF) ||
                (cls == UsbClassMisc && sub == 0x04 && proto == 0x01)) {
                return LinkProtocol_Rndis;
            }
            if (cls == UsbClassComm && sub == CdcSubclassNcm) {
                return LinkProtocol_Ncm;
            }
            if (cls == UsbClassComm && sub == CdcSubclassEthernet) {
                return LinkProtocol_Ecm;
            }
            return LinkProtocol_None;
        }

        const char *ProtocolName(LinkProtocol protocol) {
            switch (protocol) {
                case LinkProtocol_Rndis: return "RNDIS";
                case LinkProtocol_Ncm:   return "CDC-NCM";
                case LinkProtocol_Ecm:   return "CDC-ECM";
                default:                 return "none";
            }
        }

        s32 QueryByInterfaceClass(u8 interface_class, ::UsbHsInterface *out, size_t capacity) {
            ::UsbHsInterfaceFilter filter = {};
            filter.Flags = UsbHsInterfaceFilterFlags_bInterfaceClass;
            filter.bInterfaceClass = interface_class;

            s32 total = 0;
            if (R_FAILED(usbHsQueryAvailableInterfaces(std::addressof(filter), out, capacity * sizeof(::UsbHsInterface), std::addressof(total)))) {
                return 0;
            }
            return std::min<s32>(total, static_cast<s32>(capacity));
        }

        /* Log everything on the bus whenever the set of devices changes; this is what tells us */
        /* how the phone presents itself (or that it is not visible at all).                    */
        void LogBusIfChanged() {
            ::UsbHsInterfaceFilter filter = {};
            s32 total = 0;
            if (const Result rc = usbHsQueryAllInterfaces(std::addressof(filter), g_interfaces, sizeof(g_interfaces), std::addressof(total)); R_FAILED(rc)) {
                if (!g_bus_query_failure_logged) {
                    g_bus_query_failure_logged = true;
                    UBT_LOG_E("bus: cannot list USB interfaces: 0x%x", rc.GetValue());
                }
                return;
            }
            total = std::min<s32>(total, MaxInterfaces);

            u64 signature = 0x9E3779B97F4A7C15ul * static_cast<u64>(total + 1);
            for (s32 i = 0; i < total; ++i) {
                const auto &entry = g_interfaces[i];
                signature = (signature ^ (static_cast<u64>(entry.deviceID) << 32 | static_cast<u32>(entry.inf.ID))) * 0x100000001B3ul;
                signature ^= (static_cast<u64>(entry.inf.interface_desc.bInterfaceClass) << 16) | (static_cast<u64>(entry.inf.interface_desc.bInterfaceSubClass) << 8) | entry.inf.interface_desc.bInterfaceProtocol;
            }
            if (signature == g_last_bus_signature) {
                return;
            }
            g_last_bus_signature = signature;

            UBT_LOG_I("bus: %d USB interface(s) visible", total);
            for (s32 i = 0; i < total; ++i) {
                const auto &entry = g_interfaces[i];
                const auto &desc = entry.inf.interface_desc;
                UBT_LOG_I("bus:   device %u (bus %u) %04x:%04x  interface %u alt %u  class %02x/%02x/%02x  endpoints %u  id 0x%x",
                          entry.deviceID, entry.busID, entry.device_desc.idVendor, entry.device_desc.idProduct,
                          desc.bInterfaceNumber, desc.bAlternateSetting, desc.bInterfaceClass, desc.bInterfaceSubClass, desc.bInterfaceProtocol,
                          desc.bNumEndpoints, static_cast<unsigned>(entry.inf.ID));
            }
        }

        bool FindTetherInterfaces(LinkContext &ctx) {
            const u8 control_classes[] = { UsbClassWireless, UsbClassComm, UsbClassMisc };
            const bool prefer_ncm = config::Get().prefer_ncm;

            const s32 data_count = QueryByInterfaceClass(UsbClassCdcData, g_data_interfaces, MaxInterfaces);

            int best_score = 0;
            for (const u8 control_class : control_classes) {
                const s32 count = QueryByInterfaceClass(control_class, g_interfaces, MaxInterfaces);
                for (s32 i = 0; i < count; ++i) {
                    const auto &ctrl = g_interfaces[i];
                    const LinkProtocol protocol = ClassifyControlInterface(ctrl.inf.interface_desc);
                    if (protocol == LinkProtocol_None) {
                        continue;
                    }

                    /* Find the matching data interface on the same device, preferably the next one. */
                    const ::UsbHsInterface *data = nullptr;
                    for (s32 j = 0; j < data_count; ++j) {
                        const auto &candidate = g_data_interfaces[j];
                        if (candidate.deviceID != ctrl.deviceID || candidate.busID != ctrl.busID) {
                            continue;
                        }
                        if (data == nullptr || candidate.inf.interface_desc.bInterfaceNumber == ctrl.inf.interface_desc.bInterfaceNumber + 1) {
                            data = std::addressof(candidate);
                        }
                    }
                    if (data == nullptr) {
                        continue;
                    }

                    int score = 0;
                    switch (protocol) {
                        case LinkProtocol_Rndis: score = prefer_ncm ? 2 : 3; break;
                        case LinkProtocol_Ncm:   score = prefer_ncm ? 3 : 2; break;
                        default:                 score = 1; break;
                    }
                    if (score > best_score) {
                        best_score = score;
                        ctx.protocol = protocol;
                        ctx.ctrl_info = ctrl;
                        ctx.data_info = *data;
                    }
                }
            }

            return best_score > 0;
        }

        /* ------------------------------------------------------------------------------- */
        /* Link lifetime                                                                   */
        /* ------------------------------------------------------------------------------- */

        /* Number of receive transfers kept queued with the phone. With only one, the phone has */
        /* to wait after every transfer until the next one has been set up, which limits the    */
        /* receive rate to a few MB/s.                                                          */
        size_t GetRxUrbCount() {
            return std::clamp<size_t>(config::Get().rx_urbs, 1, MaxRxUrbs);
        }

        void TearDown(LinkContext &ctx) {
            g_link_up = false;
            GetVirtualAdapter().Detach();

            std::scoped_lock lk(g_io_lock);
            if (ctx.ep_in_open)  { usbHsEpClose(std::addressof(ctx.ep_in));  ctx.ep_in_open = false; }
            if (ctx.ep_out_open) { usbHsEpClose(std::addressof(ctx.ep_out)); ctx.ep_out_open = false; }
            if (ctx.data_open)   { usbHsIfClose(std::addressof(ctx.data_if)); ctx.data_open = false; }
            if (ctx.ctrl_open)   { usbHsIfClose(std::addressof(ctx.ctrl_if)); ctx.ctrl_open = false; }

            {
                std::scoped_lock tx(g_tx_lock);
                g_tx_head = 0;
                g_tx_count = 0;
            }
            {
                std::scoped_lock st(g_status_lock);
                g_protocol = LinkProtocol_None;
            }
        }

        Result BringUp(LinkContext &ctx) {
            const auto &ctrl_desc = ctx.ctrl_info.inf.interface_desc;
            UBT_LOG_I("link: found %s on device %04x:%04x (control interface %u, data interface %u)", ProtocolName(ctx.protocol),
                      ctx.ctrl_info.device_desc.idVendor, ctx.ctrl_info.device_desc.idProduct, ctrl_desc.bInterfaceNumber, ctx.data_info.inf.interface_desc.bInterfaceNumber);

            {
                std::scoped_lock st(g_status_lock);
                g_protocol = ctx.protocol;
                g_phone_vid = ctx.ctrl_info.device_desc.idVendor;
                g_phone_pid = ctx.ctrl_info.device_desc.idProduct;
                g_dhcp_offer_ip = 0;
            }

            ctx.ctrl_interface_number = ctrl_desc.bInterfaceNumber;
            ctx.rndis_request_id = 0;
            ctx.ncm_sequence = 0;

            R_TRY(usbHsAcquireUsbIf(std::addressof(ctx.ctrl_if), std::addressof(ctx.ctrl_info)));
            ctx.ctrl_open = true;
            R_TRY(usbHsAcquireUsbIf(std::addressof(ctx.data_if), std::addressof(ctx.data_info)));
            ctx.data_open = true;

            if (ctx.protocol == LinkProtocol_Rndis) {
                R_TRY(RndisInitialize(ctx));
            } else {
                R_TRY(CdcInitialize(ctx));
            }

            const ::usb_endpoint_descriptor *in_desc  = FindBulkEndpoint(ctx.data_if.inf.inf, true);
            const ::usb_endpoint_descriptor *out_desc = FindBulkEndpoint(ctx.data_if.inf.inf, false);
            if (in_desc == nullptr || out_desc == nullptr) {
                UBT_LOG_E("link: the data interface has no bulk endpoint pair");
                R_THROW(ResultLinkNotFound);
            }
            ctx.out_max_packet = out_desc->wMaxPacketSize;
            UBT_LOG_I("link: bulk endpoints: in 0x%02x (max packet %u), out 0x%02x (max packet %u)", in_desc->bEndpointAddress, in_desc->wMaxPacketSize, out_desc->bEndpointAddress, out_desc->wMaxPacketSize);

            ::usb_endpoint_descriptor in_copy = *in_desc;
            ::usb_endpoint_descriptor out_copy = *out_desc;
            ctx.rx_urb_count = GetRxUrbCount();
            if (const Result rc = usbHsIfOpenUsbEp(std::addressof(ctx.data_if), std::addressof(ctx.ep_in), static_cast<u16>(ctx.rx_urb_count), RxBufferSize, std::addressof(in_copy)); R_FAILED(rc)) {
                R_UNLESS(ctx.rx_urb_count > 1, rc);
                UBT_LOG_E("link: opening the receive endpoint for %zu transfers failed (0x%x); using one", ctx.rx_urb_count, rc.GetValue());
                ctx.rx_urb_count = 1;
                in_copy = *in_desc;
                R_TRY(usbHsIfOpenUsbEp(std::addressof(ctx.data_if), std::addressof(ctx.ep_in), 1, RxBufferSize, std::addressof(in_copy)));
            }
            ctx.ep_in_open = true;
            R_TRY(usbHsIfOpenUsbEp(std::addressof(ctx.data_if), std::addressof(ctx.ep_out), 1, TxBufferSize, std::addressof(out_copy)));
            ctx.ep_out_open = true;

            {
                std::scoped_lock st(g_status_lock);
                std::memcpy(g_mac, ctx.mac, sizeof(g_mac));
            }
            UBT_LOG_I("link: up. Our MAC on this link: %02x:%02x:%02x:%02x:%02x:%02x", ctx.mac[0], ctx.mac[1], ctx.mac[2], ctx.mac[3], ctx.mac[4], ctx.mac[5]);
            R_SUCCEED();
        }

        bool IsDeviceGone(LinkContext &ctx) {
            ::Event *event = usbHsGetInterfaceStateChangeEvent();
            if (R_FAILED(eventWait(event, 0))) {
                return false;
            }
            eventClear(event);

            s32 total = 0;
            if (R_FAILED(usbHsQueryAcquiredInterfaces(g_interfaces, sizeof(g_interfaces), std::addressof(total)))) {
                return false;
            }
            total = std::min<s32>(total, MaxInterfaces);
            for (s32 i = 0; i < total; ++i) {
                if (g_interfaces[i].inf.ID == usbHsIfGetID(std::addressof(ctx.ctrl_if))) {
                    return false;
                }
            }
            return true;
        }

        /* Things that have to happen regularly while the link is up. */
        void RunPeriodicTasks(LinkContext &ctx) {
            auto &adapter = GetVirtualAdapter();

            /* Present or withdraw the virtual adapter according to the mode and the overlay toggle. */
            const bool want_adapter = config::Get().mode == Mode_Bridge && g_bridge_enabled.load();
            if (want_adapter && !adapter.IsAttached()) {
                /* Showing eth a new device is the riskiest moment; guard it. */
                guard::Arm();
                adapter.Attach(ctx.mac, std::addressof(ctx.ctrl_info));
            } else if (!want_adapter && adapter.IsAttached()) {
                adapter.Detach();
            }

            /* One DHCP probe per link-up, if the system is not using the link itself. */
            if (config::Get().dhcp_probe && !g_probe_sent && g_probe_xid == 0 && NowMs() - g_link_up_time_ms > 4000) {
                if (!adapter.IsAcquired()) {
                    SendDhcpDiscover(ctx.mac);
                } else {
                    g_probe_xid = 1; /* not needed; the system took the adapter */
                }
            }
        }

        void RunLink(LinkContext &ctx) {
            g_link_failed = false;
            g_probe_sent = false;
            g_probe_xid = 0;
            g_link_up_time_ms = NowMs();
            g_link_up = true;
            SetState(LinkState_Up, "tethering link up");

            u32 consecutive_errors = 0;
            s64 last_periodic_ms = 0;
            const char *reason = "stopped";

            const auto PostReceive = [&](size_t index) -> Result {
                u32 xfer_id = 0;
                R_RETURN(usbHsEpPostBufferAsync(std::addressof(ctx.ep_in), g_rx_buffers[index], RxBufferSize, index, std::addressof(xfer_id)));
            };

            /* Queue the receive transfers. */
            size_t urb_count = ctx.rx_urb_count;
            for (size_t i = 0; i < urb_count; ++i) {
                if (const Result rc = PostReceive(i); R_FAILED(rc)) {
                    SetLastError(rc);
                    UBT_LOG_E("link: cannot queue receive transfer %zu of %zu: 0x%x", i + 1, urb_count, rc.GetValue());
                    urb_count = i;
                    break;
                }
            }
            UBT_LOG_I("link: %zu receive transfer(s) queued", urb_count);

            bool stop = (urb_count == 0);
            if (stop) {
                reason = "receive failed";
            }

            while (!stop) {
                /* Wait for data, waking up regularly for housekeeping. */
                while (true) {
                    if (R_SUCCEEDED(eventWait(std::addressof(ctx.ep_in.eventXfer), 200 * MsToNs))) {
                        break;
                    }
                    if (g_relink_requested.exchange(false)) { reason = "relink requested"; stop = true; break; }
                    if (g_link_failed.load())               { reason = "transmit failed";  stop = true; break; }
                    if (IsDeviceGone(ctx))                  { reason = "phone unplugged";  stop = true; break; }
                    RunPeriodicTasks(ctx);
                    last_periodic_ms = NowMs();
                }
                if (stop) {
                    break;
                }
                eventClear(std::addressof(ctx.ep_in.eventXfer));

                /* Reports come back in the order the transfers finished. */
                ::UsbHsXferReport reports[MaxRxUrbs] = {};
                u32 count = 0;
                if (const Result rc = usbHsEpGetXferReport(std::addressof(ctx.ep_in), reports, static_cast<u32>(urb_count), std::addressof(count)); R_FAILED(rc)) {
                    SetLastError(rc);
                    UBT_LOG_E("link: cannot read receive reports: 0x%x", rc.GetValue());
                    reason = "receive failed";
                    break;
                }

                for (u32 i = 0; i < count && !stop; ++i) {
                    const ::UsbHsXferReport &report = reports[i];
                    const size_t index = static_cast<size_t>(report.id);
                    if (index >= urb_count) {
                        UBT_LOG_E("link: receive report for unknown transfer %llu", static_cast<unsigned long long>(report.id));
                        reason = "receive failed";
                        stop = true;
                        break;
                    }

                    if (const Result rc = report.res; R_FAILED(rc)) {
                        SetLastError(rc);
                        if (++consecutive_errors <= 3) {
                            UBT_LOG_E("link: receive error 0x%x", rc.GetValue());
                        }
                        if (consecutive_errors >= 8 || IsDeviceGone(ctx)) {
                            reason = "receive errors";
                            stop = true;
                            break;
                        }
                        os::SleepThread(TimeSpan::FromMilliSeconds(20));
                    } else {
                        consecutive_errors = 0;
                        ProcessReceivedTransfer(ctx, g_rx_buffers[index], std::min<size_t>(report.transferredSize, RxBufferSize));
                    }

                    if (const Result rc = PostReceive(index); R_FAILED(rc)) {
                        SetLastError(rc);
                        UBT_LOG_E("link: cannot queue a receive transfer: 0x%x", rc.GetValue());
                        reason = "receive failed";
                        stop = true;
                    }
                }
                if (stop) {
                    break;
                }

                if (NowMs() - last_periodic_ms >= 200) {
                    if (g_relink_requested.exchange(false)) { reason = "relink requested"; break; }
                    if (g_link_failed.load())               { reason = "transmit failed";  break; }
                    RunPeriodicTasks(ctx);
                    last_periodic_ms = NowMs();
                }
            }

            UBT_LOG_I("link: down (%s). rx %llu packets, tx %llu packets", reason, static_cast<unsigned long long>(g_rx_packets.load()), static_cast<unsigned long long>(g_tx_packets.load()));
        }

        constexpr size_t LinkThreadStackSize = 0x4000;
        alignas(os::ThreadStackAlignment) constinit u8 g_link_thread_stack[LinkThreadStackSize];
        constinit os::ThreadType g_link_thread;

        void LinkThreadFunction(void *) {
            if (config::Get().mode == Mode_Off) {
                SetState(LinkState_Idle, "module is off (mode=0)");
                UBT_LOG_I("link: mode 0, staying idle");
                return;
            }

            if (const Result rc = usbHsInitialize(); R_FAILED(rc)) {
                SetLastError(rc);
                SetState(LinkState_Failed, "usb:hs unavailable");
                UBT_LOG_E("link: usbHsInitialize failed: 0x%x", rc.GetValue());
                return;
            }
            UBT_LOG_I("link: usb:hs session open, watching the bus");
            SetState(LinkState_Idle, "waiting for phone (enable USB tethering)");

            s64 retry_after_ms = 0;
            while (true) {
                /* Wake on bus changes, or once a second. */
                ::Event *state_event = usbHsGetInterfaceStateChangeEvent();
                if (R_SUCCEEDED(eventWait(state_event, 1000 * MsToNs))) {
                    eventClear(state_event);
                }
                g_relink_requested = false;

                LogBusIfChanged();

                if (NowMs() < retry_after_ms) {
                    continue;
                }

                LinkContext &ctx = g_ctx;
                ctx = {};
                if (!FindTetherInterfaces(ctx)) {
                    continue;
                }

                SetState(LinkState_Initializing, "phone found, starting link");
                const Result rc = BringUp(ctx);
                if (R_FAILED(rc)) {
                    SetLastError(rc);
                    UBT_LOG_E("link: bring-up failed: 0x%x", rc.GetValue());
                    TearDown(ctx);
                    SetState(LinkState_Failed, "link start failed, see log");
                    retry_after_ms = NowMs() + 5000;
                    continue;
                }

                RunLink(ctx);
                TearDown(ctx);
                SetState(LinkState_Idle, "waiting for phone (enable USB tethering)");
                g_last_bus_signature = 0;
                retry_after_ms = NowMs() + 1000;
            }
        }

        /* Transmit thread: drains the queue to the phone and drives the adapter's timer. */
        constexpr size_t TxThreadStackSize = 0x4000;
        alignas(os::ThreadStackAlignment) constinit u8 g_tx_thread_stack[TxThreadStackSize];
        constinit os::ThreadType g_tx_thread;

        bool DequeueTransmit(size_t *out_length) {
            std::scoped_lock lk(g_tx_lock);
            if (g_tx_count == 0) {
                return false;
            }
            *out_length = g_tx_length[g_tx_head];
            std::memcpy(g_tx_frame, g_tx_queue[g_tx_head], *out_length);
            g_tx_head = (g_tx_head + 1) % TxQueueSlots;
            --g_tx_count;
            return true;
        }

        void TxThreadFunction(void *) {
            s64 last_tick_ms = 0;
            u32 logged_errors = 0;

            while (true) {
                GetTxEvent().TimedWait(TimeSpan::FromMilliSeconds(100));

                size_t length = 0;
                while (DequeueTransmit(std::addressof(length))) {
                    std::scoped_lock lk(g_io_lock);
                    if (!g_link_up.load() || g_link_failed.load()) {
                        g_tx_dropped.fetch_add(1);
                        continue;
                    }

                    if (log::GetLevel() >= log::Level_Trace) {
                        log::Hex(log::Level_Trace, "link: tx frame", g_tx_frame, length, 48);
                    }

                    const size_t total = BuildTransmitBuffer(g_ctx, g_tx_frame, length);
                    if (total == 0) {
                        g_tx_dropped.fetch_add(1);
                        continue;
                    }

                    u32 transferred = 0;
                    const Result rc = EndpointTransfer(std::addressof(g_ctx.ep_out), g_tx_buffer, static_cast<u32>(total), std::addressof(transferred), 1000);
                    if (R_FAILED(rc)) {
                        g_tx_dropped.fetch_add(1);
                        SetLastError(rc);
                        if (logged_errors < 5) {
                            ++logged_errors;
                            UBT_LOG_E("link: transmit error 0x%x", rc.GetValue());
                        }
                        /* A timed out transfer is still pending; the link has to be rebuilt. */
                        g_link_failed = true;
                    } else {
                        g_tx_packets.fetch_add(1);
                        g_tx_bytes.fetch_add(length);
                    }
                }

                const s64 now = NowMs();
                if (now - last_tick_ms >= 100) {
                    last_tick_ms = now;
                    GetVirtualAdapter().Tick();
                    guard::Poll();
                }
            }
        }

    }

    void Launch() {
        util::ConstructAt(g_tx_event_storage, os::EventClearMode_AutoClear);
        LaunchVirtualAdapterThread();

        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_tx_thread), TxThreadFunction, nullptr, g_tx_thread_stack, sizeof(g_tx_thread_stack), 21));
        os::SetThreadNamePointer(std::addressof(g_tx_thread), "ubt::TxThread");
        os::StartThread(std::addressof(g_tx_thread));

        R_ABORT_UNLESS(os::CreateThread(std::addressof(g_link_thread), LinkThreadFunction, nullptr, g_link_thread_stack, sizeof(g_link_thread_stack), 21));
        os::SetThreadNamePointer(std::addressof(g_link_thread), "ubt::LinkThread");
        os::StartThread(std::addressof(g_link_thread));
    }

    bool SendFrame(const u8 *frame, size_t length) {
        if (length < 14 || length > EthernetFrameMax || !g_link_up.load()) {
            g_tx_dropped.fetch_add(1);
            return false;
        }

        {
            std::scoped_lock lk(g_tx_lock);
            if (g_tx_count == TxQueueSlots) {
                g_tx_dropped.fetch_add(1);
                return false;
            }
            const u32 slot = (g_tx_head + g_tx_count) % TxQueueSlots;
            std::memcpy(g_tx_queue[slot], frame, length);
            g_tx_length[slot] = static_cast<u16>(length);
            ++g_tx_count;
        }
        GetTxEvent().Signal();
        return true;
    }

    void SetBridgeEnabled(bool enabled) {
        g_bridge_enabled = enabled;
    }

    void RequestRelink() {
        g_relink_requested = true;
    }

    void FillStatus(Status *status) {
        {
            std::scoped_lock lk(g_status_lock);
            status->link_state = g_state;
            status->link_protocol = g_protocol;
            status->phone_vid = g_phone_vid;
            status->phone_pid = g_phone_pid;
            std::memcpy(status->mac, g_mac, sizeof(status->mac));
            status->last_error = g_last_error;
            status->dhcp_offer_ip = g_dhcp_offer_ip;
            std::memcpy(status->message, g_message, sizeof(status->message));
        }
        status->bridge_enabled = g_bridge_enabled.load();
        status->rx_packets = g_rx_packets.load();
        status->rx_bytes = g_rx_bytes.load();
        status->tx_packets = g_tx_packets.load();
        status->tx_bytes = g_tx_bytes.load();
        status->rx_dropped = g_rx_dropped.load();
        status->tx_dropped = g_tx_dropped.load();
    }

}
