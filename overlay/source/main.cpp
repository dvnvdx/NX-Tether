/*
 * usb-tether status overlay (Tesla / Ultrahand compatible).
 * Shows the state of the usb-tether system module and offers a few switches.
 */
#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

    /* Must match ams::ubt::Status in the system module. */
    struct UbtStatus {
        u32 struct_version;
        u32 module_version;
        u8  mode;
        u8  bridge_enabled;
        u8  link_state;
        u8  link_protocol;
        u16 phone_vid;
        u16 phone_pid;
        u8  mac[6];
        u8  eth_session_open;
        u8  eth_filter_count;
        u8  virt_attached;
        u8  virt_acquired;
        u8  virt_rx_enabled;
        u8  memory_mode;
        u16 emu_vid;
        u16 emu_pid;
        u32 last_error;
        u32 dhcp_offer_ip;
        u32 ctrl_requests;
        u32 unknown_requests;
        u64 rx_packets;
        u64 rx_bytes;
        u64 tx_packets;
        u64 tx_bytes;
        u64 rx_dropped;
        u64 tx_dropped;
        char message[64];
    };
    static_assert(sizeof(UbtStatus) == 0xA0);

    Service g_service;
    bool g_connected = false;

    bool Connect() {
        if (g_connected) {
            return true;
        }

        /* libtesla only keeps a service-manager session open during start-up, so open one  */
        /* for the lookup. Asking for a service that does not exist would block forever, so */
        /* check first (Atmosphere extension of the service manager, command 65100).        */
        bool ok = false;
        tsl::hlp::doWithSmSession([&ok] {
            const SmServiceName name = smEncodeName("usbtet");
            u8 exists = 0;
            if (R_FAILED(tipcDispatchInOut(smGetServiceSessionTipc(), 65100, name, exists)) || exists == 0) {
                return;
            }
            ok = R_SUCCEEDED(smGetService(&g_service, "usbtet"));
        });
        if (!ok) {
            return false;
        }
        g_connected = true;
        return true;
    }

    void Disconnect() {
        if (g_connected) {
            serviceClose(&g_service);
            g_connected = false;
        }
    }

    bool GetStatus(UbtStatus *out) {
        if (!Connect()) {
            return false;
        }
        std::memset(out, 0, sizeof(*out));
        const Result rc = serviceDispatch(&g_service, 0,
            .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
            .buffers = { { out, sizeof(*out) } },
        );
        if (R_FAILED(rc)) {
            Disconnect();
            return false;
        }
        return out->struct_version == 1;
    }

    void SetBridgeEnabled(bool enabled) {
        if (Connect()) {
            const u8 value = enabled ? 1 : 0;
            serviceDispatchIn(&g_service, 1, value);
        }
    }

    void SetBootMode(u8 mode) {
        if (Connect()) {
            serviceDispatchIn(&g_service, 2, mode);
        }
    }

    void Relink() {
        if (Connect()) {
            serviceDispatch(&g_service, 3);
        }
    }

    const char *ModeName(u8 mode) {
        switch (mode) {
            case 0:  return "Off";
            case 1:  return "Link test";
            case 2:  return "Bridge";
            default: return "?";
        }
    }

    const char *LinkStateName(u8 state) {
        switch (state) {
            case 0:  return "Waiting";
            case 1:  return "Starting";
            case 2:  return "Up";
            case 3:  return "Failed";
            default: return "?";
        }
    }

    const char *ProtocolName(u8 protocol) {
        switch (protocol) {
            case 1:  return "RNDIS";
            case 2:  return "NCM";
            case 3:  return "ECM";
            default: return "-";
        }
    }

    const char *MemoryModeName(u8 mode) {
        switch (mode) {
            case 1:  return "Mapped";
            case 2:  return "Debug";
            case 3:  return "FAILED";
            default: return "-";
        }
    }

    std::string FormatTraffic(u64 packets, u64 bytes) {
        char text[64];
        if (bytes >= 1024 * 1024) {
            std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        } else if (bytes >= 1024) {
            std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        } else {
            std::snprintf(text, sizeof(text), "%llu pkt", static_cast<unsigned long long>(packets));
        }
        return text;
    }

}

