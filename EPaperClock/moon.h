#pragma once
// Moon phase from Meeus' elongation series (Astronomical Algorithms, ch. 48):
// good to a few hours for the phase instants and ~0.1% for illumination.
#include <math.h>
#include <time.h>

static inline double moonElong(time_t t) {  // degrees 0..360: 0 = new, 180 = full
  double jd = t / 86400.0 + 2440587.5;
  double T = (jd - 2451545.0) / 36525.0;
  double D = 297.8501921 + 445267.1114034 * T;
  double M = 357.5291092 + 35999.0502909 * T;
  double Mp = 134.9633964 + 477198.8675055 * T;
  const double r = M_PI / 180.0;
  double e = D + 6.289 * sin(Mp * r) - 2.100 * sin(M * r) + 1.274 * sin((2 * D - Mp) * r) +
             0.658 * sin(2 * D * r) + 0.214 * sin(2 * Mp * r) + 0.110 * sin(D * r);
  e = fmod(e, 360.0);
  return e < 0 ? e + 360.0 : e;
}

static inline float moonIllum(double elongDeg) { return (1.0f - cosf(elongDeg * M_PI / 180.0)) * 0.5f; }

static inline const char *moonName(double e) {
  if (e < 6.5 || e >= 353.5) return "New Moon";
  if (e < 83.5) return "Waxing Crescent";
  if (e < 96.5) return "First Quarter";
  if (e < 173.5) return "Waxing Gibbous";
  if (e < 186.5) return "Full Moon";
  if (e < 263.5) return "Waning Gibbous";
  if (e < 276.5) return "Last Quarter";
  return "Waning Crescent";
}

// Next time the elongation reaches `target` (0 = new, 180 = full), scanning hourly then interpolating.
static inline time_t moonNext(time_t from, double target) {
  auto f = [&](time_t t) { return fmod(moonElong(t) - target + 540.0, 360.0) - 180.0; };
  double prev = f(from);
  for (time_t t = from + 3600; t < from + 40 * 86400L; t += 3600) {
    double cur = f(t);
    if (prev < 0 && cur >= 0) return (time_t)(t - 3600 + 3600.0 * (-prev) / (cur - prev));
    prev = cur;
  }
  return 0;
}
