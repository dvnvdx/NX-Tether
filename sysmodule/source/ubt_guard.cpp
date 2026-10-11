#include "ubt_guard.hpp"
#include "ubt_log.hpp"
#include <atomic>

namespace ams::ubt::guard {

    namespace {

        constexpr const char MarkerPath[] = "sdmc:/config/usb-tether/guard.tmp";
        constexpr s64 GuardPeriodMs = 30'000;

        constinit os::SdkMutex g_lock;
        constinit bool g_tripped = false;
        constinit bool g_armed = false;
        constinit s64 g_armed_at_ms = 0;

        s64 NowMs() {
            return os::ConvertToTimeSpan(os::GetSystemTick()).GetMilliSeconds();
        }

        bool MarkerExists() {
            fs::FileHandle file;
            if (R_FAILED(fs::OpenFile(std::addressof(file), MarkerPath, fs::OpenMode_Read))) {
                return false;
            }
            fs::CloseFile(file);
            return true;
        }

    }

    bool CheckAtStartup() {
        std::scoped_lock lk(g_lock);
        g_tripped = MarkerExists();
        if (g_tripped) {
            R_DISCARD(fs::DeleteFile(MarkerPath));
        }
        return g_tripped;
    }

    bool WasTripped() {
        return g_tripped;
    }

    void Arm() {
        std::scoped_lock lk(g_lock);
        if (!g_armed) {
            R_DISCARD(fs::CreateFile(MarkerPath, 0));
            g_armed = true;
        }
        g_armed_at_ms = NowMs();
    }

    void Poll() {
        std::scoped_lock lk(g_lock);
        if (g_armed && NowMs() - g_armed_at_ms >= GuardPeriodMs) {
            R_DISCARD(fs::DeleteFile(MarkerPath));
            g_armed = false;
            UBT_LOG_D("guard: stable for %lld s, marker cleared", static_cast<long long>(GuardPeriodMs / 1000));
        }
    }

}