class StatusGui : public tsl::Gui {
    private:
        tsl::elm::ListItem *m_message = nullptr;
        tsl::elm::ListItem *m_link = nullptr;
        tsl::elm::ListItem *m_phone = nullptr;
        tsl::elm::ListItem *m_mac = nullptr;
        tsl::elm::ListItem *m_rx = nullptr;
        tsl::elm::ListItem *m_tx = nullptr;
        tsl::elm::ListItem *m_dhcp = nullptr;
        tsl::elm::ListItem *m_mode = nullptr;
        tsl::elm::ListItem *m_hook = nullptr;
        tsl::elm::ListItem *m_adapter = nullptr;
        tsl::elm::ListItem *m_requests = nullptr;
        tsl::elm::ListItem *m_memory = nullptr;
        tsl::elm::ListItem *m_error = nullptr;
        tsl::elm::ToggleListItem *m_bridge_toggle = nullptr;
        tsl::elm::ListItem *m_boot_mode = nullptr;
        int m_frame = 0;
        int m_next_boot_mode = -1;
        bool m_toggle_synced = false;
    public:
        virtual tsl::elm::Element *createUI() override {
            auto *frame = new tsl::elm::OverlayFrame("USB Tether", "v0.1.7");
            auto *list = new tsl::elm::List();

            m_message = new tsl::elm::ListItem("Module", "...");
            list->addItem(m_message);

            list->addItem(new tsl::elm::CategoryHeader("Phone link"));
            list->addItem(m_link  = new tsl::elm::ListItem("Link", "-"));
            list->addItem(m_phone = new tsl::elm::ListItem("Phone", "-"));
            list->addItem(m_mac   = new tsl::elm::ListItem("MAC", "-"));
            list->addItem(m_rx    = new tsl::elm::ListItem("Received", "-"));
            list->addItem(m_tx    = new tsl::elm::ListItem("Sent", "-"));
            list->addItem(m_dhcp  = new tsl::elm::ListItem("DHCP test", "-"));

            list->addItem(new tsl::elm::CategoryHeader("Switch side"));
            list->addItem(m_mode     = new tsl::elm::ListItem("Running mode", "-"));
            list->addItem(m_hook     = new tsl::elm::ListItem("LAN driver hooked", "-"));
            list->addItem(m_adapter  = new tsl::elm::ListItem("Virtual adapter", "-"));
            list->addItem(m_requests = new tsl::elm::ListItem("Driver requests", "-"));
            list->addItem(m_memory   = new tsl::elm::ListItem("Buffer access", "-"));
            list->addItem(m_error    = new tsl::elm::ListItem("Last error", "-"));

            list->addItem(new tsl::elm::CategoryHeader("Controls"));
            m_bridge_toggle = new tsl::elm::ToggleListItem("Bridge", true);
            m_bridge_toggle->setStateChangedListener([](bool state) {
                SetBridgeEnabled(state);
            });
            list->addItem(m_bridge_toggle);

            m_boot_mode = new tsl::elm::ListItem("Mode after reboot", "-");
            m_boot_mode->setClickListener([this](u64 keys) {
                if (keys & HidNpadButton_A) {
                    /* Cycle Bridge -> Link test -> Off. */
                    int mode = this->m_next_boot_mode;
                    mode = (mode <= 0) ? 2 : mode - 1;
                    this->m_next_boot_mode = mode;
                    SetBootMode(static_cast<u8>(mode));
                    this->m_boot_mode->setValue(ModeName(static_cast<u8>(mode)));
                    return true;
                }
                return false;
            });
            list->addItem(m_boot_mode);

            auto *relink = new tsl::elm::ListItem("Reconnect phone");
            relink->setClickListener([](u64 keys) {
                if (keys & HidNpadButton_A) {
                    Relink();
                    return true;
                }
                return false;
            });
            list->addItem(relink);

            frame->setContent(list);
            this->refresh();
            return frame;
        }

        virtual void update() override {
            if (++m_frame >= 30) {
                m_frame = 0;
                this->refresh();
            }
        }
    private:
        void refresh() {
            UbtStatus st;
            if (!GetStatus(&st)) {
                m_message->setValue("not running", true);
                return;
            }

            char text[96];

            m_message->setValue(st.message);

            if (!m_toggle_synced) {
                m_toggle_synced = true;
                m_bridge_toggle->setState(st.bridge_enabled != 0);
            }

            std::snprintf(text, sizeof(text), "%s %s", LinkStateName(st.link_state), st.link_state == 2 ? ProtocolName(st.link_protocol) : "");
            m_link->setValue(text, st.link_state != 2);

            if (st.phone_vid != 0) {
                std::snprintf(text, sizeof(text), "%04x:%04x", st.phone_vid, st.phone_pid);
                m_phone->setValue(text);
            } else {
                m_phone->setValue("-", true);
            }

            if (st.link_state == 2) {
                std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
                m_mac->setValue(text);
            } else {
                m_mac->setValue("-", true);
            }

            m_rx->setValue(FormatTraffic(st.rx_packets, st.rx_bytes));
            m_tx->setValue(FormatTraffic(st.tx_packets, st.tx_bytes));

            if (st.dhcp_offer_ip != 0) {
                const u8 *ip = reinterpret_cast<const u8 *>(&st.dhcp_offer_ip);
                std::snprintf(text, sizeof(text), "OK %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                m_dhcp->setValue(text);
            } else {
                m_dhcp->setValue("-", true);
            }

            m_mode->setValue(ModeName(st.mode));
            if (m_next_boot_mode < 0) {
                m_next_boot_mode = st.mode;
                m_boot_mode->setValue(ModeName(st.mode));
            }

            if (st.mode != 2) {
                m_hook->setValue("off in this mode", true);
            } else if (st.eth_session_open) {
                std::snprintf(text, sizeof(text), "Yes (%u filters)", st.eth_filter_count);
                m_hook->setValue(text);
            } else {
                m_hook->setValue("No", true);
            }

            if (!st.virt_attached) {
                m_adapter->setValue("Not shown", true);
            } else if (!st.virt_acquired) {
                std::snprintf(text, sizeof(text), "Offered %04x:%04x", st.emu_vid, st.emu_pid);
                m_adapter->setValue(text, true);
            } else if (!st.virt_rx_enabled) {
                m_adapter->setValue("Opened by driver");
            } else {
                m_adapter->setValue("In use");
            }

            if (st.unknown_requests != 0) {
                std::snprintf(text, sizeof(text), "%u (%u unknown)", st.ctrl_requests, st.unknown_requests);
            } else {
                std::snprintf(text, sizeof(text), "%u", st.ctrl_requests);
            }
            m_requests->setValue(text, st.ctrl_requests == 0);

            m_memory->setValue(MemoryModeName(st.memory_mode), st.memory_mode == 0);

            if (st.last_error != 0) {
                std::snprintf(text, sizeof(text), "0x%x", st.last_error);
                m_error->setValue(text);
            } else {
                m_error->setValue("none", true);
            }
        }
};

class UsbTetherOverlay : public tsl::Overlay {
    public:
        virtual void initServices() override {}
        virtual void exitServices() override { Disconnect(); }

        virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
            return initially<StatusGui>();
        }
};

int main(int argc, char **argv) {
    return tsl::loop<UsbTetherOverlay>(argc, argv);
}
