#include "rtc.hpp"

#include "build.hpp"

#include <cstdint>
#include <algorithm>
#include <ctime>

#include "guest.hpp"
#include "system.hpp"

namespace shinobi {


namespace rt = qilin::rt;

namespace {

// Shinobi's flags (0x8C117DF4): bit 0 asks the next read to read the
// hardware, bit 1 a write to it. Only Shinobi's own functions set them, and
// none of those is reached: anything set is new and stops.
constexpr uint32_t kFlags = kBuild.shinobi_data + 0x38u;
void check_flags(uint32_t entry) {
    if (const uint32_t f = rt::ld32(kFlags)) throw rt::Stop{rt::Stop::Unsupported, entry, f};
}

struct Date {
    uint32_t year, month, day, hour, minute, second;
};

Date host_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return {static_cast<uint32_t>(tm.tm_year + 1900), static_cast<uint32_t>(tm.tm_mon + 1), static_cast<uint32_t>(tm.tm_mday),
            static_cast<uint32_t>(tm.tm_hour), static_cast<uint32_t>(tm.tm_min), static_cast<uint32_t>(std::min(tm.tm_sec, 59))};
}

// 0x8C0B439C's count: seconds since 1950, with a leap year every fourth
// (right from 1901 to 2099, as the clock's range is).
uint32_t seconds_since_1950(const Date& d) {
    static constexpr uint32_t kBefore[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const uint32_t years = d.year - 1950;
    uint32_t days = years * 365 + (years + 2) / 4;
    if ((years + 2) % 4 == 0 && d.month <= 2) --days;
    days += kBefore[d.month - 1] + d.day - 1;
    return ((days * 24 + d.hour) * 60 + d.minute) * 60 + d.second;
}

// 0x8C0B4744: the day of the week, 0 for Sunday (a Zeller congruence without
// the century terms).
uint32_t day_of_week(uint32_t year, uint32_t month, uint32_t day) {
    if (month <= 2) month += 12, --year;
    return (year + year / 4 + (13 * month + 8) / 5 + day - 15) % 7;
}

// 0x8C0B4780: the moon's age in days, from a new moon at 1555306449 seconds
// and a month of 2551423 seconds.
uint32_t moon_age(uint32_t seconds) {
    constexpr uint32_t kNewMoon = 0x5CB417D1u, kMonth = 2551423u;
    const uint32_t into = seconds >= kNewMoon ? (seconds - kNewMoon) % kMonth : kMonth - (kNewMoon - seconds) % kMonth;
    return into / 86400u;
}

// 0x8C0B427C: brings the kept count up to date when the flags ask: with
// none set, nothing.
void shinobi_rtc_refresh(rt::Context&) { check_flags(kBuild.natives.rtc_refresh); }

// 0x8C0B42A6: the date into r4: +0 the year (16 bits), +2 the month, +3 the
// day, +4 the hour, +5 the minute, +6 the second, +7 the day of the week, +8
// the moon's age. The count the date comes from is kept at 0x8C117DF0, as the
// guest code keeps it. Returns the flags.
void shinobi_rtc_get_date(rt::Context& c) {
    check_flags(kBuild.natives.rtc_get_date);
    const Date d = host_now();
    const uint32_t to = c.r[4], seconds = seconds_since_1950(d);
    rt::st32(kBuild.shinobi_data + 0x34u, seconds);
    rt::st16(to, d.year);
    rt::st8(to + 2, d.month), rt::st8(to + 3, d.day), rt::st8(to + 4, d.hour), rt::st8(to + 5, d.minute), rt::st8(to + 6, d.second);
    rt::st8(to + 7, day_of_week(d.year, d.month, d.day));
    rt::st8(to + 8, moon_age(seconds));
    c.r[0] = 0;
}

// Toy Racer's 0x8C0C6088: a date at r4 (as rtc_get_date writes one) into
// the clock's count at r5, seconds since 1950, as the code computes it: 365
// days a year plus a leap day every fourth from 1950 + 2 (one less in January
// and February of a leap year), the days before the month from the table at
// kBuild.rtc_month_days (copied to the stack first), then hours, minutes and
// seconds, all in 32 bits. The game calls it. r0 ends as the seconds byte.
// (Toy Commander's lifted code never reaches its own.)
void shinobi_rtc_date_to_count(rt::Context& c) {
    const uint32_t at = c.r[4];
    const uint32_t years = rt::ld16(at) + 0xF862u, month = rt::ld8(at + 2);
    const uint32_t shifted = years + 2;
    uint32_t days = years * 0x16Du + (shifted >> 2);
    if ((shifted & 3u) == 0 && month <= 2) --days;
    days += rt::ld32(kBuild.rtc_month_days + 4 * (month - 1)) + rt::ld8(at + 3) - 1;
    const uint32_t second = rt::ld8(at + 6);
    const uint32_t count = ((days * 24 + rt::ld8(at + 4)) * 60 + rt::ld8(at + 5)) * 60 + second;
    rt::st32(c.r[5], count);
    c.r[0] = second, c.r[6] = count, c.r[7] = 60;
}

constexpr rt::Provided kNatives[] = {
    {kBuild.natives.rtc_refresh, "shinobi_rtc_refresh", shinobi_rtc_refresh},
    {kBuild.natives.rtc_get_date, "shinobi_rtc_get_date", shinobi_rtc_get_date},
};
constexpr rt::Provided kDateToCount[] = {
    {kBuild.natives.rtc_date_to_count, "shinobi_rtc_date_to_count", shinobi_rtc_date_to_count},
};
static_assert((kBuild.natives.rtc_date_to_count == 0) == (kBuild.rtc_month_days == 0), "rtc_date_to_count needs its month table");

}  // namespace

void provide_rtc() {
    rt::provide(kNatives);
    if constexpr (kBuild.natives.rtc_date_to_count != 0) rt::provide(kDateToCount);
}

}  // namespace shinobi
