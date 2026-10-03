#pragma once

#include <cstddef>
#include <cstring>

// Daily backups are "stats_YYYY-MM-DD.bin" (20 chars). Manual ("..._HHMM.bin")
// and clockless ("stats_backup_NNN.bin") backups are pruned as a separate set so
// neither can push out the other's newest files.
inline bool isDailyStatsBackupName(const char* name) {
  return name && strlen(name) == 20 && strncmp(name, "stats_", 6) == 0 && name[10] == '-' && name[13] == '-';
}

// Oldest-first order within one set: shorter name first (stats_backup_999 before
// stats_backup_1000), then by text (dates are zero-padded).
inline bool statsBackupOlder(const char* a, const char* b) {
  const size_t la = strlen(a), lb = strlen(b);
  return la != lb ? la < lb : strcmp(a, b) < 0;
}
