#pragma once
// Pure (I/O-free) parts of the Logitech G435 protocol on the older 046D:0ACB dongle.
// The radio SoC speaks the same "PA"/"PI" command layer as on the 0ADF dongle, but the USB side
// is a Realtek ALC4021 bridge: commands go out as a 514-byte output report and replies come back
// through feature report 6. Kept independent of Win32 so the parsing can be tested without the
// headset. Protocol: deddroid/logitech-g435-battery (docs/PROTOCOL.md), from endes' research.
#include <cstddef>
#include <cstdint>

#include "logitech_g435_0adf.h"

namespace G435Acb {

constexpr int kMinMv = 2500;
constexpr int kMaxMv = 4600;

// A GET_BATTERY reply: 07 00 06 <seq | 0x80> 00 <mV lo> <mV hi>. Returns the raw millivolts
// (which may be the dongle's own meaningless answer in the wrong remote mode), or -1.
inline int ParseBatteryRaw(const std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i + 7 <= size; i++) {
        if (data[i] == 0x07 && data[i + 1] == 0x00 && data[i + 2] == 0x06 && (data[i + 3] & 0x80) && data[i + 4] == 0x00)
            return data[i + 5] | (data[i + 6] << 8);
    }
    return -1;
}

// The reply above, only when it is a plausible Li-Po voltage. In remote mode 0 the dongle answers
// GET_BATTERY itself with small values (105, 210), so anything outside the range means "ask again
// after switching to mode 1".
inline int ParseBatteryMv(const std::uint8_t* data, std::size_t size) {
    int mv = ParseBatteryRaw(data, size);
    return mv >= kMinMv && mv <= kMaxMv ? mv : -1;
}

// Value of a wireless-category "get" reply: 02 00 <cmd> <value>. Returns -1 when absent.
inline int ParseRemoteValue(const std::uint8_t* data, std::size_t size, std::uint8_t cmd) {
    for (std::size_t i = 0; i + 4 <= size; i++)
        if (data[i] == 0x02 && data[i + 1] == 0x00 && data[i + 2] == cmd) return data[i + 3];
    return -1;
}

// The 0ACB and 0ADF dongles reach the same headset, so the voltage curve is the same.
inline int Percent(int mv) { return G435Adf::Percent(mv); }

}  // namespace G435Acb