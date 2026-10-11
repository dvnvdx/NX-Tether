/*
 * PC-side tests for the parts of usb-tether that do not need a Switch:
 *   - the ASIX AX88772 register model, driven the way the Linux and FreeBSD drivers drive a real adapter
 *   - ASIX bulk framing, checked against re-implementations of those drivers' parsers/builders
 *   - RNDIS and CDC-NCM framing, checked against re-implementations of the Linux USB gadget
 *     (which is what an Android phone runs)
 *   - the DHCP probe frame
 *
 * Build and run:  g++ -std=c++20 -Wall -Wextra -O1 -I../sysmodule/source test_core.cpp -o test_core && ./test_core
 */
#include "ubt_asix_model.hpp"
#include "ubt_framing.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>

using namespace ubtcore;
using Bytes = std::vector<std::uint8_t>;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::mt19937 g_rng(12345);

static Bytes MakeFrame(std::size_t length, bool broadcast = false) {
    Bytes frame(length);
    for (auto &b : frame) { b = static_cast<std::uint8_t>(g_rng()); }
    if (length >= 14) {
        if (broadcast) { for (int i = 0; i < 6; ++i) frame[i] = 0xFF; } else { frame[0] &= 0xFE; }
        frame[12] = 0x08; frame[13] = 0x00;
    }
    return frame;
}

/* ------------------------------------------------------------------------------------------- */
/* A miniature USB "host side" in front of the model, with the helpers the Linux driver uses.  */
/* ------------------------------------------------------------------------------------------- */

struct Device {
    asix::Model model;
    int unknown = 0;

    int read_cmd(std::uint8_t cmd, std::uint16_t value, std::uint16_t index, std::uint16_t size, void *data) {
        Bytes buf(size ? size : 1, 0);
        const auto r = model.HandleVendorRequest(cmd, true, value, index, size, buf.data());
        if (r.event == asix::Event_UnknownRequest) { ++unknown; }
        std::memcpy(data, buf.data(), r.out_size);
        return static_cast<int>(r.out_size);
    }

    int write_cmd(std::uint8_t cmd, std::uint16_t value, std::uint16_t index, std::uint16_t size, const void *data) {
        Bytes buf(size ? size : 1, 0);
        if (size) { std::memcpy(buf.data(), data, size); }
        const auto r = model.HandleVendorRequest(cmd, false, value, index, size, buf.data());
        if (r.event == asix::Event_UnknownRequest) { ++unknown; }
        return size;
    }
};

/* Linux: asix_check_host_enable + __asix_mdio_read */
static int linux_mdio_read(Device &dev, int phy_id, int loc) {
    std::uint8_t smsr = 0;
    int i;
    for (i = 0; i < 30; ++i) {
        dev.write_cmd(0x06 /* AX_CMD_SET_SW_MII */, 0, 0, 0, nullptr);
        if (dev.read_cmd(0x09 /* AX_CMD_STATMNGSTS_REG */, 0, 0, 1, &smsr) < 1) { continue; }
        if (smsr & 0x01 /* AX_HOST_EN */) { break; }
    }
    if (i >= 30) { return -1; }
    std::uint8_t res[2] = {};
    if (dev.read_cmd(0x07 /* AX_CMD_READ_MII_REG */, static_cast<std::uint16_t>(phy_id), static_cast<std::uint16_t>(loc), 2, res) < 2) { return -1; }
    dev.write_cmd(0x0A /* AX_CMD_SET_HW_MII */, 0, 0, 0, nullptr);
    return res[0] | (res[1] << 8);
}

static void linux_mdio_write(Device &dev, int phy_id, int loc, int val) {
    const std::uint8_t v[2] = { static_cast<std::uint8_t>(val), static_cast<std::uint8_t>(val >> 8) };
    dev.write_cmd(0x06, 0, 0, 0, nullptr);
    dev.write_cmd(0x08 /* AX_CMD_WRITE_MII_REG */, static_cast<std::uint16_t>(phy_id), static_cast<std::uint16_t>(loc), 2, v);
    dev.write_cmd(0x0A, 0, 0, 0, nullptr);
}

