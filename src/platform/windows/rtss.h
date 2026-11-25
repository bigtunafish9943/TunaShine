/**
 * @file src/platform/windows/rtss.h
 * @brief Capping games with RivaTuner Statistics Server (RTSS) while streaming.
 */
#pragma once

namespace platf::rtss {
  /**
   * @brief Cap games at exactly this rate (in thousandths of a frame a second, 59940 for 59.94)
   *        with RTSS's global limit. The user's own limit is kept to put back.
   * @return false when RTSS isn't installed and running, so another limiter should be used.
   */
  bool set_limit(int millifps);

  /// Put RTSS's own limit back, if TunaStream changed it (also after a crash).
  void restore();
}  // namespace platf::rtss
