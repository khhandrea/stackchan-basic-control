// BM8563 real-time clock (PCF8563 compatible). Methods: time.get, time.set.
// The system clock is set from the RTC at boot and on time.set.
#pragma once

namespace rtc {

void init();
void registerMethods();

}  // namespace rtc
