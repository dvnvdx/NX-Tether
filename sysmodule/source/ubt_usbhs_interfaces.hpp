#pragma once
#include "ubt_common.hpp"

/* Wire-compatible definitions of the nn::usb::hs IPC interfaces. Layouts follow libnx's      */
/* usbhs.c, which is what homebrew uses against the real service. Every command takes its raw */
/* input as a single struct so that the layout is exactly what the client sends.              */

namespace ams::ubt {

    struct CreateInterfaceAvailableEventIn {
        u8 index;
        u8 pad;
        DeviceFilter filter;
    };
    static_assert(sizeof(CreateInterfaceAvailableEventIn) == 0x12);

    struct CtrlXferIn {
        u8  bm_request_type;
        u8  b_request;
        u16 w_value;
        u16 w_index;
        u16 w_length;
        u64 buffer;
    };
    static_assert(sizeof(CtrlXferIn) == 0x10);

    struct OpenUsbEpIn {
        u16 max_urb_count;
        u16 pad;
        u32 ep_type;       /* (bmAttributes & 3) + 1 */
        u32 ep_number;
        u32 ep_direction;  /* 1 = OUT (host to device), 2 = IN */
        u32 max_xfer_size;
    };
    static_assert(sizeof(OpenUsbEpIn) == 0x14);

    struct EndpointDescriptor {
        u8 data[7];
    };
    static_assert(sizeof(EndpointDescriptor) == 7 && sizeof(EndpointDescriptor) == sizeof(::usb_endpoint_descriptor));

    struct PostBufferIn {
        u32 size;
        u32 pad;
        u64 buffer;
        u64 id;
    };
    static_assert(sizeof(PostBufferIn) == 0x18);

    struct BatchBufferIn {
        u32 urb_count;
        u32 unk1;
        u32 unk2;
        u32 pad;
        u64 buffer;
        u64 id;
    };
    static_assert(sizeof(BatchBufferIn) == 0x20);

    struct CreateSmmuSpaceIn {
        u32 size;
        u32 pad;
        u64 buffer;
    };
    static_assert(sizeof(CreateSmmuSpaceIn) == 0x10);

}

#define AMS_UBT_CLIENT_EP_SESSION_INTERFACE_INFO(C, H)                                                                                                                                            \
    AMS_SF_METHOD_INFO(C, H, 0, Result, ReOpen,             (),                                                                                              ())                                  \
    AMS_SF_METHOD_INFO(C, H, 1, Result, Close,              (),                                                                                              ())                                  \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetCompletionEvent, (sf::OutCopyHandle out_event),                                                                   (out_event))                         \
    AMS_SF_METHOD_INFO(C, H, 3, Result, PopulateRing,       (),                                                                                              ())                                  \
    AMS_SF_METHOD_INFO(C, H, 4, Result, PostBufferAsync,    (sf::Out<u32> out_xfer_id, ams::ubt::PostBufferIn in),                                           (out_xfer_id, in))                   \
    AMS_SF_METHOD_INFO(C, H, 5, Result, GetXferReport,      (sf::Out<u32> out_count, const sf::OutAutoSelectBuffer &out_reports, u32 count),                 (out_count, out_reports, count))     \
    AMS_SF_METHOD_INFO(C, H, 6, Result, BatchBufferAsync,   (sf::Out<u32> out_xfer_id, const sf::InAutoSelectBuffer &urb_sizes, ams::ubt::BatchBufferIn in), (out_xfer_id, urb_sizes, in))        \
    AMS_SF_METHOD_INFO(C, H, 7, Result, CreateSmmuSpace,    (ams::ubt::CreateSmmuSpaceIn in),                                                                (in))                                \
    AMS_SF_METHOD_INFO(C, H, 8, Result, ShareReportRing,    (sf::CopyHandle &&tmem, u32 size),                                                               (std::move(tmem), size))

AMS_SF_DEFINE_INTERFACE(ams::ubt::impl, IClientEpSession, AMS_UBT_CLIENT_EP_SESSION_INTERFACE_INFO, 0x7E7B0E01)

