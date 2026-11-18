// Logitech G435 LIGHTSPEED headset on the older 046D:0ACB dongle. The dongle is a Realtek
// ALC4021 audio chip with a PAU1823 radio SoC behind it; the headset does not speak HID++.
// Report 6 (usage page 0xFFC0) is the bridge: a 514-byte output report carries
//   enter/leave bootloader: 06 01 03 / 06 08 03
//   run a SoC command:      06 04 03 <len lo> <len hi> <PA ...>
// and the reply to each bridge message is an input report 06 5a (or 06 fe when already there).
// SoC replies are then read with GET_FEATURE on report 6.
//
// A read stops audio and the microphone for about 0.6 s, and every command inside the bootloader
// costs time, so the battery is cached for 10 minutes instead of being read on every poll.
// GET_BATTERY must never be the first command of a session: sent first it has left the dongle
// returning no data until replugged, so REMOTE_GET_CONNECT_STATUS always goes first.
//
// Protocol: github.com/deddroid/logitech-g435-battery (docs/PROTOCOL.md), from endes' research.
#include "drivers.h"
#include "logitech_g435_0acb.h"

namespace {

constexpr USHORT kVid = 0x046D;
constexpr USHORT kPid = 0x0ACB;
constexpr BYTE kReport = 6;
constexpr BYTE kAckOk = 0x5A;
constexpr BYTE kAckAlready = 0xFE;
constexpr BYTE kCatOta = 0x06;
constexpr BYTE kCatWireless = 0x0E;
constexpr BYTE kGetBattery = 0x31;
constexpr BYTE kGetConnectStatus = 0xE3;
constexpr BYTE kSetMode = 0xE1;

// A full read interrupts audio, so it runs at most this often. When the headset is off there is
// no audio to interrupt and a shorter retry makes switching it on show up quickly.
constexpr ULONGLONG kReadEveryMs = 10ull * 60 * 1000;
constexpr ULONGLONG kRetryOffMs = 2ull * 60 * 1000;

struct Reading {
    bool valid = false;
    ULONGLONG at = 0;
    int mv = -1;   // last plausible voltage
    bool off = false;
};
Reading g_cache;

// Sends one bridge message and waits for its acknowledgement on the input endpoint, skipping
// reports that are not ours (volume keys arrive as report 3). Returns the ack byte, 0 on no reply.
BYTE Bridge(HidDev& d, std::initializer_list<BYTE> op, const std::vector<BYTE>& body) {
    std::vector<BYTE> out(d.coll().outLen(), 0);
    if (out.size() < 3 + body.size()) return 0;
    out[0] = kReport;
    size_t at = 1;
    for (BYTE b : op) out[at++] = b;
    memcpy(out.data() + at, body.data(), body.size());
    d.Flush();
    if (!d.Write(out)) return 0;
    ULONGLONG end = GetTickCount64() + 800;
    for (;;) {
        LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
        if (left <= 0) return 0;
        std::vector<BYTE> r;
        if (d.Read(r, (int)left) < 0) continue;
        if (r.size() >= 2 && r[0] == kReport) return r[1];
    }
}

// Leaves the bridge bootloader (and with it resumes audio) once a session was opened.
struct SessionGuard {
    HidDev& d;
    bool open = false;
    ~SessionGuard() {
        if (open) Bridge(d, { 0x08, 0x03 }, {});
    }
};

// Commands run inside the session; the sequence number advances for every one of them.
struct Session {
    HidDev& d;
    BYTE seq = 1;
    bool connected = true;

    bool Query(BYTE category, BYTE len, std::initializer_list<BYTE> payload, int& value,
               int (*parse)(const std::uint8_t*, std::size_t, std::uint8_t), BYTE cmd) {
        std::vector<BYTE> pa = { 0x50, 0x41, category, seq, len };
        pa.insert(pa.end(), payload);
        seq = (BYTE)(seq % 127 + 1);
        std::vector<BYTE> body = { (BYTE)(pa.size() & 0xFF), (BYTE)(pa.size() >> 8) };
        body.insert(body.end(), pa.begin(), pa.end());
        if (Bridge(d, { 0x04, 0x03 }, body) == 0) return false;

        ULONGLONG end = GetTickCount64() + 1500;
        while (GetTickCount64() < end) {
            std::vector<BYTE> r;
            if (d.GetFeature(kReport, r) && r.size() > 1 && r[1] != 0xFF) {
                value = parse(r.data(), r.size(), cmd);
                if (value >= 0) return true;
                continue;
            }
            Sleep(20);
        }
        return false;
    }

