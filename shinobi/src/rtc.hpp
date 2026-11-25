// Shinobi's real-time clock (syRtc) over the host's clock: the console's
// clock is the AICA's, a count of seconds since 1950-01-01 00:00 local time,
// which Shinobi turns into a date.
#pragma once

namespace shinobi {

// Registers the clock's natives.
void provide_rtc();

}  // namespace shinobi
