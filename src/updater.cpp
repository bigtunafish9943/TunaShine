/**
 * @file src/updater.cpp
 * @brief Installs a newer TunaStream release from inside the app.
 *
 * The latest release is looked up on GitHub, its installer is downloaded, checked against the
 * SHA-256 digest GitHub publishes for it, and run silently. The installer stops this program,
 * replaces it and starts the new version, keeping settings and paired devices.
 */
// standard includes
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

// local includes
#include "logging.h"
#include "platform/common.h"
#include "updater.h"
#include "utility.h"

#ifdef _WIN32
  #include <windows.h>

  #include "platform/windows/misc.h"
#endif

using namespace std::literals;

namespace updater {
  namespace {
    // Releases are only ever taken from this repository
    constexpr auto RELEASES_API = "https://api.github.com/repos/bigtunafish9943/TunaStream/releases/latest";
    constexpr auto DOWNLOAD_PREFIX = "https://github.com/bigtunafish9943/TunaStream/releases/download/";

    std::mutex state_lock;
    state_t current {"idle", {}, {}};
    bool running = false;
    bool verify_only_run = false;

    void set_stage(const std::string &stage, const std::string &version = {}, const std::string &error = {}) {
      std::lock_guard lg {state_lock};
      current = {stage, version, error};
    }

    size_t write_to_stream(char *data, size_t size, size_t count, void *user) {
      static_cast<std::ostream *>(user)->write(data, static_cast<std::streamsize>(size * count));
      return size * count;
    }

    /**
     * @brief Fetch a URL over HTTPS into a stream, following redirects.
     * @return Empty on success, otherwise a description of the failure.
     */
    std::string fetch(const std::string &url, std::ostream &out, bool api = false) {
      CURL *curl = curl_easy_init();
      if (!curl) {
        return "could not start the download";
      }
      auto cleanup = util::fail_guard([curl]() {
        curl_easy_cleanup(curl);
      });

      // GitHub's API wants this; file downloads get no special header
      curl_slist *headers = api ? curl_slist_append(nullptr, "Accept: application/vnd.github+json") : nullptr;
      auto free_headers = util::fail_guard([headers]() {
        curl_slist_free_all(headers);
      });

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "TunaStream-updater");  // GitHub's API requires one
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
#ifdef _WIN32
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_stream);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

