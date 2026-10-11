#pragma once
#include "ubt_common.hpp"

/* Crash guard. A marker file exists on the SD card while the module is doing something that  */
/* could take the system down (intercepting eth at boot, presenting the virtual adapter). If  */
/* the marker is still there at the next boot, that attempt did not survive, and the module   */
/* drops back to link-test mode by itself so that the console boots normally again.           */
namespace ams::ubt::guard {

    /* Call once at startup, before anything risky. Returns true if the last run tripped it. */
    bool CheckAtStartup();
    bool WasTripped();

    /* Start (or restart) a guarded period. */
    void Arm();

    /* Call regularly; removes the marker once the guarded period has passed quietly. */
    void Poll();

}
