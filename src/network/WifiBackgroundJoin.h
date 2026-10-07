#pragma once

// Joins the last-used saved Wi-Fi network on a short-lived task with no
// screen, the way the OPDS browser preloads its root page's feeds in the
// background: the OPDS server list starts it on entry so the browser finds
// the link up when a server is picked. Main task only.
namespace wifi_background_join {
// Starts the join unless the radio is already up (connected, joining, or the
// Goodies remote's link), or internal RAM is below the Wi-Fi entry gate. The
// task reads wifi.json, brings the driver up and calls WiFi.begin(); a missing
// saved network ends it quietly.
void start();
// Internal RAM clears the Wi-Fi entry gate (or the radio is already up); logs
// the skip when it does not. start() checks it too.
bool heapAllows();
// Radio off on a task, for the screen that called start() when it leaves for a
// screen without Wi-Fi. Leaves the link alone when the Goodies remote wants it.
void stop();
// Blocks until a join or teardown task is done: before a Wi-Fi screen's
// onEnter() and before deep sleep, so no Wi-Fi call overlaps the task's.
void wait();
// A join started here is running or has begun and is neither connected nor
// timed out yet: the browser waits for it instead of restarting the join.
bool joining();
}  // namespace wifi_background_join
