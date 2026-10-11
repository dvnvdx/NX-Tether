#include "ubt_config.hpp"
#include "ubt_log.hpp"

namespace ams::ubt::config {

    namespace {

        constexpr const char ConfigPath[] = "sdmc:/config/usb-tether/config.ini";

        constinit Config g_config = {
            .mode       = Mode_Bridge,
            .log_level  = log::Level_Debug,
            .dhcp_probe = true,
            .force_pid  = 0,
            .prefer_ncm = false,
            .rx_single_frame = false,
        };

        constinit char g_buffer[2048];

        bool IsSpace(char c) {
            return c == ' ' || c == '\t' || c == '\r';
        }

        long ParseNumber(const char *s) {
            return std::strtol(s, nullptr, 0);
        }

        void ApplyKeyValue(const char *key, const char *value) {
            if (std::strcmp(key, "mode") == 0) {
                const long v = ParseNumber(value);
                if (v >= Mode_Off && v <= Mode_Bridge) {
                    g_config.mode = static_cast<u8>(v);
                }
            } else if (std::strcmp(key, "log_level") == 0) {
                const long v = ParseNumber(value);
                if (v >= log::Level_Error && v <= log::Level_Trace) {
                    g_config.log_level = static_cast<int>(v);
                }
            } else if (std::strcmp(key, "dhcp_probe") == 0) {
                g_config.dhcp_probe = ParseNumber(value) != 0;
            } else if (std::strcmp(key, "force_pid") == 0) {
                g_config.force_pid = static_cast<u16>(ParseNumber(value));
            } else if (std::strcmp(key, "prefer_ncm") == 0) {
                g_config.prefer_ncm = ParseNumber(value) != 0;
            } else if (std::strcmp(key, "rx_single_frame") == 0) {
                g_config.rx_single_frame = ParseNumber(value) != 0;
            }
        }

        void Parse(char *text) {
            char *line = text;
            while (line != nullptr && *line != '\0') {
                char *next = std::strchr(line, '\n');
                if (next != nullptr) {
                    *next++ = '\0';
                }

                while (IsSpace(*line)) { ++line; }
                if (*line != '\0' && *line != ';' && *line != '#' && *line != '[') {
                    char *eq = std::strchr(line, '=');
                    if (eq != nullptr) {
                        /* Trim the key. */
                        char *key_end = eq;
                        while (key_end > line && IsSpace(key_end[-1])) { --key_end; }
                        *key_end = '\0';

                        /* Trim the value. */
                        char *value = eq + 1;
                        while (IsSpace(*value)) { ++value; }
                        char *value_end = value + std::strlen(value);
                        while (value_end > value && IsSpace(value_end[-1])) { --value_end; }
                        *value_end = '\0';

                        ApplyKeyValue(line, value);
                    }
                }

                line = next;
            }
        }

        Result WriteConfig() {
            const int len = util::SNPrintf(g_buffer, sizeof(g_buffer),
                "; usb-tether configuration\n"
                ";\n"
                "; mode: 0 = off (module idles)\n"
                ";       1 = phone link test only (nothing in the system is intercepted)\n"
                ";       2 = full bridge (virtual LAN adapter)\n"
                "; Changing the mode takes effect after a reboot.\n"
                "mode=%d\n"
                "\n"
                "; log_level: 0 = errors, 1 = info, 2 = debug (default), 3 = per-packet trace (slow)\n"
                "log_level=%d\n"
                "\n"
                "; dhcp_probe: send one DHCP request when the phone link comes up and the\n"
                "; system is not using it, to prove that data flows. 1 = on, 0 = off.\n"
                "dhcp_probe=%d\n"
                "\n"
                "; force_pid: USB product id of the ASIX adapter to imitate, e.g. 0x7720.\n"
                "; 0 = choose automatically from what the system asks for.\n"
                "force_pid=0x%04x\n"
                "\n"
                "; prefer_ncm: when the phone offers both RNDIS and NCM, use NCM.\n"
                "prefer_ncm=%d\n"
                "\n"
                "; rx_single_frame: hand the console one ethernet frame per USB transfer instead of\n"
                "; packing several. Slower; for diagnosing crashes under heavy traffic.\n"
                "rx_single_frame=%d\n",
                g_config.mode, g_config.log_level, g_config.dhcp_probe ? 1 : 0, g_config.force_pid, g_config.prefer_ncm ? 1 : 0, g_config.rx_single_frame ? 1 : 0);
            R_UNLESS(len > 0, fs::ResultInvalidArgument());

            R_DISCARD(fs::DeleteFile(ConfigPath));
            R_TRY(fs::CreateFile(ConfigPath, len));

            fs::FileHandle file;
            R_TRY(fs::OpenFile(std::addressof(file), ConfigPath, fs::OpenMode_Write));
            ON_SCOPE_EXIT { fs::CloseFile(file); };

            R_RETURN(fs::WriteFile(file, 0, g_buffer, len, fs::WriteOption::Flush));
        }

    }

    void Load() {
        fs::FileHandle file;
        if (R_FAILED(fs::OpenFile(std::addressof(file), ConfigPath, fs::OpenMode_Read))) {
            /* First boot: write the defaults so that the user has something to edit. */
            R_DISCARD(WriteConfig());
            return;
        }

        size_t read_size = 0;
        const Result rc = fs::ReadFile(std::addressof(read_size), file, 0, g_buffer, sizeof(g_buffer) - 1);
        fs::CloseFile(file);

        if (R_SUCCEEDED(rc)) {
            g_buffer[read_size] = '\0';
            Parse(g_buffer);
        }
    }

    const Config &Get() {
        return g_config;
    }

    void SetRunningMode(u8 mode) {
        g_config.mode = mode;
    }

    Result SaveMode(u8 mode) {
        R_UNLESS(mode <= Mode_Bridge, fs::ResultInvalidArgument());

        /* Only the stored value changes; the running mode stays as booted. */
        const u8 running = g_config.mode;
        g_config.mode = mode;
        const Result rc = WriteConfig();
        g_config.mode = running;
        R_RETURN(rc);
    }

}
