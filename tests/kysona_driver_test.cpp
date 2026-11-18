// VGN DragonFly F2 Ultra+ on the Kysona protocol: receiver 3554:FB49, USB cable 3554:FB3D.
// Checks the cable label and that the dongle and the cable are shown as one device, which is
// what used to produce "the same mouse twice" after plugging the charging cable in or out.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>
#include <map>

namespace {
std::map<BYTE, std::vector<BYTE>> answers;
std::vector<std::vector<BYTE>> pending;
int opens = 0;

BYTE Checksum(const std::vector<BYTE>& b) {
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) sum += b[i];
    return (BYTE)(0x55 - sum);
}

HidColl Collection(USHORT pid) {
    HidColl c;
    c.vid = 0x3554;
    c.pid = pid;
    c.caps.UsagePage = 0xFF02;
    c.caps.Usage = 0x0002;
    c.caps.InputReportByteLength = 17;
    c.caps.OutputReportByteLength = 17;
    return c;
}

std::vector<BYTE> Reply(BYTE cmd, BYTE percent, bool charging) {
    std::vector<BYTE> r(17, 0);
    r[0] = 0x08;
    r[1] = cmd;
    r[6] = cmd == 0x03 ? 1 : percent;  // online flag, then the level
    r[7] = charging ? 1 : 0;
    r[8] = 0x0E;
    r[9] = 0x61;  // 3681 mV
    r[16] = Checksum(r);
    return r;
}

void Answer(BYTE percent, bool charging) {
    answers.clear();
    pending.clear();
    answers[0x03] = Reply(0x03, 0, false);
    answers[0x04] = Reply(0x04, percent, charging);
}

PollContext Poll(std::vector<HidColl> colls) {
    PollContext ctx;
    ctx.colls = std::move(colls);
    PollKysona(ctx);
    return ctx;
}
}  // namespace

bool HidDev::Open(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    ++opens;
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
void HidDev::Flush() { pending.clear(); }
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    assert(data.size() == 17 && data[0] == 0x08 && data[16] == Checksum(data));
    auto it = answers.find(data[1]);
    if (it != answers.end()) pending.push_back(it->second);
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int) {
    if (pending.empty()) return -1;
    out = pending.front();
    pending.erase(pending.begin());
    return (int)out.size();
}
std::wstring HidProduct(const HidColl&) { return L"VGN DragonFly F2 Ultra+"; }
std::wstring Hex4(USHORT value) {
    wchar_t s[8];
    swprintf(s, 8, L"%04X", value);
    return s;
}
DeviceKind KindFromCollections(const std::vector<HidColl>&, USHORT, USHORT) { return DeviceKind::Other; }
const wchar_t* GWolvesModelName(USHORT) { return L"G-Wolves mouse"; }
bool GWolvesIsCable(USHORT) { return false; }

int main() {
    // The receiver alone.
    Answer(30, false);
    auto ctx = Poll({ Collection(0xFB49) });
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].name == L"VGN DragonFly F2 Ultra+" && ctx.out[0].kind == DeviceKind::Mouse);
    assert(ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 30 && !ctx.out[0].charging && !ctx.out[0].asleep);
    assert(ctx.out[0].key == L"kysona:3554:FB49");

    // The mouse on its USB cable.
    Answer(95, true);
    ctx = Poll({ Collection(0xFB3D) });
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].via == L"USB cable");
    assert(ctx.out[0].percent == 95 && ctx.out[0].charging);
    assert(ctx.out[0].key == L"kysona:3554:FB3D");

    // Both plugged in: one entry, the cable's live charging reading wins.
    Answer(95, true);
    ctx = Poll({ Collection(0xFB3D), Collection(0xFB49) });
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].via == L"USB cable");
    assert(ctx.out[0].percent == 95 && ctx.out[0].charging);
    assert(ctx.IsClaimed(0x3554, 0xFB3D) && ctx.IsClaimed(0x3554, 0xFB49));

    // The same receiver read twice by a later poll does not add a second entry.
    int before = opens;
    PollKysona(ctx);
    assert(opens == before && ctx.out.size() == 1);

    puts("Kysona driver tests passed");
}