/* Mirrors ax88772_bind() + ax88772_hw_reset() + a generic PHY bring-up from Linux 6.x. */
static void TestLinuxDriverSequence(std::uint16_t pid) {
    const std::uint8_t mac[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    Device dev;
    dev.model.Reset(pid, mac);

    /* MAC address */
    std::uint8_t buf[6] = {};
    if (asix::IsChecksumCapableProduct(pid)) {
        /* FLAG_EEPROM_MAC: three EEPROM words starting at 0x04 */
        for (int i = 0; i < 3; ++i) {
            CHECK(dev.read_cmd(0x0B /* AX_CMD_READ_EEPROM */, static_cast<std::uint16_t>(0x04 + i), 0, 2, buf + i * 2) == 2);
        }
    } else {
        CHECK(dev.read_cmd(0x13 /* AX_CMD_READ_NODE_ID */, 0, 0, 6, buf) == 6);
    }
    CHECK(std::memcmp(buf, mac, 6) == 0);

    /* asix_read_phy_addr(dev, true) */
    std::uint8_t phy_ids[2] = {};
    CHECK(dev.read_cmd(0x19 /* AX_CMD_READ_PHY_ID */, 0, 0, 2, phy_ids) == 2);
    const int phy_addr = phy_ids[1];
    CHECK(phy_addr < 32);
    const bool embd_phy = (phy_addr & 0x1F) == 0x10;
    CHECK(embd_phy);

    /* chip code */
    std::uint8_t chipcode = 0xFF;
    CHECK(dev.read_cmd(0x09, 0, 0, 1, &chipcode) == 1);
    chipcode &= 0x70;
    if (pid == 0x7720)      { CHECK(chipcode == 0x00); }
    else if (pid == 0x772A) { CHECK(chipcode == 0x10); }
    else                    { CHECK(chipcode == 0x20); }

    /* ax88772_hw_reset() */
    dev.write_cmd(0x1F /* AX_CMD_WRITE_GPIOS */, 0x80 | 0x20 | 0x10, 0, 0, nullptr);
    dev.write_cmd(0x22 /* AX_CMD_SW_PHY_SELECT */, embd_phy, 0, 0, nullptr);
    dev.write_cmd(0x20 /* AX_CMD_SW_RESET */, 0x40 /* IPPD */, 0, 0, nullptr);
    dev.write_cmd(0x20, 0x00, 0, 0, nullptr);
    dev.write_cmd(0x20, 0x20 | 0x08 /* IPRL | PRL */, 0, 0, nullptr);
    CHECK(linux_mdio_read(dev, phy_addr, 2 /* MII_PHYSID1 */) > 0);

    const std::uint16_t default_rx_ctl = 0x0080 | 0x0008; /* SO | AB */
    const std::uint16_t medium_default = 0x0200 | 0x0002 | 0x0010 | 0x0020 | 0x0004 | 0x0100; /* PS|FD|RFC|TFC|AC|RE */
    dev.write_cmd(0x10 /* AX_CMD_WRITE_RX_CTL */, default_rx_ctl, 0, 0, nullptr);
    dev.write_cmd(0x1B /* AX_CMD_WRITE_MEDIUM_MODE */, medium_default, 0, 0, nullptr);
    dev.write_cmd(0x12 /* AX_CMD_WRITE_IPG0 */, 0x15 | (0x0C << 8), 0x12, 0, nullptr);
    dev.write_cmd(0x14 /* AX_CMD_WRITE_NODE_ID */, 0, 0, 6, mac);
    dev.write_cmd(0x10, default_rx_ctl, 0, 0, nullptr);

    std::uint8_t v[2] = {};
    CHECK(dev.read_cmd(0x0F /* AX_CMD_READ_RX_CTL */, 0, 0, 2, v) == 2);
    CHECK((v[0] | (v[1] << 8)) == default_rx_ctl);
    CHECK(dev.read_cmd(0x1A /* AX_CMD_READ_MEDIUM_STATUS */, 0, 0, 2, v) == 2);
    CHECK((v[0] | (v[1] << 8)) == medium_default);

    /* PHY probe and autonegotiation, the way a generic MII PHY driver does it. */
    const int id1 = linux_mdio_read(dev, phy_addr, 2);
    const int id2 = linux_mdio_read(dev, phy_addr, 3);
    CHECK(id1 > 0 && id1 != 0xFFFF);
    CHECK(id2 > 0 && id2 != 0xFFFF);

    linux_mdio_write(dev, phy_addr, 0 /* MII_BMCR */, 0x8000 /* BMCR_RESET */);
    CHECK((linux_mdio_read(dev, phy_addr, 0) & 0x8000) == 0);      /* reset bit self-clears */
    linux_mdio_write(dev, phy_addr, 4 /* MII_ADVERTISE */, 0x01E1);
    linux_mdio_write(dev, phy_addr, 0, 0x1000 | 0x0200 /* ANENABLE | ANRESTART */);
    CHECK((linux_mdio_read(dev, phy_addr, 0) & 0x0200) == 0);      /* restart bit self-clears */

    const int bmsr = linux_mdio_read(dev, phy_addr, 1 /* MII_BMSR */);
    CHECK((bmsr & 0x0004) != 0);  /* link up */
    CHECK((bmsr & 0x0020) != 0);  /* autoneg complete */
    const int adv = linux_mdio_read(dev, phy_addr, 4);
    const int lpa = linux_mdio_read(dev, phy_addr, 5);
    CHECK(adv == 0x01E1);
    CHECK(((adv & lpa) & 0x0100) != 0); /* 100BASE-TX full duplex negotiated */

    /* Nothing to see on an address without a PHY. */
    CHECK(linux_mdio_read(dev, 0x03, 2) == 0xFFFF);

    CHECK(dev.model.IsRxEnabled());
    CHECK(!dev.model.UsesChecksumHeader());
    CHECK(dev.unknown == 0);
}

/* FreeBSD if_axe: how it interprets READ_PHYID and the receive header. */
static void TestFreeBsdExpectations() {
    const std::uint8_t mac[6] = { 0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    Device dev;
    dev.model.Reset(0x7720, mac);

    std::uint8_t phyaddrs[2] = {};
    CHECK(dev.read_cmd(0x19, 0, 0, 2, phyaddrs) == 2);
    /* axe_get_phyno(sc, AXE_PHY_SEL_PRI): type must be 10/100 (0) or gigabit (1) */
    const int pri_type = (phyaddrs[1] & 0xE0) >> 5;
    CHECK(pri_type == 0 || pri_type == 1);
    CHECK((phyaddrs[1] & 0x1F) == 0x10);          /* AXE_772_PHY_NO_EPHY */
    CHECK(((phyaddrs[0] & 0xE0) >> 5) == 7);      /* secondary: PHY_TYPE_NON_SUP */

    std::uint8_t ipg[3] = {};
    CHECK(dev.read_cmd(0x11 /* AXE_CMD_READ_IPG012 */, 0, 0, 3, ipg) == 3);

    std::uint8_t eaddr[6] = {};
    CHECK(dev.read_cmd(0x13 /* AXE_178_CMD_READ_NODEID */, 0, 0, 6, eaddr) == 6);
    CHECK(std::memcmp(eaddr, mac, 6) == 0);

    /* Standard frame header check in axe_rx_frame(): (hdr.len ^ hdr.ilen) == 0xFFFF */
    dev.model.rx_ctl = 0x0088;
    const Bytes frame = MakeFrame(100);
    std::uint8_t out[256];
    const std::size_t n = dev.model.PackFrame(out, sizeof(out), frame.data(), frame.size());
    CHECK(n == 104);
    const std::uint16_t len = asix::Le16(out), ilen = asix::Le16(out + 2);
    CHECK(static_cast<std::uint16_t>(len ^ ilen) == 0xFFFF);
    CHECK(len == 100);
}

/* ------------------------------------------------------------------------------------------- */
/* ASIX framing                                                                                */
/* ------------------------------------------------------------------------------------------- */

/* Re-implementation of Linux asix_rx_fixup_internal() for a whole transfer in one buffer. */
static bool LinuxAsixRxFixup(const Bytes &skb, std::vector<Bytes> &out) {
    std::size_t offset = 0;
    while (offset + 2 <= skb.size()) {
        if (skb.size() - offset < 4) { return false; }
        const std::uint32_t header = framing::Le32(skb.data() + offset);
        offset += 4;
        const std::uint16_t size = static_cast<std::uint16_t>(header & 0x7FF);
        if (size != ((~header >> 16) & 0x7FF)) { return false; }
        if (size > 1500 + 14 + 4) { return false; }
        if (size > skb.size() - offset) { return false; }
        out.emplace_back(skb.begin() + offset, skb.begin() + offset + size);
        offset += (size + 1) & 0xFFFE;
    }
    return skb.size() == offset;
}

static void TestAsixReceiveFraming() {
    const std::uint8_t mac[6] = { 2, 0, 0, 0, 0, 1 };
    asix::Model model;
    model.Reset(0x7720, mac);
    model.rx_ctl = 0x0088;

    /* Several frames in one transfer, including a runt that must be padded and odd lengths. */
    const std::size_t lengths[] = { 42, 60, 61, 590, 1513, 1514 };
    Bytes transfer(16384);
    std::size_t used = 0;
    std::vector<Bytes> sent;
    for (const std::size_t length : lengths) {
        const Bytes frame = MakeFrame(length);
        const std::size_t n = model.PackFrame(transfer.data() + used, transfer.size() - used, frame.data(), frame.size());
        CHECK(n != 0);
        CHECK((n & 1) == 0);
        used += n;
        sent.push_back(frame);
    }
    transfer.resize(used);

    std::vector<Bytes> received;
    CHECK(LinuxAsixRxFixup(transfer, received));
    CHECK(received.size() == sent.size());
    for (std::size_t i = 0; i < sent.size() && i < received.size(); ++i) {
        const std::size_t expected = sent[i].size() < 60 ? 60 : sent[i].size();
        CHECK(received[i].size() == expected);
        CHECK(std::memcmp(received[i].data(), sent[i].data(), sent[i].size()) == 0);
        for (std::size_t j = sent[i].size(); j < received[i].size(); ++j) { CHECK(received[i][j] == 0); }
    }

    /* Does not fit: must report 0 and leave the caller to try the next buffer. */
    std::uint8_t small[64];
    const Bytes big = MakeFrame(100);
    CHECK(model.PackFrame(small, sizeof(small), big.data(), big.size()) == 0);

    /* AX88772B checksum-header mode (FreeBSD axe_rx_frame, AXE_FLAG_CSUM_FRAME branch). */
    asix::Model b;
    b.Reset(0x772B, mac);
    b.rx_ctl = 0x0088 | asix::RxCtlHeaderType1;
    CHECK(b.UsesChecksumHeader());
    const Bytes bf = MakeFrame(61, true);
    std::uint8_t out[128];
    const std::size_t n = b.PackFrame(out, sizeof(out), bf.data(), bf.size());
    CHECK(n == ((6 + 61 + 3) & ~3u));
    const std::uint16_t len = asix::Le16(out), ilen = asix::Le16(out + 2);
    CHECK(((len & 0x7FF) ^ (ilen & 0x7FF)) == 0x7FF);
    CHECK((len & 0x7FF) == 61);
    CHECK((len & 0x8000) != 0);
    CHECK(std::memcmp(out + 6, bf.data(), bf.size()) == 0);
}

/* Linux asix_tx_fixup() */
static Bytes LinuxAsixTxFixup(const Bytes &frame, std::size_t maxpacket) {
    Bytes out;
    const std::uint32_t len = static_cast<std::uint32_t>(frame.size());
    const std::uint32_t packet_len = ((len ^ 0x0000FFFF) << 16) + len;
    out.resize(4);
    framing::PutLe32(out.data(), packet_len);
    out.insert(out.end(), frame.begin(), frame.end());
    if (((frame.size() + 4) & (maxpacket - 1)) == 0) {
        std::uint8_t pad[4];
        framing::PutLe32(pad, 0xFFFF0000u);
        out.insert(out.end(), pad, pad + 4);
    }
    return out;
}

/* FreeBSD axe_bulk_write_callback(): frames back to back, each with a header. */
static Bytes FreeBsdAxeTx(const std::vector<Bytes> &frames) {
    Bytes out;
    for (const Bytes &frame : frames) {
        Bytes one(4);
        const std::uint16_t len = static_cast<std::uint16_t>(frame.size());
        asix::PutLe16(one.data(), len);
        asix::PutLe16(one.data() + 2, static_cast<std::uint16_t>(~len));
        one.insert(one.end(), frame.begin(), frame.end());
        if ((one.size() % 512) == 0) {
            const std::uint8_t pad[4] = { 0x00, 0x00, 0xFF, 0xFF };
            one.insert(one.end(), pad, pad + 4);
        }
        out.insert(out.end(), one.begin(), one.end());
    }
    return out;
}

static void TestAsixTransmitParsing() {
    /* Linux: one frame per transfer; 508 byte frame triggers the padding word. */
    for (const std::size_t length : { std::size_t(42), std::size_t(60), std::size_t(508), std::size_t(1020), std::size_t(1514) }) {
        const Bytes frame = MakeFrame(length);
        const Bytes transfer = LinuxAsixTxFixup(frame, 512);
        std::vector<Bytes> got;
        const long bad = asix::Model::ParseTransmit(transfer.data(), transfer.size(), [&](const std::uint8_t *f, std::size_t n) { got.emplace_back(f, f + n); });
        CHECK(bad == -1);
        CHECK(got.size() == 1);
        if (!got.empty()) { CHECK(got[0] == frame); }
    }

    /* FreeBSD: several frames, even and odd lengths, in one transfer. */
    {
        const std::vector<Bytes> frames = { MakeFrame(60), MakeFrame(1514), MakeFrame(508), MakeFrame(98), MakeFrame(342) };
        const Bytes transfer = FreeBsdAxeTx(frames);
        std::vector<Bytes> got;
        const long bad = asix::Model::ParseTransmit(transfer.data(), transfer.size(), [&](const std::uint8_t *f, std::size_t n) { got.emplace_back(f, f + n); });
        CHECK(bad == -1);
        CHECK(got == frames);
    }

    /* Odd-length frames padded to even between frames. */
    {
        const std::vector<Bytes> frames = { MakeFrame(61), MakeFrame(75), MakeFrame(64) };
        Bytes transfer;
        for (const Bytes &frame : frames) {
            std::uint8_t hdr[4];
            asix::PutLe16(hdr, static_cast<std::uint16_t>(frame.size()));
            asix::PutLe16(hdr + 2, static_cast<std::uint16_t>(~frame.size()));
            transfer.insert(transfer.end(), hdr, hdr + 4);
            transfer.insert(transfer.end(), frame.begin(), frame.end());
            if (transfer.size() & 1) { transfer.push_back(0); }
        }
        std::vector<Bytes> got;
        const long bad = asix::Model::ParseTransmit(transfer.data(), transfer.size(), [&](const std::uint8_t *f, std::size_t n) { got.emplace_back(f, f + n); });
        CHECK(bad == -1);
        CHECK(got == frames);
    }

    /* Garbage is reported, not delivered. */
    {
        const Bytes garbage = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
        int count = 0;
        const long bad = asix::Model::ParseTransmit(garbage.data(), garbage.size(), [&](const std::uint8_t *, std::size_t) { ++count; });
        CHECK(bad == 0);
        CHECK(count == 0);
    }

    /* A header that claims more data than the transfer holds. */
    {
        Bytes transfer(4 + 20);
        asix::PutLe16(transfer.data(), 100);
        asix::PutLe16(transfer.data() + 2, static_cast<std::uint16_t>(~100));
        int count = 0;
        const long bad = asix::Model::ParseTransmit(transfer.data(), transfer.size(), [&](const std::uint8_t *, std::size_t) { ++count; });
        CHECK(bad == 0);
        CHECK(count == 0);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* RNDIS                                                                                       */
/* ------------------------------------------------------------------------------------------- */

/* Linux gadget rndis_rm_hdr(): what the phone does with a transfer from us. */
static bool GadgetRndisRmHdr(const Bytes &skb, Bytes &out) {
    if (skb.size() < 16) { return false; }
    if (framing::Le32(skb.data()) != 1) { return false; }
    const std::uint32_t pull = framing::Le32(skb.data() + 8) + 8;
    if (pull > skb.size()) { return false; }
    const std::uint32_t data_length = framing::Le32(skb.data() + 12);
    Bytes rest(skb.begin() + pull, skb.end());
    if (data_length < rest.size()) { rest.resize(data_length); } /* skb_trim only shortens */
    out = rest;
    return true;
}

/* Linux gadget rndis_add_hdr(): what the phone sends us. */
static Bytes GadgetRndisAddHdr(const Bytes &frame) {
    Bytes out(44, 0);
    framing::PutLe32(out.data() + 0, 1);
    framing::PutLe32(out.data() + 4, static_cast<std::uint32_t>(44 + frame.size()));
    framing::PutLe32(out.data() + 8, 36);
    framing::PutLe32(out.data() + 12, static_cast<std::uint32_t>(frame.size()));
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

static void TestRndisFraming() {
    std::uint8_t buffer[4096];

    /* Us -> phone. 468 + 44 = 512 exercises the pad byte. */
    for (const std::size_t length : { std::size_t(42), std::size_t(60), std::size_t(468), std::size_t(980), std::size_t(1514) }) {
        const Bytes frame = MakeFrame(length);
        const std::size_t n = framing::BuildRndisPacket(buffer, sizeof(buffer), frame.data(), frame.size(), 512);
        CHECK(n >= 44 + length);
        CHECK((n % 512) != 0);
        Bytes got;
        CHECK(GadgetRndisRmHdr(Bytes(buffer, buffer + n), got));
        CHECK(got == frame);
        CHECK(framing::Le32(buffer + 4) == 44 + length);
    }

    /* Phone -> us: single message, several messages in one transfer, and trailing padding. */
    {
        const std::vector<Bytes> frames = { MakeFrame(60), MakeFrame(1514), MakeFrame(342), MakeFrame(64) };
        Bytes transfer;
        for (const Bytes &frame : frames) {
            const Bytes msg = GadgetRndisAddHdr(frame);
            transfer.insert(transfer.end(), msg.begin(), msg.end());
        }
        std::vector<Bytes> got;
        unsigned bad = framing::ParseRndisTransfer(transfer.data(), transfer.size(), [&](const std::uint8_t *f, std::size_t n) { got.emplace_back(f, f + n); });
        CHECK(bad == 0);
        CHECK(got == frames);

        /* A zero pad byte after the last message is not an error. */
        transfer.push_back(0);
        got.clear();
        bad = framing::ParseRndisTransfer(transfer.data(), transfer.size(), [&](const std::uint8_t *f, std::size_t n) { got.emplace_back(f, f + n); });
        CHECK(bad == 0);
        CHECK(got == frames);
    }

    /* Corrupt lengths must not read out of bounds. */
    {
        Bytes msg = GadgetRndisAddHdr(MakeFrame(100));
        framing::PutLe32(msg.data() + 12, 5000);
        int count = 0;
        const unsigned bad = framing::ParseRndisTransfer(msg.data(), msg.size(), [&](const std::uint8_t *, std::size_t) { ++count; });
        CHECK(bad == 1 && count == 0);

        msg = GadgetRndisAddHdr(MakeFrame(100));
        framing::PutLe32(msg.data() + 4, 100000);
        count = 0;
        CHECK(framing::ParseRndisTransfer(msg.data(), msg.size(), [&](const std::uint8_t *, std::size_t) { ++count; }) == 1);
        CHECK(count == 0);

        msg = GadgetRndisAddHdr(MakeFrame(100));
        framing::PutLe32(msg.data() + 8, 0xFFFFFFF0u);
        count = 0;
        CHECK(framing::ParseRndisTransfer(msg.data(), msg.size(), [&](const std::uint8_t *, std::size_t) { ++count; }) == 1);
        CHECK(count == 0);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* CDC-NCM                                                                                     */
/* ------------------------------------------------------------------------------------------- */

/* Linux gadget ncm_unwrap_ntb() for 16 bit NTBs without CRC: what the phone does with our NTB. */
static bool GadgetNcmUnwrap(const Bytes &skb, std::vector<Bytes> &out, unsigned ntb_max = 16384, unsigned frame_max = 1514) {
    const std::uint8_t *ntb = skb.data();
    int to_process = static_cast<int>(skb.size());

    while (true) {
        if (to_process < 12) { return false; }
        if (framing::Le32(ntb) != 0x484D434E) { return false; }
        if (framing::Le16(ntb + 4) != 12) { return false; }
        unsigned block_len = framing::Le16(ntb + 8);
        if (block_len == 0) { block_len = to_process; }
        if (block_len < 12 + 8 || block_len > ntb_max || block_len > static_cast<unsigned>(to_process)) { return false; }
        unsigned ndp_index = framing::Le16(ntb + 10);

        do {
            if ((ndp_index % 4) != 0 || ndp_index < 12 || ndp_index > block_len - 8) { return false; }
            const std::uint8_t *tmp = ntb + ndp_index;
            if (framing::Le32(tmp) != 0x304D434E) { return false; }
            unsigned ndp_len = framing::Le16(tmp + 4);
            if (ndp_len < 8 + 2 * 2 * 2 || (ndp_len % 4) != 0) { return false; }
            ndp_index = framing::Le16(tmp + 6);
            tmp += 8;
            ndp_len -= 8;
            unsigned index2 = framing::Le16(tmp), dg_len2 = framing::Le16(tmp + 2);
            tmp += 4;
            do {
                const unsigned index = index2;
                if (index < 12 || index > block_len) { return false; }
                const unsigned dg_len = dg_len2;
                if (dg_len < 14 || dg_len > frame_max || dg_len > block_len - index) { return false; }
                index2 = framing::Le16(tmp); dg_len2 = framing::Le16(tmp + 2);
                tmp += 4;
                if (index2 > block_len) { return false; }
                out.emplace_back(ntb + index, ntb + index + dg_len);
                ndp_len -= 4;
                if (index2 == 0 || dg_len2 == 0) { break; }
            } while (ndp_len > 4);
        } while (ndp_index);

        to_process -= block_len;
        if (to_process == 1 && ntb[block_len] == 0x00) {
            to_process--;
        } else if (to_process > 0 && block_len != 0) {
            ntb += block_len;
            continue;
        }
        break;
    }
    return true;
}

/* Linux gadget ncm_wrap_ntb()/package_for_tx(): NTH16, datagrams, then the NDP16 at the end. */
static Bytes GadgetNcmWrap(const std::vector<Bytes> &frames, unsigned div = 4, unsigned rem = 2, unsigned ndp_align = 4) {
    Bytes data(12, 0);
    framing::PutLe32(data.data(), 0x484D434E);
    framing::PutLe16(data.data() + 4, 12);
    Bytes ndp(8, 0);
    framing::PutLe32(ndp.data(), 0x304D434E);

    for (const Bytes &frame : frames) {
        std::size_t ncb_len = data.size();
        const std::size_t pad = ((ncb_len + div - 1) / div) * div + rem - ncb_len;
        ncb_len += pad;
        std::uint8_t entry[4];
        framing::PutLe16(entry, static_cast<std::uint16_t>(ncb_len));
        framing::PutLe16(entry + 2, static_cast<std::uint16_t>(frame.size()));
        ndp.insert(ndp.end(), entry, entry + 4);
        data.insert(data.end(), pad, 0);
        data.insert(data.end(), frame.begin(), frame.end());
    }
    /* terminating zero entry */
    ndp.insert(ndp.end(), 4, 0);

    /* package_for_tx(): align, append NDP, fill in lengths. */
    const std::size_t ndp_pad = ((data.size() + ndp_align - 1) / ndp_align) * ndp_align - data.size();
    const std::size_t ndp_index = data.size() + ndp_pad;
    framing::PutLe16(ndp.data() + 4, static_cast<std::uint16_t>(ndp.size()));
    data.insert(data.end(), ndp_pad, 0);
    data.insert(data.end(), ndp.begin(), ndp.end());
    framing::PutLe16(data.data() + 8, static_cast<std::uint16_t>(data.size()));
    framing::PutLe16(data.data() + 10, static_cast<std::uint16_t>(ndp_index));
    return data;
}

static void TestNcmFraming() {
    std::uint8_t buffer[4096];

    /* Us -> phone, with the alignment rules phones typically announce, and a stricter set. */
    const framing::NcmOutParameters parameter_sets[] = { { 4, 2, 4 }, { 4, 0, 4 }, { 1, 0, 4 }, { 8, 6, 8 } };
    std::uint16_t sequence = 0;
    for (const auto &params : parameter_sets) {
        for (const std::size_t length : { std::size_t(42), std::size_t(60), std::size_t(482), std::size_t(484), std::size_t(1514) }) {
            const Bytes frame = MakeFrame(length);
            const std::size_t n = framing::BuildNcmBlock(buffer, sizeof(buffer), frame.data(), frame.size(), params, sequence++, 512);
            CHECK(n != 0);
            CHECK((n % 512) != 0);
            CHECK(framing::Le16(buffer + 8) == n);
            const std::size_t dg_index = framing::Le16(buffer + framing::Le16(buffer + 10) + 8);
            CHECK((dg_index % params.divisor) == params.remainder);
            CHECK((framing::Le16(buffer + 10) % params.alignment) == 0);

            std::vector<Bytes> got;
            CHECK(GadgetNcmUnwrap(Bytes(buffer, buffer + n), got));
            CHECK(got.size() == 1);
            if (!got.empty()) { CHECK(got[0] == frame); }
        }
    }

    /* Phone -> us: aggregated datagrams with the NDP at the end. */
    {
        const std::vector<Bytes> frames = { MakeFrame(60), MakeFrame(1514), MakeFrame(590), MakeFrame(61) };
        const Bytes block = GadgetNcmWrap(frames);
        std::vector<Bytes> got;
        const unsigned bad = framing::ParseNcmBlock(block.data(), block.size(), [&](const std::uint8_t *f, std::size_t n) { got.emplace_back(f, f + n); });
        CHECK(bad == 0);
        CHECK(got == frames);

        /* Sanity check of the reference itself: the gadget's own parser accepts its output. */
        std::vector<Bytes> check;
        CHECK(GadgetNcmUnwrap(block, check));
        CHECK(check == frames);
    }

    /* Malformed blocks. */
    {
        Bytes block = GadgetNcmWrap({ MakeFrame(100) });
        block[0] ^= 0xFF;
        int count = 0;
        CHECK(framing::ParseNcmBlock(block.data(), block.size(), [&](const std::uint8_t *, std::size_t) { ++count; }) != 0);
        CHECK(count == 0);

        block = GadgetNcmWrap({ MakeFrame(100) });
        const std::size_t ndp = framing::Le16(block.data() + 10);
        framing::PutLe16(block.data() + ndp + 10, 5000); /* datagram length beyond the block */
        count = 0;
        CHECK(framing::ParseNcmBlock(block.data(), block.size(), [&](const std::uint8_t *, std::size_t) { ++count; }) != 0);
        CHECK(count == 0);

        block = GadgetNcmWrap({ MakeFrame(100) });
        framing::PutLe16(block.data() + 10, 0xFFF0); /* NDP index beyond the block */
        count = 0;
        CHECK(framing::ParseNcmBlock(block.data(), block.size(), [&](const std::uint8_t *, std::size_t) { ++count; }) != 0);
        CHECK(count == 0);
    }
}

/* ------------------------------------------------------------------------------------------- */
/* DHCP probe                                                                                  */
/* ------------------------------------------------------------------------------------------- */

static void TestDhcpProbe(const char *dump_path) {
    const std::uint8_t mac[6] = { 0x02, 0x12, 0x34, 0x56, 0x78, 0x9A };
    const std::uint32_t xid = 0xDEADBEEF;
    std::uint8_t frame[framing::DhcpDiscoverFrameSize];
    framing::BuildDhcpDiscover(frame, mac, xid);

    /* Ethernet + IPv4 header sanity. */
    for (int i = 0; i < 6; ++i) { CHECK(frame[i] == 0xFF); }
    CHECK(std::memcmp(frame + 6, mac, 6) == 0);
    CHECK(framing::Be16(frame + 12) == 0x0800);
    CHECK(framing::InternetChecksum(frame + 14, 20) == 0); /* a correct header sums to zero */
    CHECK(framing::Be16(frame + 14 + 2) == sizeof(frame) - 14);
    CHECK(framing::Be16(frame + 34 + 4) == sizeof(frame) - 34);

    if (dump_path != nullptr) {
        if (std::FILE *f = std::fopen(dump_path, "wb")) {
            std::fwrite(frame, 1, sizeof(frame), f);
            std::fclose(f);
        }
    }

    /* Our own DISCOVER is not a reply. */
    framing::DhcpReply reply;
    CHECK(!framing::ParseDhcpReply(frame, sizeof(frame), xid, &reply));

    /* Build an OFFER the way a DHCP server would (unicast to our MAC, from 192.168.42.129). */
    Bytes offer(14 + 20 + 8 + 300, 0);
    std::memcpy(offer.data(), mac, 6);
    const std::uint8_t server_mac[6] = { 0x02, 0, 0, 0, 0, 0x99 };
    std::memcpy(offer.data() + 6, server_mac, 6);
    framing::PutBe16(offer.data() + 12, 0x0800);
    std::uint8_t *ip = offer.data() + 14;
    ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
    framing::PutBe16(ip + 2, 20 + 8 + 300);
    const std::uint8_t server_ip[4] = { 192, 168, 42, 129 }, your_ip[4] = { 192, 168, 42, 57 };
    std::memcpy(ip + 12, server_ip, 4);
    std::memcpy(ip + 16, your_ip, 4);
    framing::PutBe16(ip + 10, framing::InternetChecksum(ip, 20));
    std::uint8_t *udp = ip + 20;
    framing::PutBe16(udp, 67); framing::PutBe16(udp + 2, 68); framing::PutBe16(udp + 4, 308);
    std::uint8_t *bootp = udp + 8;
    bootp[0] = 2; bootp[1] = 1; bootp[2] = 6;
    std::memcpy(bootp + 4, &xid, 4);
    std::memcpy(bootp + 16, your_ip, 4);
    std::memcpy(bootp + 28, mac, 6);

    CHECK(framing::ParseDhcpReply(offer.data(), offer.size(), xid, &reply));
    CHECK(std::memcmp(reply.your_ip, your_ip, 4) == 0);
    CHECK(std::memcmp(reply.server_ip, server_ip, 4) == 0);

    /* Wrong transaction, wrong port, truncated. */
    CHECK(!framing::ParseDhcpReply(offer.data(), offer.size(), xid + 1, &reply));
    Bytes other = offer;
    framing::PutBe16(other.data() + 14 + 20 + 2, 53);
    CHECK(!framing::ParseDhcpReply(other.data(), other.size(), xid, &reply));
    CHECK(!framing::ParseDhcpReply(offer.data(), 100, xid, &reply));
}

int main(int argc, char **argv) {
    for (const std::uint16_t pid : asix::SupportedProducts) {
        TestLinuxDriverSequence(pid);
    }
    TestFreeBsdExpectations();
    TestAsixReceiveFraming();
    TestAsixTransmitParsing();
    TestRndisFraming();
    TestNcmFraming();
    TestDhcpProbe(argc > 1 ? argv[1] : nullptr);

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
