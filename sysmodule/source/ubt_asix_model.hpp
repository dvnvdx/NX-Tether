/*
 * Register-level model of an ASIX AX88772 family USB LAN adapter, and its bulk framing.
 *
 * This file has no dependency on the Switch SDK so that it can be compiled and tested on a
 * PC (see tests/). Semantics follow the ASIX datasheets as reflected in the FreeBSD (if_axe)
 * and Linux (asix) drivers.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace ubtcore::asix {

    constexpr inline std::uint16_t VendorId = 0x0B95;

    /* Product ids this model can imitate, in order of preference. */
    constexpr inline std::uint16_t SupportedProducts[] = { 0x7720 /* AX88772 */, 0x772A /* AX88772A */, 0x772B /* AX88772B */, 0x7E2B /* AX88772B */ };

    constexpr inline bool IsSupportedProduct(std::uint16_t pid) {
        for (const std::uint16_t supported : SupportedProducts) {
            if (supported == pid) {
                return true;
            }
        }
        return false;
    }

    constexpr inline bool IsChecksumCapableProduct(std::uint16_t pid) {
        return pid == 0x772B || pid == 0x7E2B;
    }

    constexpr inline std::uint16_t EmbeddedPhyAddress = 0x10;

    constexpr inline std::size_t FrameMin = 60;
    constexpr inline std::size_t FrameMax = 1514;

    /* Vendor requests (bRequest). */
    enum Request : std::uint8_t {
        Request_SetSwMii           = 0x06,
        Request_ReadMiiReg         = 0x07,
        Request_WriteMiiReg        = 0x08,
        Request_ReadStatMngSts     = 0x09,
        Request_SetHwMii           = 0x0A,
        Request_ReadEeprom         = 0x0B,
        Request_WriteEeprom        = 0x0C,
        Request_WriteEepromEnable  = 0x0D,
        Request_WriteEepromDisable = 0x0E,
        Request_ReadRxCtl          = 0x0F,
        Request_WriteRxCtl         = 0x10,
        Request_ReadIpg012         = 0x11,
        Request_WriteIpg0          = 0x12,
        Request_ReadNodeId         = 0x13,
        Request_WriteNodeId        = 0x14,
        Request_ReadMulticast      = 0x15,
        Request_WriteMulticast     = 0x16,
        Request_ReadNodeId172      = 0x17,
        Request_WriteNodeId172     = 0x18,
        Request_ReadPhyId          = 0x19,
        Request_ReadMedium         = 0x1A,
        Request_WriteMedium        = 0x1B,
        Request_ReadMonitorMode    = 0x1C,
        Request_WriteMonitorMode   = 0x1D,
        Request_ReadGpio           = 0x1E,
        Request_WriteGpio          = 0x1F,
        Request_SwReset            = 0x20,
        Request_ReadPhySelect      = 0x21,
        Request_WritePhySelect     = 0x22,
        Request_ReadVlanCtrl       = 0x27,
        Request_WriteVlanCtrl      = 0x28,
        Request_WriteRxCtlCfg      = 0x2A,
        Request_ReadRxCsum         = 0x2B,
        Request_WriteRxCsum        = 0x2C,
        Request_ReadTxCsum         = 0x2D,
        Request_WriteTxCsum        = 0x2E,
    };

    constexpr inline std::uint16_t RxCtlStartOperation = 0x0080;
    constexpr inline std::uint16_t RxCtlHeaderType1    = 0x0100; /* AX88772B only; burst size on older chips */
    constexpr inline std::uint16_t RxCtlIpHeaderAlign  = 0x0200; /* AX88772B only; burst size on older chips */

    enum MiiRegister : std::uint16_t {
        MiiRegister_Bmcr   = 0,
        MiiRegister_Bmsr   = 1,
        MiiRegister_PhyId1 = 2,
        MiiRegister_PhyId2 = 3,
        MiiRegister_Anar   = 4,
        MiiRegister_Anlpar = 5,
        MiiRegister_Aner   = 6,
    };

    /* What a vendor request did, for the caller's log. */
    enum Event : int {
        Event_None           = 0,
        Event_RxCtlChanged   = 1,
        Event_MediumChanged  = 2,
        Event_NodeIdSet      = 3,
        Event_UnknownRequest = 4,
    };

    struct RequestResult {
        std::uint32_t out_size;   /* Bytes produced for device-to-host requests. */
        Event event;
        std::uint16_t old_value;  /* Previous register value for the *Changed events. */
    };

    inline std::uint16_t Le16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
    inline void PutLe16(std::uint8_t *p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
    inline void PutLe32(std::uint8_t *p, std::uint32_t v) { PutLe16(p, static_cast<std::uint16_t>(v)); PutLe16(p + 2, static_cast<std::uint16_t>(v >> 16)); }

    class Model {
        public:
            std::uint16_t pid = 0x7720;
            std::uint8_t  mac[6] = {};
            std::uint8_t  node_id[6] = {};
            bool          sw_mii = false;
            std::uint16_t rx_ctl = 0;
            std::uint16_t medium = 0;
            std::uint8_t  ipg[3] = { 0x15, 0x0C, 0x12 };
            std::uint8_t  multicast[8] = {};
            std::uint8_t  gpio = 0;
            std::uint8_t  monitor = 0;
            std::uint8_t  sw_reset = 0;
            std::uint8_t  phy_select = 1;
            std::uint16_t rx_csum = 0;
            std::uint16_t tx_csum = 0;
            std::uint32_t vlan_ctrl = 0;
            std::uint16_t phy_bmcr = 0x3100;
            std::uint16_t phy_anar = 0x01E1;
        public:
            void Reset(std::uint16_t product, const std::uint8_t *mac_address) {
                pid = product;
                std::memcpy(mac, mac_address, sizeof(mac));
                std::memcpy(node_id, mac_address, sizeof(node_id));
                sw_mii = false;
                rx_ctl = 0;
                medium = 0;
                ipg[0] = 0x15; ipg[1] = 0x0C; ipg[2] = 0x12;
                std::memset(multicast, 0, sizeof(multicast));
                gpio = 0;
                monitor = 0;
                sw_reset = 0;
                phy_select = 1;
                rx_csum = 0;
                tx_csum = 0;
                vlan_ctrl = 0;
                phy_bmcr = 0x3100;
                phy_anar = 0x01E1;
            }

            bool IsRxEnabled() const {
                return (rx_ctl & RxCtlStartOperation) != 0;
            }

            /* Largest bulk IN transfer the chip produces. On the AX88772/772A the driver picks */
            /* it through RX control bits 8-9 (2048 << n); the AX88772B uses those bits for the */
            /* header format and bursts 2048 bytes. Drivers may size their parsing for this.    */
            std::size_t MaxBurstSize() const {
                if (IsChecksumCapableProduct(pid)) {
                    return 2048;
                }
                return static_cast<std::size_t>(2048) << ((rx_ctl >> 8) & 3);
            }

            bool UsesChecksumHeader() const {
                return IsChecksumCapableProduct(pid) && (rx_ctl & RxCtlHeaderType1) != 0;
            }

            std::uint16_t ReadPhy(std::uint16_t phy, std::uint16_t reg) const {
                if (phy != EmbeddedPhyAddress) {
                    return 0xFFFF;
                }
                switch (reg) {
                    case MiiRegister_Bmcr:   return phy_bmcr;
                    case MiiRegister_Bmsr:   return 0x782D; /* 10/100 half/full capable, autoneg complete, link up */
                    case MiiRegister_PhyId1: return 0x003B;
                    case MiiRegister_PhyId2: return 0x1881;
                    case MiiRegister_Anar:   return phy_anar;
                    case MiiRegister_Anlpar: return 0x45E1; /* partner: 100FD/100HD/10FD/10HD, pause, ack */
                    case MiiRegister_Aner:   return 0x0003;
                    default:                 return 0x0000;
                }
            }

            void WritePhy(std::uint16_t phy, std::uint16_t reg, std::uint16_t value) {
                if (phy != EmbeddedPhyAddress) {
                    return;
                }
                switch (reg) {
                    case MiiRegister_Bmcr:
                        /* Reset and restart-autoneg are self clearing; everything completes instantly. */
                        phy_bmcr = static_cast<std::uint16_t>(value & ~(0x8000 | 0x0200));
                        break;
                    case MiiRegister_Anar:
                        phy_anar = value;
                        break;
                    default:
                        break;
                }
            }

            std::uint16_t ReadEeprom(std::uint16_t offset) const {
                switch (offset) {
                    case 0x04: return static_cast<std::uint16_t>(mac[0] | (mac[1] << 8));
                    case 0x05: return static_cast<std::uint16_t>(mac[2] | (mac[3] << 8));
                    case 0x06: return static_cast<std::uint16_t>(mac[4] | (mac[5] << 8));
                    default:   return 0xFFFF;
                }
            }

            /* Handle a vendor request. For host-to-device requests `data` holds the payload   */
            /* (length bytes); for device-to-host requests the answer is written to `data`,    */
            /* which the caller has zero-filled for `length` bytes.                             */
            RequestResult HandleVendorRequest(std::uint8_t request, bool device_to_host, std::uint16_t value, std::uint16_t index, std::uint16_t length, std::uint8_t *data) {
                RequestResult result = { device_to_host ? length : 0u, Event_None, 0 };

                switch (request) {
                    case Request_SetSwMii:
                        sw_mii = true;
                        result.out_size = 0;
                        break;
                    case Request_SetHwMii:
                        sw_mii = false;
                        result.out_size = 0;
                        break;
                    case Request_ReadMiiReg:
                        PutLe16(data, this->ReadPhy(value, index));
                        result.out_size = 2;
                        break;
                    case Request_WriteMiiReg:
                        if (length >= 2) {
                            this->WritePhy(value, index, Le16(data));
                        }
                        break;
                    case Request_ReadStatMngSts:
                    {
                        std::uint8_t status = sw_mii ? 0x01 : 0x00;
                        if (pid == 0x772A) {
                            status |= 0x10;
                        } else if (IsChecksumCapableProduct(pid)) {
                            status |= 0x20;
                        }
                        data[0] = status;
                        result.out_size = 1;
                        break;
                    }
                    case Request_ReadEeprom:
                        PutLe16(data, this->ReadEeprom(value));
                        result.out_size = 2;
                        break;
                    case Request_WriteEeprom:
                    case Request_WriteEepromEnable:
                    case Request_WriteEepromDisable:
                    case Request_WriteRxCtlCfg:
                        break;
                    case Request_ReadRxCtl:
                        PutLe16(data, rx_ctl);
                        result.out_size = 2;
                        break;
                    case Request_WriteRxCtl:
                        if (rx_ctl != value) {
                            result.event = Event_RxCtlChanged;
                            result.old_value = rx_ctl;
                        }
                        rx_ctl = value;
                        break;
                    case Request_ReadIpg012:
                        std::memcpy(data, ipg, 3);
                        result.out_size = 3;
                        break;
                    case Request_WriteIpg0:
                        ipg[0] = static_cast<std::uint8_t>(value);
                        ipg[1] = static_cast<std::uint8_t>(value >> 8);
                        ipg[2] = static_cast<std::uint8_t>(index);
                        break;
                    case Request_ReadNodeId:
                    case Request_ReadNodeId172:
                        if (device_to_host) {
                            std::memcpy(data, node_id, 6);
                            result.out_size = 6;
                        }
                        break;
                    case Request_WriteNodeId:
                    case Request_WriteNodeId172:
                        if (!device_to_host && length >= 6) {
                            std::memcpy(node_id, data, 6);
                            result.event = Event_NodeIdSet;
                        }
                        break;
                    case Request_ReadMulticast:
                        std::memcpy(data, multicast, 8);
                        result.out_size = 8;
                        break;
                    case Request_WriteMulticast:
                        std::memcpy(multicast, data, length < 8 ? length : 8);
                        break;
                    case Request_ReadPhyId:
                        data[0] = 0xE0;               /* secondary: none */
                        data[1] = EmbeddedPhyAddress; /* primary: embedded 10/100 PHY */
                        result.out_size = 2;
                        break;
                    case Request_ReadMedium:
                        PutLe16(data, medium);
                        result.out_size = 2;
                        break;
                    case Request_WriteMedium:
                        if (medium != value) {
                            result.event = Event_MediumChanged;
                            result.old_value = medium;
                        }
                        medium = value;
                        break;
                    case Request_ReadMonitorMode:
                        data[0] = monitor;
                        result.out_size = 1;
                        break;
                    case Request_WriteMonitorMode:
                        monitor = static_cast<std::uint8_t>(value);
                        break;
                    case Request_ReadGpio:
                        data[0] = gpio;
                        result.out_size = 1;
                        break;
                    case Request_WriteGpio:
                        gpio = static_cast<std::uint8_t>(value);
                        break;
                    case Request_SwReset:
                        sw_reset = static_cast<std::uint8_t>(value);
                        break;
                    case Request_ReadPhySelect:
                        data[0] = phy_select;
                        result.out_size = 1;
                        break;
                    case Request_WritePhySelect:
                        phy_select = static_cast<std::uint8_t>(value);
                        break;
                    case Request_ReadVlanCtrl:
                        PutLe32(data, vlan_ctrl);
                        result.out_size = 4;
                        break;
                    case Request_WriteVlanCtrl:
                        vlan_ctrl = static_cast<std::uint32_t>(value) | (static_cast<std::uint32_t>(index) << 16);
                        break;
                    case Request_ReadRxCsum:
                        PutLe16(data, rx_csum);
                        result.out_size = 2;
                        break;
                    case Request_WriteRxCsum:
                        rx_csum = value;
                        break;
                    case Request_ReadTxCsum:
                        PutLe16(data, tx_csum);
                        result.out_size = 2;
                        break;
                    case Request_WriteTxCsum:
                        tx_csum = value;
                        break;
                    default:
                        /* Unknown: answer zeros / accept. */
                        result.event = Event_UnknownRequest;
                        break;
                }

                if (result.out_size > length) {
                    result.out_size = length;
                }
                return result;
            }

            /* Wrap one received ethernet frame the way the chip does on its bulk IN endpoint. */
            /* Returns the number of bytes written, or 0 if it does not fit.                   */
            std::size_t PackFrame(std::uint8_t *dst, std::size_t capacity, const std::uint8_t *frame, std::size_t length) const {
                /* A real adapter never delivers runts: the wire pads frames to 60 bytes. */
                const std::size_t frame_length = length < FrameMin ? FrameMin : length;

                if (this->UsesChecksumHeader()) {
                    /* AX88772B "type 1" header: length, inverted length, checksum status; padded to 4. */
                    const std::size_t total = (6 + frame_length + 3) & ~static_cast<std::size_t>(3);
                    if (total > capacity) {
                        return 0;
                    }
                    std::memset(dst, 0, total);
                    std::uint16_t len_field = static_cast<std::uint16_t>(frame_length & 0x7FF);
                    if ((frame[0] & 0x01) != 0) {
                        len_field |= 0x8000; /* broadcast/multicast */
                    }
                    PutLe16(dst + 0, len_field);
                    PutLe16(dst + 2, static_cast<std::uint16_t>(~frame_length & 0x7FF));
                    PutLe16(dst + 4, 0);
                    std::memcpy(dst + 6, frame, length);
                    return total;
                }

                /* Standard header: 16 bit length, 16 bit inverted length; frame padded to even. */
                const std::size_t total = 4 + frame_length + (frame_length & 1);
                if (total > capacity) {
                    return 0;
                }
                std::memset(dst, 0, total);
                PutLe16(dst + 0, static_cast<std::uint16_t>(frame_length));
                PutLe16(dst + 2, static_cast<std::uint16_t>(~frame_length));
                std::memcpy(dst + 4, frame, length);
                return total;
            }

            /* Split a bulk OUT transfer from the driver into ethernet frames. Calls            */
            /* on_frame(const uint8_t *frame, size_t length) for each. Returns the offset of a  */
            /* malformed header, or -1 if the whole transfer was understood.                    */
            template<typename F>
            static long ParseTransmit(const std::uint8_t *data, std::size_t length, F &&on_frame) {
                const auto IsValidHeader = [&](std::size_t pos) -> bool {
                    if (pos + 4 > length) {
                        return false;
                    }
                    const std::uint16_t len = Le16(data + pos);
                    const std::uint16_t inv = Le16(data + pos + 2);
                    return ((len ^ inv) & 0x7FF) == 0x7FF;
                };

                std::size_t pos = 0;
                while (pos + 4 <= length) {
                    if (!IsValidHeader(pos)) {
                        /* Frames may be padded to an even offset. */
                        if ((pos & 1) != 0 && IsValidHeader(pos + 1)) {
                            ++pos;
                            continue;
                        }
                        return static_cast<long>(pos);
                    }

                    const std::size_t frame_length = Le16(data + pos) & 0x7FF;
                    pos += 4;
                    if (frame_length == 0) {
                        /* Zero length header: padding at the end of a transfer. */
                        continue;
                    }
                    if (pos + frame_length > length) {
                        return static_cast<long>(pos - 4);
                    }
                    if (frame_length >= 14 && frame_length <= FrameMax) {
                        on_frame(data + pos, frame_length);
                    }
                    pos += frame_length;
                }
                return -1;
            }
    };

}
