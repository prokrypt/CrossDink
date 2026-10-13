#pragma once

#include <string>

#include "util/ScreenshotInfo.h"

class Activity;

// On-device bug reports: the pull-down's bug button snaps the screen, asks for
// a description, and saves /bugreports/<id>/{report.txt,log.txt,screen.bmp}.
// The log goes through BugReportSanitize first. Reports are POSTed to the URL
// in /bugreports/url.txt whenever Wi-Fi is up for another reason; a sent report
// gets a .sent marker and stays on the card.
namespace BugReport {

constexpr const char* DIR = "/bugreports";
constexpr const char* URL_PATH = "/bugreports/url.txt";
constexpr size_t MAX_TEXT = 500;

struct Context {
  char id[24] = {};
  char activity[32] = {};  // the screen the report was started on
  ScreenshotInfo book;     // its book, when it is a reader
  bool hasScreenshot = false;
};

// Main task, from the pull-down's result: redraws the screen the panel covered,
// saves it as the screenshot, then opens the keyboard over `source`.
void begin(Activity& source);
// Main task: writes report.txt and the sanitized log.txt, deletes the
// screenshot when not wanted, and queues the report for sending.
bool save(const Context& ctx, const std::string& text, bool includeScreenshot, bool includeLog);
// Main task: removes a report the user cancelled.
void discard(const Context& ctx);
// Main loop: sends queued reports on a worker task while Wi-Fi is connected.
// Never brings Wi-Fi up and checks at most every few seconds.
void loop();
// Deep sleep: ends a running send (between file chunks) and waits for it.
void stopForSleep();

}  // namespace BugReport
