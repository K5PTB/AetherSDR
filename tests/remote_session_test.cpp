#include "core/RemoteSession.h"

#include <iostream>

using namespace AetherSDR;

int main()
{
    bool ok = true;
    const auto check = [&ok](bool condition, const char* message) {
        if (!condition) {
            std::cerr << "FAIL: " << message << '\n';
            ok = false;
        }
    };
    const auto info = [](const char* address, bool routed) {
        RadioInfo radio;
        radio.address = QHostAddress(QString::fromLatin1(address));
        radio.isRouted = routed;
        return radio;
    };

    check(!isRemoteRadioSession(false, info("10.0.0.5", false)),
          "a LAN radio on a direct connection is local");
    check(!isRemoteRadioSession(false, info("192.168.1.20", false)),
          "a 192.168 radio on a direct connection is local");
    check(isRemoteRadioSession(true, info("10.0.0.5", false)),
          "SmartLink (WAN) is remote");
    check(isRemoteRadioSession(false, info("10.0.0.5", true)),
          "a routed / manual-IP connection is remote");
    check(isRemoteRadioSession(false, info("100.64.1.2", false)),
          "a radio discovered at a tailnet IPv4 address is remote");
    check(isRemoteRadioSession(false, info("fd7a:115c:a1e0::1234", false)),
          "a radio discovered at a tailnet IPv6 address is remote");
    check(!isRemoteRadioSession(false, RadioInfo{}),
          "an unset address is not tailnet");

    return ok ? 0 : 1;
}
