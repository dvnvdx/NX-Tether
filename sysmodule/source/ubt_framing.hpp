/*
 * Wire formats used on the phone side of the bridge: RNDIS and CDC-NCM data framing, and
 * the DHCP probe. No dependency on the Switch SDK, so this can be tested on a PC (tests/).
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace ubtcore::framing {

    inline std::uint16_t Le16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
    inline std::uint32_t Le32(const std::uint8_t *p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
    inline void PutLe16(std::uint8_t *p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); }
    inline void PutLe32(std::uint8_t *p, std::uint32_t v) { p[0] = static_cast<std::uint8_t>(v); p[1] = static_cast<std::uint8_t>(v >> 8); p[2] = static_cast<std::uint8_t>(v >> 16); p[3] = static_cast<std::uint8_t>(v >> 24); }
    inline std::uint16_t Be16(const std::uint8_t *p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
    inline void PutBe16(std::uint8_t *p, std::uint16_t v) { p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v); }

    constexpr inline std::size_t EthernetFrameMax = 1514;

    /* ------------------------------------------------------------------------------- */
    /* RNDIS data channel                                                              */
    /* ------------------------------------------------------------------------------- */

    constexpr inline std::uint32_t RndisMsgPacket = 0x00000001;
    constexpr inline std::size_t RndisPacketHeaderSize = 44;

    /* Wrap one ethernet frame in a REMOTE_NDIS_PACKET_MSG. Returns the transfer length, or 0 */
    /* if it does not fit. If the message would end exactly on a USB packet boundary, one pad */
    /* byte is appended (as Linux does) so that the transfer ends with a short packet.         */
    inline std::size_t BuildRndisPacket(std::uint8_t *out, std::size_t capacity, const std::uint8_t *frame, std::size_t length, std::size_t max_packet) {
        std::size_t total = RndisPacketHeaderSize + length;
        if (total + 1 > capacity) {
            return 0;
        }
        std::memset(out, 0, RndisPacketHeaderSize);
        PutLe32(out + 0,  RndisMsgPacket);
        PutLe32(out + 4,  static_cast<std::uint32_t>(total));
        PutLe32(out + 8,  static_cast<std::uint32_t>(RndisPacketHeaderSize - 8)); /* data offset, counted from this field */
        PutLe32(out + 12, static_cast<std::uint32_t>(length));
        std::memcpy(out + RndisPacketHeaderSize, frame, length);
        if (max_packet != 0 && (total % max_packet) == 0) {
            out[total++] = 0;
        }
        return total;
    }

    /* Split a bulk IN transfer into ethernet frames; one transfer may carry several       */
    /* messages. Calls on_frame(const uint8_t *, size_t). Returns the number of malformed  */
    /* messages skipped.                                                                    */
    template<typename F>
    inline unsigned ParseRndisTransfer(const std::uint8_t *data, std::size_t size, F &&on_frame) {
        unsigned bad = 0;
        std::size_t pos = 0;
        while (pos + RndisPacketHeaderSize <= size) {
            const std::uint32_t type = Le32(data + pos);
            const std::uint32_t message_length = Le32(data + pos + 4);
            if (type != RndisMsgPacket || message_length < RndisPacketHeaderSize || message_length > size - pos) {
                /* Not a data message, or truncated: nothing more to recover from this transfer. */
                if (type != 0 || message_length != 0) {
                    ++bad;
                }
                break;
            }
            const std::uint64_t data_offset = static_cast<std::uint64_t>(Le32(data + pos + 8)) + 8;
            const std::uint32_t data_length = Le32(data + pos + 12);
            if (data_offset <= message_length && data_length <= message_length - data_offset) {
                on_frame(data + pos + data_offset, static_cast<std::size_t>(data_length));
            } else {
                ++bad;
            }
            pos += message_length;
        }
        return bad;
    }

    /* ------------------------------------------------------------------------------- */
    /* CDC-NCM (16 bit transfer blocks)                                                */
    /* ------------------------------------------------------------------------------- */

    constexpr inline std::uint32_t NcmNth16Signature      = 0x484D434E; /* "NCMH" */
    constexpr inline std::uint32_t NcmNdp16SignatureNoCrc = 0x304D434E; /* "NCM0" */
    constexpr inline std::uint32_t NcmNdp16SignatureCrc   = 0x314D434E; /* "NCM1" */

    struct NcmOutParameters {
        std::uint16_t divisor;    /* wNdpOutDivisor, >= 1 */
        std::uint16_t remainder;  /* wNdpOutPayloadRemainder, < divisor */
        std::uint16_t alignment;  /* wNdpOutAlignment, power of two >= 4 */
    };

    /* Build an NTB16 carrying a single datagram: NTH16, NDP16, datagram. */
    inline std::size_t BuildNcmBlock(std::uint8_t *out, std::size_t capacity, const std::uint8_t *frame, std::size_t length, const NcmOutParameters &params, std::uint16_t sequence, std::size_t max_packet) {
        const std::size_t alignment = params.alignment;
        const std::size_t ndp_offset = (12 + alignment - 1) & ~(alignment - 1);
        const std::size_t ndp_length = 16;
        std::size_t data_offset = ndp_offset + ndp_length;
        while ((data_offset % params.divisor) != params.remainder) {
            ++data_offset;
        }
        std::size_t total = data_offset + length;
        if (total + 1 > capacity || total + 1 > 0xFFFF) {
            return 0;
        }

        std::memset(out, 0, data_offset);
        PutLe32(out + 0,  NcmNth16Signature);
        PutLe16(out + 4,  12);                                       /* wHeaderLength */
        PutLe16(out + 6,  sequence);                                 /* wSequence */
        PutLe16(out + 10, static_cast<std::uint16_t>(ndp_offset));   /* wNdpIndex */
        PutLe32(out + ndp_offset + 0,  NcmNdp16SignatureNoCrc);
        PutLe16(out + ndp_offset + 4,  static_cast<std::uint16_t>(ndp_length));
        PutLe16(out + ndp_offset + 6,  0);                           /* wNextNdpIndex */
        PutLe16(out + ndp_offset + 8,  static_cast<std::uint16_t>(data_offset));
        PutLe16(out + ndp_offset + 10, static_cast<std::uint16_t>(length));
        /* The terminating zero entry is already in place. */
        std::memcpy(out + data_offset, frame, length);
        if (max_packet != 0 && (total % max_packet) == 0) {
            out[total++] = 0;
        }
        PutLe16(out + 8, static_cast<std::uint16_t>(total));         /* wBlockLength */
        return total;
    }

    template<typename F>
    inline unsigned ParseNcmBlock(const std::uint8_t *data, std::size_t size, F &&on_frame) {
        if (size < 12 || Le32(data) != NcmNth16Signature) {
            return 1;
        }

        unsigned bad = 0;
        std::size_t ndp = Le16(data + 10);
        for (int guard = 0; guard < 32 && ndp != 0; ++guard) {
            if (ndp + 8 > size) {
                ++bad;
                break;
            }
            const std::uint32_t signature = Le32(data + ndp);
            if (signature != NcmNdp16SignatureNoCrc && signature != NcmNdp16SignatureCrc) {
                ++bad;
                break;
            }
            const std::size_t ndp_length = Le16(data + ndp + 4);
            const std::size_t next = Le16(data + ndp + 6);
            if (ndp_length < 8 || ndp + ndp_length > size) {
                ++bad;
                break;
            }
            for (std::size_t entry = ndp + 8; entry + 4 <= ndp + ndp_length; entry += 4) {
                const std::size_t index = Le16(data + entry);
                std::size_t length = Le16(data + entry + 2);
                if (index == 0 || length == 0) {
                    break;
                }
                if (index + length > size) {
                    ++bad;
                    break;
                }
                if (signature == NcmNdp16SignatureCrc && length > 4) {
                    length -= 4; /* strip the CRC32 */
                }
                on_frame(data + index, length);
            }
            ndp = next;
        }
        return bad;
    }

    /* ------------------------------------------------------------------------------- */
    /* DHCP probe                                                                      */
    /* ------------------------------------------------------------------------------- */

    inline std::uint16_t InternetChecksum(const std::uint8_t *data, std::size_t length) {
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i + 1 < length; i += 2) {
            sum += Be16(data + i);
        }
        if ((length & 1) != 0) {
            sum += static_cast<std::uint32_t>(data[length - 1]) << 8;
        }
        while ((sum >> 16) != 0) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return static_cast<std::uint16_t>(~sum);
    }

    constexpr inline std::size_t DhcpDiscoverFrameSize = 14 + 20 + 8 + 300;

    /* Build a broadcast DHCPDISCOVER. `frame` must hold DhcpDiscoverFrameSize bytes. */
    inline void BuildDhcpDiscover(std::uint8_t *frame, const std::uint8_t *mac, std::uint32_t xid) {
        constexpr std::size_t BootpSize = 300;
        std::memset(frame, 0, DhcpDiscoverFrameSize);

        /* Ethernet */
        std::memset(frame, 0xFF, 6);
        std::memcpy(frame + 6, mac, 6);
        PutBe16(frame + 12, 0x0800);

        /* IPv4, 0.0.0.0 -> 255.255.255.255 */
        std::uint8_t *ip = frame + 14;
        ip[0] = 0x45;
        PutBe16(ip + 2, 20 + 8 + BootpSize);
        ip[8] = 64;
        ip[9] = 17;
        std::memset(ip + 16, 0xFF, 4);
        PutBe16(ip + 10, InternetChecksum(ip, 20));

        /* UDP 68 -> 67, checksum not used */
        std::uint8_t *udp = ip + 20;
        PutBe16(udp + 0, 68);
        PutBe16(udp + 2, 67);
        PutBe16(udp + 4, 8 + BootpSize);

        /* BOOTP */
        std::uint8_t *bootp = udp + 8;
        bootp[0] = 1; /* request */
        bootp[1] = 1; /* ethernet */
        bootp[2] = 6;
        std::memcpy(bootp + 4, &xid, 4);
        PutBe16(bootp + 10, 0x8000); /* ask for a broadcast reply */
        std::memcpy(bootp + 28, mac, 6);
        const std::uint8_t options[] = { 99, 130, 83, 99, /* magic */ 53, 1, 1, /* DISCOVER */ 55, 3, 1, 3, 6, /* want mask, router, dns */ 255 };
        std::memcpy(bootp + 236, options, sizeof(options));
    }

    struct DhcpReply {
        std::uint8_t your_ip[4];
        std::uint8_t server_ip[4];
    };

    /* True if `frame` is a BOOTP reply to our transaction. */
    inline bool ParseDhcpReply(const std::uint8_t *frame, std::size_t length, std::uint32_t xid, DhcpReply *out) {
        if (length < 14 + 20 + 8 + 240 || Be16(frame + 12) != 0x0800) {
            return false;
        }
        const std::uint8_t *ip = frame + 14;
        const std::size_t ihl = static_cast<std::size_t>(ip[0] & 0x0F) * 4;
        if ((ip[0] >> 4) != 4 || ip[9] != 17 || ihl < 20 || 14 + ihl + 8 + 240 > length) {
            return false;
        }
        const std::uint8_t *udp = ip + ihl;
        if (Be16(udp + 2) != 68) {
            return false;
        }
        const std::uint8_t *bootp = udp + 8;
        if (bootp[0] != 2 || std::memcmp(bootp + 4, &xid, 4) != 0) {
            return false;
        }
        std::memcpy(out->your_ip, bootp + 16, 4);
        std::memcpy(out->server_ip, ip + 12, 4);
        return true;
    }

}