    // GET_BATTERY, whatever value the reply carries (-1 when there is none).
    int BatteryRaw() {
        int mv = -1;
        if (!Query(kCatOta, 1, { 0x00, kGetBattery }, mv, [](const std::uint8_t* p, std::size_t n, std::uint8_t) {
                return G435Acb::ParseBatteryRaw(p, n);
            }, 0))
            return -1;
        return mv;
    }
};

int ReadMillivolts(HidDev& d, bool& off) {
    off = false;
    BYTE ack = Bridge(d, { 0x01, 0x03 }, {});
    if (ack != kAckOk && ack != kAckAlready) return -1;

    SessionGuard guard{ d, true };
    // Discard whatever the dongle queued before this session (the first reply after entering the
    // bootloader can also be missing its first two bytes).
    for (int i = 0; i < 16; i++) {
        std::vector<BYTE> r;
        if (!d.GetFeature(kReport, r) || r.size() < 2 || r[1] == 0xFF) break;
    }

    Session s{ d };
    int connected = -1;
    if (s.Query(kCatWireless, 1, { kGetConnectStatus }, connected, G435Acb::ParseRemoteValue, kGetConnectStatus) && connected == 0) {
        off = true;
        return -1;
    }

    int mv = s.BatteryRaw();
    if (mv >= G435Acb::kMinMv && mv <= G435Acb::kMaxMv) return mv;

    // Remote mode 0 makes the dongle answer GET_BATTERY itself; mode 1 routes it to the headset.
    int mode = -1;
    s.Query(kCatWireless, 2, { kSetMode, 0x01 }, mode, [](const std::uint8_t*, std::size_t, std::uint8_t) { return 0; }, 0);
    mv = s.BatteryRaw();
    return mv >= G435Acb::kMinMv && mv <= G435Acb::kMaxMv ? mv : -1;
}

}  // namespace

void PollLogitechG435Acb(PollContext& ctx) {
    if (ctx.IsClaimed(kVid, kPid)) return;
    auto colls = ctx.Find(kVid, kPid, [](const HidColl& c) {
        return c.page() == 0xFFC0 && c.usage() == 0x0001 && c.outLen() > 8 && c.featLen() > 4;
    });
    if (colls.empty()) return;

    DeviceStatus s;
    s.key = L"g435:" + Hex4(kPid);
    s.name = L"Logitech G435";
    s.kind = DeviceKind::Headset;
    s.via = L"2.4 GHz receiver";

    ULONGLONG now = GetTickCount64();
    bool fresh = g_cache.valid && now - g_cache.at < (g_cache.off ? kRetryOffMs : kReadEveryMs);
    if (!fresh) {
        HidDev d;
        bool read = false;
        if (d.Open(*colls[0])) {
            bool off = false;
            int mv = ReadMillivolts(d, off);
            g_cache.at = now;
            if (mv >= 0) {
                g_cache = { true, now, mv, false };
                read = true;
            } else if (off) {
                g_cache.valid = true;
                g_cache.off = true;
            }
        }
        if (!read && !g_cache.valid) {
            // No reading and never had one: report the receiver so the menu can show the headset,
            // but leave it unconnected (hidden) until a read succeeds.
            s.connected = false;
            ctx.Add(s, kVid, kPid);
            return;
        }
    }

    if (g_cache.off) {
        s.connected = false;
        ctx.Add(s, kVid, kPid);
        return;
    }
    s.millivolts = g_cache.mv;
    s.percent = G435Acb::Percent(g_cache.mv);
    ctx.Add(s, kVid, kPid);
}