/**
 * @file src/other_hosts.cpp
 * @brief Notices Sunshine and Apollo running on this PC, and pauses or resumes them on request.
 *
 * Two hosts streaming at once fight over the encoder, the virtual display driver and the
 * controllers. TunaStream runs as a system service, so it can stop and start their services.
 * Their start-up settings are not changed: a stopped host starts again with Windows.
 */
// standard includes
#include <array>
#include <thread>

// local includes
#include "logging.h"
#include "other_hosts.h"

#ifdef _WIN32
  #include <windows.h>
#endif

using namespace std::literals;

namespace other_hosts {
  namespace {
    struct known_t {
      const char *name;
      const char *service;
    };

    constexpr std::array<known_t, 2> known_hosts {{
      {"Sunshine", "SunshineService"},
      {"Apollo", "ApolloService"},
    }};
  }  // namespace

#ifdef _WIN32
  std::vector<host_t> installed() {
    std::vector<host_t> out;
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
      return out;
    }
    for (const auto &host : known_hosts) {
      SC_HANDLE service = OpenServiceA(manager, host.service, SERVICE_QUERY_STATUS);
      if (!service) {
        continue;
      }
      SERVICE_STATUS status {};
      bool running = QueryServiceStatus(service, &status) && status.dwCurrentState != SERVICE_STOPPED;
      out.push_back({host.name, host.service, running});
      CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return out;
  }

  std::string set_running(const std::string &service_name, bool run) {
    const known_t *host = nullptr;
    for (const auto &h : known_hosts) {
      if (service_name == h.service) {
        host = &h;
      }
    }
    if (!host) {
      return "Unknown service.";
    }

    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
      return "Could not reach Windows services.";
    }
    SC_HANDLE service = OpenServiceA(manager, host->service, SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
    if (!service) {
      CloseServiceHandle(manager);
      return std::string {host->name} + " is not installed.";
    }

    std::string error;
    SERVICE_STATUS status {};
    if (run) {
      if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        error = "Windows would not start " + std::string {host->name} + " (error " + std::to_string(GetLastError()) + ").";
      }
    } else if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
      error = "Windows would not stop " + std::string {host->name} + " (error " + std::to_string(GetLastError()) + ").";
    }

    // Wait briefly for the change, so the page shows the new state
    if (error.empty()) {
      const DWORD wanted = run ? SERVICE_RUNNING : SERVICE_STOPPED;
      for (int i = 0; i < 40 && QueryServiceStatus(service, &status) && status.dwCurrentState != wanted; ++i) {
        std::this_thread::sleep_for(250ms);
      }
      BOOST_LOG(info) << (run ? "Started "sv : "Paused "sv) << host->name << " at the user's request"sv;
    } else {
      BOOST_LOG(warning) << error;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return error;
  }
#else
  std::vector<host_t> installed() {
    return {};
  }

  std::string set_running(const std::string &, bool) {
    return "Only available on Windows.";
  }
#endif
}  // namespace other_hosts
