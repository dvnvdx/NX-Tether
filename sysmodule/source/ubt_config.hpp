#pragma once
#include "ubt_common.hpp"

namespace ams::ubt::config {

    struct Config {
        u8   mode;            /* ubt::Mode */
        int  log_level;       /* ubt::log::Level */
        bool dhcp_probe;      /* Send one DHCP DISCOVER when the link comes up and nobody uses it. */
        u16  force_pid;       /* 0 = pick from eth's filters; otherwise emulate this ASIX product id. */
        bool prefer_ncm;      /* If the phone offers both, pick NCM over RNDIS. */
        u8   rx_urbs;         /* Receive transfers kept queued with the phone (1-4). */
        bool tx_aggregate;    /* RNDIS: send several queued frames in one transfer to the phone. */
        bool rx_single_frame; /* Give the LAN driver one frame per bulk IN transfer instead of packing several. */
    };

    void Load();
    const Config &Get();

    /* Persist a new mode for the next boot. */
    Result SaveMode(u8 mode);

    /* Change the mode this boot runs in (used by the crash guard). Only valid during startup. */
    void SetRunningMode(u8 mode);

}
