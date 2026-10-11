#pragma once
#include "ubt_common.hpp"

namespace ams::ubt::config {

    struct Config {
        u8   mode;            /* ubt::Mode */
        int  log_level;       /* ubt::log::Level */
        bool dhcp_probe;      /* Send one DHCP DISCOVER when the link comes up and nobody uses it. */
        u16  force_pid;       /* 0 = pick from eth's filters; otherwise emulate this ASIX product id. */
        bool prefer_ncm;      /* If the phone offers both, pick NCM over RNDIS. */
    };

    void Load();
    const Config &Get();

    /* Persist a new mode for the next boot. */
    Result SaveMode(u8 mode);

    /* Change the mode this boot runs in (used by the crash guard). Only valid during startup. */
    void SetRunningMode(u8 mode);

}
