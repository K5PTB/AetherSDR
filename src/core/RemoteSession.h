#pragma once

#include "core/RadioDiscovery.h"
#include "core/TailnetAddress.h"

namespace AetherSDR {

// A session is remote unless it is a direct local-network connection:
// SmartLink (WAN), a manual-IP/routed connection, or a radio reached at a
// tailnet address (discovered via the shim's forwarded announcement). One
// definition, so the Radio Setup IP gate, the network-quality thresholds, the
// NTP gate, the discovery socket and the low-bandwidth default cannot drift.
inline bool isRemoteRadioSession(bool wan, const RadioInfo& info)
{
    return wan || info.isRouted || isTailnetAddress(info.address);
}

}  // namespace AetherSDR
