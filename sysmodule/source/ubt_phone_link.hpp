#pragma once
#include "ubt_common.hpp"

namespace ams::ubt::link {

    /* Starts the threads that find the phone on the USB bus and run the tethering link. */
    void Launch();

    /* Queue an ethernet frame for transmission to the phone. Returns false if it was dropped. */
    bool SendFrame(const u8 *frame, size_t length);

    void SetBridgeEnabled(bool enabled);
    void RequestRelink();

    void FillStatus(Status *status);

}
