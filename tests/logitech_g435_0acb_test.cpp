// Checks the 0ACB dongle's reply parsing against buffers captured from real hardware
// (deddroid/logitech-g435-battery, docs/PROTOCOL.md).
#include "../src/drivers/logitech_g435_0acb.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> Hex(const char* s) {
    std::vector<std::uint8_t> out;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        out.push_back((std::uint8_t)(nibble(s[i]) * 16 + nibble(s[i + 1])));
    }
    return out;
}

}  // namespace

int main() {
    // Battery reply with the headset in remote mode 1: 0x1032 = 4146 mV.
    auto ok = Hex("06504901c875480d00070006820032100000ffffff");
    assert(G435Acb::ParseBatteryRaw(ok.data(), ok.size()) == 4146);
    assert(G435Acb::ParseBatteryMv(ok.data(), ok.size()) == 4146);
    assert(G435Acb::Percent(4146) == 95);

    // The same reply in remote mode 0: the dongle answers itself with 210 mV.
    auto wrong = Hex("06504901c0d64300000700068200d2000000ffff");
    assert(G435Acb::ParseBatteryRaw(wrong.data(), wrong.size()) == 210);
    assert(G435Acb::ParseBatteryMv(wrong.data(), wrong.size()) == -1);

    // Wireless-category values: connect status 1 / 0, remote mode 1.
    auto linked = Hex("0650490ec7000000000200e301ffff");
    assert(G435Acb::ParseRemoteValue(linked.data(), linked.size(), 0xE3) == 1);
    auto unlinked = Hex("0650490ec7000000000200e300ffff");
    assert(G435Acb::ParseRemoteValue(unlinked.data(), unlinked.size(), 0xE3) == 0);
    auto mode = Hex("0650490ec7000000000200e001ffff");
    assert(G435Acb::ParseRemoteValue(mode.data(), mode.size(), 0xE0) == 1);
    assert(G435Acb::ParseRemoteValue(mode.data(), mode.size(), 0xE3) == -1);

    // An empty feature report (all 0xFF after the report id) has no reply in it.
    std::vector<std::uint8_t> empty(64, 0xFF);
    empty[0] = 0x06;
    assert(G435Acb::ParseBatteryRaw(empty.data(), empty.size()) == -1);
    assert(G435Acb::ParseRemoteValue(empty.data(), empty.size(), 0xE3) == -1);

    // Voltage curve is shared with the 0ADF dongle (same headset).
    assert(G435Acb::Percent(4300) == 100);
    assert(G435Acb::Percent(3725) == 40);
    assert(G435Acb::Percent(3300) == 0);

    std::puts("logitech_g435_0acb_test: ok");
    return 0;
}