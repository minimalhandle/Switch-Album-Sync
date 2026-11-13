#pragma once

#include "nso_album_sync/config.hpp"

#include <chrono>
#include <string>

namespace nso {

// True when `hour` (0-23, local time) falls in the quiet period. A period such
// as 23 to 7 wraps past midnight; equal start and end hours mean no quiet hours.
inline bool in_quiet_hours(int start_hour, int end_hour, int hour) {
    if (start_hour == end_hour) return false;
    return start_hour < end_hour
        ? hour >= start_hour && hour < end_hour
        : hour >= start_hour || hour < end_hour;
}

// Why an automatic sync should be skipped right now, or an empty string to
// go ahead. Skipped syncs simply wait for the next scheduled check, so these
// options only ever reduce how often the app contacts Nintendo.
inline std::string auto_sync_pause_reason(
    const AppConfig& config, bool battery_saver, bool metered, int local_hour) {
    if (config.pause_on_battery_saver && battery_saver) return "battery saver is on";
    if (config.pause_on_metered && metered) return "the network connection is metered";
    if (config.quiet_hours && in_quiet_hours(config.quiet_start_hour, config.quiet_end_hour, local_hour)) {
        return "it's within quiet hours";
    }
    return {};
}

// Local system state used by the policy above (Windows).
bool battery_saver_on();
bool metered_connection();
int local_hour_now();
// How long since the last keyboard or mouse input on this PC.
std::chrono::milliseconds user_idle_time();

}  // namespace nso
