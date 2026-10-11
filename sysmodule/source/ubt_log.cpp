#include "ubt_log.hpp"
#include <cstdarg>

namespace ams::ubt::log {

    namespace {

        constexpr const char LogDirectory0[] = "sdmc:/config";
        constexpr const char LogDirectory1[] = "sdmc:/config/usb-tether";
        constexpr const char LogPath[]       = "sdmc:/config/usb-tether/log.txt";
        constexpr const char OldLogPath[]    = "sdmc:/config/usb-tether/log.old.txt";

        constexpr s64 MaxLogSize     = 1_MB;   /* Rotated at boot when larger than this. */
        constexpr s64 HardLogLimit   = 4_MB;   /* Stop writing entirely past this in one boot. */

        constinit os::SdkMutex g_lock;
        constinit bool g_ready = false;
        constinit int g_level = Level_Debug;
        constinit s64 g_offset = 0;
        constinit bool g_limit_reported = false;
        constinit char g_line[768];
        constinit fs::FileHandle g_file = {};

        void WriteLocked(const char *data, size_t size) {
            if (!g_ready || size == 0) {
                return;
            }
            if (g_offset + static_cast<s64>(size) > HardLogLimit) {
                if (g_limit_reported) {
                    return;
                }
                g_limit_reported = true;
                data = "*** log size limit reached, logging stopped until reboot ***\n";
                size = std::strlen(data);
            }

            /* The file stays open for the lifetime of the module; every line is flushed so that */
            /* the log survives a crash or a hard power-off. */
            if (R_SUCCEEDED(fs::WriteFile(g_file, g_offset, data, size, fs::WriteOption::Flush))) {
                g_offset += size;
            }
        }

    }

    void Initialize() {
        std::scoped_lock lk(g_lock);

        /* Directories may already exist; ignore failures. */
        R_DISCARD(fs::CreateDirectory(LogDirectory0));
        R_DISCARD(fs::CreateDirectory(LogDirectory1));

        /* Rotate an oversized log from previous boots. */
        {
            fs::FileHandle file;
            if (R_SUCCEEDED(fs::OpenFile(std::addressof(file), LogPath, fs::OpenMode_Read))) {
                s64 size = 0;
                R_DISCARD(fs::GetFileSize(std::addressof(size), file));
                fs::CloseFile(file);
                if (size > MaxLogSize) {
                    R_DISCARD(fs::DeleteFile(OldLogPath));
                    R_DISCARD(fs::RenameFile(LogPath, OldLogPath));
                    size = 0;
                }
                g_offset = size;
            }
        }

        /* Make sure the file exists. */
        R_DISCARD(fs::CreateFile(LogPath, 0));
        if (R_SUCCEEDED(fs::OpenFile(std::addressof(g_file), LogPath, fs::OpenMode_Write | fs::OpenMode_AllowAppend))) {
            s64 size = 0;
            R_DISCARD(fs::GetFileSize(std::addressof(size), g_file));
            g_offset = size;
            g_ready = true;
        }
    }

    void SetLevel(int level) {
        g_level = level;
    }

    int GetLevel() {
        return g_level;
    }

    void Print(int level, const char *fmt, ...) {
        if (level > g_level) {
            return;
        }

        std::scoped_lock lk(g_lock);

        const s64 ms = os::ConvertToTimeSpan(os::GetSystemTick()).GetMilliSeconds();
        int pos = util::SNPrintf(g_line, sizeof(g_line), "[%6lld.%03lld] ", static_cast<long long>(ms / 1000), static_cast<long long>(ms % 1000));
        if (pos < 0) {
            pos = 0;
        }

        std::va_list vl;
        va_start(vl, fmt);
        const int len = util::VSNPrintf(g_line + pos, sizeof(g_line) - pos - 2, fmt, vl);
        va_end(vl);

        size_t total = pos;
        if (len > 0) {
            total += std::min<size_t>(static_cast<size_t>(len), sizeof(g_line) - pos - 3);
        }
        g_line[total++] = '\n';

        WriteLocked(g_line, total);
    }

    void Hex(int level, const char *prefix, const void *data, size_t size, size_t max_size) {
        if (level > g_level) {
            return;
        }

        static constexpr char Digits[] = "0123456789abcdef";
        char text[3 * 64 + 8];
        const size_t shown = std::min<size_t>(std::min(size, max_size), 64);
        const u8 *bytes = static_cast<const u8 *>(data);

        size_t pos = 0;
        for (size_t i = 0; i < shown; ++i) {
            text[pos++] = Digits[bytes[i] >> 4];
            text[pos++] = Digits[bytes[i] & 0xF];
            text[pos++] = ' ';
        }
        if (shown < size) {
            text[pos++] = '.';
            text[pos++] = '.';
        }
        text[pos] = '\0';

        Print(level, "%s (%zu bytes): %s", prefix, size, text);
    }

}