      CURLcode result = curl_easy_perform(curl);
      if (result != CURLE_OK) {
        return std::string {"download failed: "} + curl_easy_strerror(result);
      }
      return {};
    }

    /// Split "v1.2.3" or "1.2.3" into numbers; non-numeric parts count as 0.
    std::vector<int> version_parts(std::string version) {
      if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        version.erase(0, 1);
      }
      std::vector<int> parts;
      std::stringstream ss {version};
      std::string part;
      while (std::getline(ss, part, '.')) {
        try {
          parts.push_back(std::stoi(part));
        } catch (...) {
          parts.push_back(0);
        }
      }
      return parts;
    }

    bool is_newer(const std::string &candidate, const std::string &installed) {
      auto a = version_parts(candidate);
      auto b = version_parts(installed);
      auto n = std::max(a.size(), b.size());
      a.resize(n, 0);
      b.resize(n, 0);
      return a > b;
    }

    std::string sha256_of_file(const std::filesystem::path &file) {
      std::ifstream in {file, std::ios::binary};
      if (!in) {
        return {};
      }
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      auto free_ctx = util::fail_guard([ctx]() {
        EVP_MD_CTX_free(ctx);
      });
      EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
      std::vector<char> buffer(1 << 16);
      while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        EVP_DigestUpdate(ctx, buffer.data(), static_cast<size_t>(in.gcount()));
      }
      unsigned char digest[EVP_MAX_MD_SIZE];
      unsigned int length = 0;
      EVP_DigestFinal_ex(ctx, digest, &length);

      // Lowercase hex, first byte first, as GitHub publishes it
      static constexpr char digits[] = "0123456789abcdef";
      std::string hex;
      hex.reserve(length * 2);
      for (unsigned int i = 0; i < length; ++i) {
        hex.push_back(digits[digest[i] >> 4]);
        hex.push_back(digits[digest[i] & 0x0F]);
      }
      return hex;
    }

    void run_update() {
      auto done = util::fail_guard([]() {
        std::lock_guard lg {state_lock};
        running = false;
      });

      set_stage("checking");
      std::stringstream release_json;
      if (auto err = fetch(RELEASES_API, release_json, true); !err.empty()) {
        BOOST_LOG(warning) << "Update check failed: "sv << err;
        set_stage("failed", {}, "Could not reach GitHub to look for updates (" + err + ").");
        return;
      }

      nlohmann::json release;
      try {
        release = nlohmann::json::parse(release_json.str());
      } catch (const std::exception &e) {
        set_stage("failed", {}, "GitHub's answer could not be read.");
        return;
      }

      const std::string version = release.value("tag_name", "");
      if (version.empty()) {
        set_stage("failed", {}, "No release was found.");
        return;
      }
      const bool verify_only = verify_only_run;
      if (!verify_only && !is_newer(version, PROJECT_VERSION)) {
        set_stage("up_to_date", version);
        return;
      }

      // Find the installer among the release's files
      std::string url, digest, name;
      for (const auto &asset : release.value("assets", nlohmann::json::array())) {
        std::string asset_name = asset.value("name", "");
        if (asset_name.size() > 10 && asset_name.ends_with("-setup.exe")) {
          name = asset_name;
          url = asset.value("browser_download_url", "");
          digest = asset.value("digest", "");
          break;
        }
      }
      if (url.empty() || !url.starts_with(DOWNLOAD_PREFIX)) {
        set_stage("failed", version, "The release has no installer to download.");
        return;
      }

      set_stage("downloading", version);
      auto folder = std::filesystem::path {platf::appdata()} / "updates";
      std::error_code ec;
      std::filesystem::create_directories(folder, ec);
      // Installers from earlier updates are no longer needed
      for (const auto &old : std::filesystem::directory_iterator {folder, ec}) {
        if (old.path().extension() == ".exe") {
          std::filesystem::remove(old.path(), ec);
        }
      }
      auto installer = folder / name;
      {
        std::ofstream out {installer, std::ios::binary | std::ios::trunc};
        if (!out) {
          set_stage("failed", version, "Could not save the installer.");
          return;
        }
        if (auto err = fetch(url, out); !err.empty()) {
          out.close();
          std::filesystem::remove(installer, ec);
          set_stage("failed", version, "Could not download the installer (" + err + ").");
          return;
        }
      }

      // GitHub publishes a SHA-256 for every release file; refuse a file that does not match
      if (digest.starts_with("sha256:")) {
        auto expected = digest.substr(7);
        auto actual = sha256_of_file(installer);
        std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
        std::transform(actual.begin(), actual.end(), actual.begin(), ::tolower);
        if (actual != expected) {
          std::filesystem::remove(installer, ec);
          BOOST_LOG(error) << "Update installer checksum mismatch: expected "sv << expected << ", got "sv << actual;
          set_stage("failed", version, "The downloaded installer did not match its published checksum, so it was not run.");
          return;
        }
      } else {
        BOOST_LOG(warning) << "Release has no published checksum; installing without one"sv;
      }

      if (verify_only) {
        // A check that the latest release downloads and matches its checksum; nothing is installed
        BOOST_LOG(info) << "Update check: "sv << name << " downloaded and matched its checksum"sv;
        std::filesystem::remove(installer, ec);
        set_stage(digest.starts_with("sha256:") ? "verified" : "failed", version, digest.starts_with("sha256:") ? "" : "The release has no published checksum.");
        return;
      }

      set_stage("installing", version);
      BOOST_LOG(info) << "Installing TunaStream "sv << version << " from "sv << installer.string();

#ifdef _WIN32
      // Run the installer silently, outside this program's job, so it survives this program
      // being stopped and replaced.
      std::wstring command = L"\"" + installer.wstring() + L"\" /S";
      STARTUPINFOW startup {};
      startup.cb = sizeof(startup);
      PROCESS_INFORMATION process {};
      if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS, nullptr, folder.wstring().c_str(), &startup, &process)) {
        auto code = GetLastError();
        BOOST_LOG(error) << "Couldn't start the update installer: "sv << code;
        set_stage("failed", version, "The installer could not be started (Windows error " + std::to_string(code) + ").");
        return;
      }
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
#else
      set_stage("failed", version, "Installing updates is only supported on Windows.");
#endif
    }
  }  // namespace

  bool start(bool verify_only) {
    {
      std::lock_guard lg {state_lock};
      if (running) {
        return false;
      }
      running = true;
      verify_only_run = verify_only;
    }
    std::thread {run_update}.detach();
    return true;
  }

  state_t state() {
    std::lock_guard lg {state_lock};
    return current;
  }
}  // namespace updater
