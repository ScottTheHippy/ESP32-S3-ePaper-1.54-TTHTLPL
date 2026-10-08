#pragma once
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static inline int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}

// "YYYY-MM-DD HH:MM" in GMT -> epoch seconds (0 on parse failure)
static inline time_t parseGmt(const char *s) {
  int y, mo, d, h, mi;
  if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
  return (time_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60);
}
