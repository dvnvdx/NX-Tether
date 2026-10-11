#pragma once
#include "ubt_common.hpp"

namespace ams::ubt::log {

    enum Level : int {
        Level_Error = 0,
        Level_Info  = 1,
        Level_Debug = 2, /* Per-request detail. */
        Level_Trace = 3, /* Per-packet detail. Very slow; for short captures only. */
    };

    void Initialize();
    void SetLevel(int level);
    int  GetLevel();

    void Print(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    void Hex(int level, const char *prefix, const void *data, size_t size, size_t max_size = 64);

}

#define UBT_LOG_E(...) ::ams::ubt::log::Print(::ams::ubt::log::Level_Error, __VA_ARGS__)
#define UBT_LOG_I(...) ::ams::ubt::log::Print(::ams::ubt::log::Level_Info,  __VA_ARGS__)
#define UBT_LOG_D(...) ::ams::ubt::log::Print(::ams::ubt::log::Level_Debug, __VA_ARGS__)
#define UBT_LOG_T(...) ::ams::ubt::log::Print(::ams::ubt::log::Level_Trace, __VA_ARGS__)
