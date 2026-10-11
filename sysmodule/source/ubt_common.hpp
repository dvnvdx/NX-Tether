/*
 * usb-tether: USB tethering bridge for Horizon OS (Atmosphere sysmodule).
 * Licensed under GPLv2 (same as Atmosphere-libs, which this links against).
 */
#pragma once
#include <switch.h>
#include <stratosphere.hpp>

namespace ams::ubt {

    constexpr inline u32 Version = 0x000100; /* 0.1.0 */
    #define UBT_VERSION_STRING "0.1.0"

    /* Operating modes (config.ini "mode="). */
    enum Mode : u8 {
        Mode_Off       = 0, /* Module idles. No interception, no phone link. */
        Mode_LinkTest  = 1, /* Phone link only: bring up tethering and probe DHCP. eth is never intercepted. */
        Mode_Bridge    = 2, /* Full bridge: intercept eth's usb:hs session and present a virtual LAN adapter. */
    };

    enum LinkState : u8 {
        LinkState_Idle         = 0, /* Nothing tether-capable on the bus. */
        LinkState_Initializing = 1,
        LinkState_Up           = 2,
        LinkState_Failed       = 3, /* Found a candidate but could not bring it up; see log. */
    };

    enum LinkProtocol : u8 {
        LinkProtocol_None  = 0,
        LinkProtocol_Rndis = 1,
        LinkProtocol_Ncm   = 2,
        LinkProtocol_Ecm   = 3,
    };

    enum MemoryAccessMode : u8 {
        MemoryAccessMode_Unknown = 0,
        MemoryAccessMode_Map     = 1, /* svcMapProcessMemory. */
        MemoryAccessMode_Debug   = 2, /* svcRead/WriteDebugProcessMemory. */
        MemoryAccessMode_Failed  = 3,
    };

    /* Wire format of nn::usb::hs::DeviceFilter. */
    struct DeviceFilter {
        u16 flags;
        u16 id_vendor;
        u16 id_product;
        u16 bcd_device_min;
        u16 bcd_device_max;
        u8  device_class;
        u8  device_subclass;
        u8  device_protocol;
        u8  interface_class;
        u8  interface_subclass;
        u8  interface_protocol;
    };
    static_assert(sizeof(DeviceFilter) == 0x10 && alignof(DeviceFilter) == 2);

    enum DeviceFilterFlag : u16 {
        DeviceFilterFlag_IdVendor          = (1u << 0),
        DeviceFilterFlag_IdProduct         = (1u << 1),
        DeviceFilterFlag_BcdDeviceMin      = (1u << 2),
        DeviceFilterFlag_BcdDeviceMax      = (1u << 3),
        DeviceFilterFlag_DeviceClass       = (1u << 4),
        DeviceFilterFlag_DeviceSubClass    = (1u << 5),
        DeviceFilterFlag_DeviceProtocol    = (1u << 6),
        DeviceFilterFlag_InterfaceClass    = (1u << 7),
        DeviceFilterFlag_InterfaceSubClass = (1u << 8),
        DeviceFilterFlag_InterfaceProtocol = (1u << 9),
    };

    static_assert(sizeof(::UsbHsInterfaceInfo) == 0x1B8);
    static_assert(sizeof(::UsbHsInterface) == 0x228);
    static_assert(sizeof(::UsbHsXferReport) == 0x18);

    constexpr inline size_t EthernetFrameMax = 1514;
    constexpr inline size_t EthernetFrameMin = 60;
    constexpr inline size_t FrameSlotSize    = 1536;

    /* Status block shared with the overlay (service "usbtet", command 0). Keep layout stable. */
    struct Status {
        u32 struct_version;      /* = 1 */
        u32 module_version;
        u8  mode;
        u8  bridge_enabled;
        u8  link_state;
        u8  link_protocol;
        u16 phone_vid;
        u16 phone_pid;
        u8  mac[6];
        u8  eth_session_open;    /* eth opened usb:hs through us. */
        u8  eth_filter_count;    /* Number of device filters eth registered. */
        u8  virt_attached;       /* Virtual adapter currently presented to eth. */
        u8  virt_acquired;       /* eth acquired the virtual adapter. */
        u8  virt_rx_enabled;     /* eth enabled the receiver (RX control register). */
        u8  memory_mode;         /* MemoryAccessMode. */
        u16 emu_vid;
        u16 emu_pid;
        u32 last_error;          /* Last non-zero Result seen in the link code. */
        u32 dhcp_offer_ip;       /* Network byte order; 0 if no offer seen (link test). */
        u32 ctrl_requests;       /* Control requests eth sent to the virtual adapter. */
        u32 unknown_requests;    /* ...of which we did not recognize. */
        u64 rx_packets;          /* phone -> Switch */
        u64 rx_bytes;
        u64 tx_packets;          /* Switch -> phone */
        u64 tx_bytes;
        u64 rx_dropped;
        u64 tx_dropped;
        char message[64];        /* Human readable one-liner. */
    };
    static_assert(sizeof(Status) == 0xA0);
    static_assert(std::is_trivially_copyable<Status>::value);

    inline u16 ReadLe16(const u8 *p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
    inline u32 ReadLe32(const u8 *p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }
    inline void WriteLe16(u8 *p, u16 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); }
    inline void WriteLe32(u8 *p, u32 v) { p[0] = static_cast<u8>(v); p[1] = static_cast<u8>(v >> 8); p[2] = static_cast<u8>(v >> 16); p[3] = static_cast<u8>(v >> 24); }
    inline u16 ReadBe16(const u8 *p) { return static_cast<u16>((p[0] << 8) | p[1]); }
    inline void WriteBe16(u8 *p, u16 v) { p[0] = static_cast<u8>(v >> 8); p[1] = static_cast<u8>(v); }

}
