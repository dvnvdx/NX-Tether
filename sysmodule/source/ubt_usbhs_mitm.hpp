#pragma once
#include "ubt_common.hpp"

namespace ams::ubt::mitm {

    /* Starts the IPC server thread: the usb:hs interception and the status service. */
    void Launch();

    /* Device filters the intercepted client (eth) registered. Returns the number written. */
    size_t GetClientFilters(DeviceFilter *out, size_t max_count);

    /* Called by the virtual adapter after it appeared or disappeared. */
    void NotifyVirtualAdapterChanged(bool attached);

    void FillStatus(Status *status);

}
