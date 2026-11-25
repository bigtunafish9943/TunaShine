/**
 * @file src/platform/windows/self_check.h
 * @brief What the PC can check by itself before testing (the checklist page's Self-check).
 */
#pragma once

// lib includes
#include <nlohmann/json.hpp>

namespace platf {
  /// [{"id", "state": "ok" | "warn" | "info", "detail"}]
  nlohmann::json self_check();
}  // namespace platf
