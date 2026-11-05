// Goodies > Pin Monitor as an SD module: the firmware row's page, same data.
// Confirm or a tap on the first line switches the monitor; any other key or tap redraws.
#include "goody.h"

static char* put(char* p, const char* s) {
  while (*s) *p++ = *s++;
  return p;
}

static char* putu(char* p, unsigned v) {
  char tmp[10];
  int n = 0;
  do tmp[n++] = (char)('0' + v % 10);
  while ((v /= 10) != 0);
  while (n) *p++ = tmp[--n];
  return p;
}

GD_EXPORT(gd_render) void gd_render(void) {
  const int lh = gd_line_height() + 6;
  char line[80];
  char* p = put(line, "Monitor: ");
  p = put(p, gd_pinmon_enabled() ? "On" : "Off");
  *p = 0;
  gd_text(0, 0, line, 1);
  for (int i = 0; i < gd_pin_count(); ++i) {
    p = put(line, "GPIO");
    p = putu(p, (unsigned)gd_pin_stat(i, GD_PIN_GPIO));
    p = put(p, ": ");
    if (gd_pin_stat(i, GD_PIN_CHATTER)) p = put(p, "chatter off, ");
    p = put(p, gd_pin_stat(i, GD_PIN_LEVEL) ? "high, " : "low, ");
    p = putu(p, (unsigned)gd_pin_stat(i, GD_PIN_CHANGES));
    p = put(p, " changes, ");
    p = putu(p, (unsigned)gd_pin_stat(i, GD_PIN_WAKES));
    p = put(p, " wakes");
    *p = 0;
    gd_text(0, lh * (i + 1), line, 0);
  }
}

GD_EXPORT(gd_input) int gd_input(int code, int x, int y) {
  (void)x;
  if (code == GD_CONFIRM || (code == GD_TAP && y < gd_line_height() + 6)) {
    gd_pinmon_set(!gd_pinmon_enabled());
    gd_log(gd_pinmon_enabled() ? "pinmon on" : "pinmon off");
  }
  return 1;
}