#define AMS_UBT_CLIENT_IF_SESSION_INTERFACE_INFO(C, H)                                                                                                                                                                          \
    AMS_SF_METHOD_INFO(C, H, 0, Result, GetStateChangeEvent,        (sf::OutCopyHandle out_event),                                                                                   (out_event))                               \
    AMS_SF_METHOD_INFO(C, H, 1, Result, SetInterface,               (const sf::OutBuffer &out_profile, u8 id),                                                                       (out_profile, id))                         \
    AMS_SF_METHOD_INFO(C, H, 2, Result, GetInterface,               (const sf::OutBuffer &out_profile),                                                                              (out_profile))                             \
    AMS_SF_METHOD_INFO(C, H, 3, Result, GetAlternateInterface,      (const sf::OutBuffer &out_profile, u8 id),                                                                       (out_profile, id))                         \
    AMS_SF_METHOD_INFO(C, H, 4, Result, GetCurrentFrame,            (sf::Out<u32> out_frame),                                                                                        (out_frame))                               \
    AMS_SF_METHOD_INFO(C, H, 5, Result, CtrlXferAsync,              (ams::ubt::CtrlXferIn in),                                                                                       (in))                                      \
    AMS_SF_METHOD_INFO(C, H, 6, Result, GetCtrlXferCompletionEvent, (sf::OutCopyHandle out_event),                                                                                   (out_event))                               \
    AMS_SF_METHOD_INFO(C, H, 7, Result, GetCtrlXferReport,          (const sf::OutBuffer &out_report),                                                                               (out_report))                              \
    AMS_SF_METHOD_INFO(C, H, 8, Result, ResetDevice,                (),                                                                                                              ())                                        \
    AMS_SF_METHOD_INFO(C, H, 9, Result, OpenUsbEp,                  (sf::Out<ams::ubt::EndpointDescriptor> out_desc, sf::Out<sf::SharedPointer<ams::ubt::impl::IClientEpSession>> out, ams::ubt::OpenUsbEpIn in), (out_desc, out, in))

AMS_SF_DEFINE_INTERFACE(ams::ubt::impl, IClientIfSession, AMS_UBT_CLIENT_IF_SESSION_INTERFACE_INFO, 0x7E7B0E02)

#define AMS_UBT_CLIENT_ROOT_SESSION_MITM_INTERFACE_INFO(C, H)                                                                                                                                                                              \
    AMS_SF_METHOD_INFO(C, H, 1, Result, QueryAllInterfaces,             (sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces, ams::ubt::DeviceFilter filter),                           (out_total, out_interfaces, filter))      \
    AMS_SF_METHOD_INFO(C, H, 2, Result, QueryAvailableInterfaces,       (sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces, ams::ubt::DeviceFilter filter),                           (out_total, out_interfaces, filter))      \
    AMS_SF_METHOD_INFO(C, H, 3, Result, QueryAcquiredInterfaces,        (sf::Out<s32> out_total, const sf::OutBuffer &out_interfaces),                                                          (out_total, out_interfaces))              \
    AMS_SF_METHOD_INFO(C, H, 4, Result, CreateInterfaceAvailableEvent,  (sf::OutCopyHandle out_event, ams::ubt::CreateInterfaceAvailableEventIn in),                                            (out_event, in))                          \
    AMS_SF_METHOD_INFO(C, H, 5, Result, DestroyInterfaceAvailableEvent, (u8 index),                                                                                                             (index))                                  \
    AMS_SF_METHOD_INFO(C, H, 6, Result, GetInterfaceStateChangeEvent,   (sf::OutCopyHandle out_event),                                                                                          (out_event))                              \
    AMS_SF_METHOD_INFO(C, H, 7, Result, AcquireUsbIf,                   (sf::Out<sf::SharedPointer<ams::ubt::impl::IClientIfSession>> out, const sf::OutBuffer &out_buffer0, const sf::OutBuffer &out_buffer1, s32 id), (out, out_buffer0, out_buffer1, id))

AMS_SF_DEFINE_MITM_INTERFACE(ams::ubt::impl, IClientRootSessionMitm, AMS_UBT_CLIENT_ROOT_SESSION_MITM_INTERFACE_INFO, 0x7E7B0E03)
