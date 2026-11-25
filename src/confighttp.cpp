/**
 * @file src/confighttp.cpp
 * @brief Definitions for the Web UI Config HTTPS server.
 *
 * @todo Authentication, better handling of routes common to nvhttp, cleanup
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <ctime>
#include <thread>
#include <numeric>
#include <algorithm>
#include <condition_variable>
#include <cctype>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>
#include <Simple-Web-Server/crypto.hpp>
#include <Simple-Web-Server/server_https.hpp>

// local includes
#include "config.h"
#include "confighttp.h"
#include "library_sync.h"
#include "watch.h"
#include "crypto.h"
#include "display_device.h"
#include "file_handler.h"
#include "globals.h"
#include "httpcommon.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "platform/common.h"
#include "input.h"
#include "process.h"
#include "rtsp.h"
#include "stream.h"
#include "game_library.h"
#include "other_hosts.h"
#include "file_outbox.h"
#include "pairing_backup.h"
#include "play_history.h"
#include "play_limits.h"
#include "box_art.h"
#include "captures.h"
#include "receiver_settings.h"
#include "settings_backup.h"
#include "sound_board.h"
#include "tv_messages.h"
#include "updater.h"
#include "utility.h"
#include "uuid.h"

#ifdef _WIN32
  #include "platform/windows/utils.h"
#endif

using namespace std::literals;

namespace confighttp {
  namespace fs = std::filesystem;

  using https_server_t = SimpleWeb::Server<SimpleWeb::HTTPS>;
  using args_t = SimpleWeb::CaseInsensitiveMultimap;
  using resp_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Response>;
  using req_https_t = std::shared_ptr<typename SimpleWeb::ServerBase<SimpleWeb::HTTPS>::Request>;

  // Keep the base enum for client operations.
  enum class op_e {
    ADD,    ///< Add client
    REMOVE  ///< Remove client
  };

  /**
   * Signed-in browsers. Each sign-in gets its own cookie, so signing in on the phone doesn't
   * sign out the PC's browser, and they are kept in the config folder (as salted hashes, never
   * the cookies themselves) so a restart (Save and Apply, an update) doesn't sign anyone out.
   */
  namespace web_sessions {
    struct session_t {
      std::string hash;
      std::int64_t created;  // seconds since 1970
    };

    constexpr std::size_t MAX_SESSIONS = 20;
    std::mutex lock;
    std::vector<session_t> sessions;
    bool loaded = false;

    std::filesystem::path file() {
      return platf::appdata() / "web_sessions.json";
    }

    std::int64_t now_s() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // With the lock held
    void save() {
      nlohmann::json list = nlohmann::json::array();
      for (const auto &s : sessions) {
        list.push_back({{"hash", s.hash}, {"created", s.created}});
      }
      std::error_code ec;
      const auto path = file();
      auto partial = path;
      partial += ".part";
      {
        std::ofstream out {partial, std::ios::trunc};
        out << list.dump();
        if (!out) {
          return;
        }
      }
      std::filesystem::rename(partial, path, ec);
    }

    // With the lock held: read them once, and drop expired ones
    void prepare() {
      if (!loaded) {
        loaded = true;
        std::ifstream in {file()};
        if (in) {
          try {
            auto list = nlohmann::json::parse(in);
            for (const auto &s : list) {
              sessions.push_back({s.value("hash", ""), s.value("created", (std::int64_t) 0)});
            }
          } catch (...) {
            sessions.clear();
          }
        }
      }
      const auto oldest = now_s() - std::chrono::duration_cast<std::chrono::seconds>(SESSION_EXPIRE_DURATION).count();
      const auto before = sessions.size();
      std::erase_if(sessions, [&](const session_t &s) {
        return s.hash.empty() || s.created < oldest;
      });
      if (sessions.size() != before) {
        save();
      }
    }

    bool valid(const std::string &hash) {
      std::lock_guard lg {lock};
      prepare();
      return std::any_of(sessions.begin(), sessions.end(), [&](const session_t &s) {
        return s.hash == hash;
      });
    }

    void add(const std::string &hash) {
      std::lock_guard lg {lock};
      prepare();
      sessions.push_back({hash, now_s()});
      // The oldest sign-ins give way beyond the limit
      while (sessions.size() > MAX_SESSIONS) {
        sessions.erase(sessions.begin());
      }
      save();
    }

    /// Everyone signs in again (the password changed)
    void clear() {
      std::lock_guard lg {lock};
      prepare();
      sessions.clear();
      save();
    }
  }  // namespace web_sessions

  /**
   * Access tokens for scripts and share links: each allows only the scopes picked for it, is
   * shown once, and is kept only as a salted hash in api_tokens.json.
   */
  namespace api_tokens {
    struct token_t {
      std::string id;
      std::string name;
      std::string hash;
      std::vector<std::string> scopes;
      std::int64_t created = 0;
      std::int64_t expires = 0;  // 0: never
      std::int64_t last_used = 0;
      // Watch links: the link is an invite that works once, for a day; the device that opens it
      // gets its own key, which changes every day while it's used
      bool invite = false;
      bool rotating = false;
      // A watch password: typed on the Watch page, it gives that device a watch key. It doesn't
      // change and doesn't run out; removing it also removes the keys it gave.
      bool password = false;
      std::string parent;  // the watch password that gave this key
      std::string prev_hash;  // the key before the last change, for a few minutes
      std::int64_t prev_until = 0;
      std::int64_t rotated = 0;
    };

    constexpr std::int64_t ROTATE_EVERY = 86400;  // a watch key changes daily
    constexpr std::int64_t ROTATING_LIFETIME = 7 * 86400;  // and stops working after a week unused
    constexpr std::int64_t ROTATE_GRACE = 600;

    std::mutex lock;
    std::vector<token_t> tokens;
    bool loaded = false;

    const std::vector<std::string> SCOPES {"status", "watch", "messages", "sessions", "launch", "logs"};

    std::filesystem::path file() {
      return platf::appdata() / "api_tokens.json";
    }

    std::int64_t now_s() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string hash_of(const std::string &token) {
      return util::hex(crypto::hash(token + config::tunastream.salt)).to_string();
    }

    // With the lock held
    void save() {
      nlohmann::json list = nlohmann::json::array();
      for (const auto &t : tokens) {
        list.push_back({{"id", t.id}, {"name", t.name}, {"hash", t.hash}, {"scopes", t.scopes}, {"created", t.created}, {"expires", t.expires}, {"lastUsed", t.last_used}, {"invite", t.invite}, {"rotating", t.rotating}, {"prevHash", t.prev_hash}, {"prevUntil", t.prev_until}, {"rotated", t.rotated}, {"password", t.password}, {"parent", t.parent}});
      }
      std::error_code ec;
      const auto path = file();
      auto partial = path;
      partial += ".part";
      {
        std::ofstream out {partial, std::ios::trunc};
        out << list.dump();
        if (!out) {
          return;
        }
      }
      std::filesystem::rename(partial, path, ec);
    }

    // With the lock held
    void prepare() {
      if (loaded) {
        return;
      }
      loaded = true;
      std::ifstream in {file()};
      if (!in) {
        return;
      }
      try {
        for (const auto &j : nlohmann::json::parse(in)) {
          token_t t;
          t.id = j.value("id", "");
          t.name = j.value("name", "");
          t.hash = j.value("hash", "");
          t.scopes = j.value("scopes", std::vector<std::string> {});
          t.created = j.value("created", (std::int64_t) 0);
          t.expires = j.value("expires", (std::int64_t) 0);
          t.last_used = j.value("lastUsed", (std::int64_t) 0);
          t.invite = j.value("invite", false);
          t.rotating = j.value("rotating", false);
          t.prev_hash = j.value("prevHash", "");
          t.prev_until = j.value("prevUntil", (std::int64_t) 0);
          t.rotated = j.value("rotated", (std::int64_t) 0);
          t.password = j.value("password", false);
          t.parent = j.value("parent", "");
          if (!t.id.empty() && !t.hash.empty()) {
            tokens.push_back(std::move(t));
          }
        }
      } catch (const std::exception &) {
        tokens.clear();
      }
    }

    /// The scope a request needs, or empty when no token may make it
    std::string scope_of(const std::string &method, const std::string &path) {
      if (method == "GET") {
        if (path == "/api/watch/stream" || path == "/watch" || path == "/watch/") {
          return "watch";
        }
        if (path == "/api/watch/viewers") {
          return "status";
        }
        if (path == "/api/tokens/self") {
          return "any";  // what a token may do, for agents (tunastream.exe --mcp)
        }
        if (path == "/api/logs" || path == "/api/receiver-logs" || path == "/api/receiver-log") {
          return "logs";
        }
        for (auto p : {"/api/status", "/api/clients/list", "/api/apps", "/api/recap", "/api/play-limits", "/api/captures", "/api/receiver-settings", "/api/sounds"}) {
          if (path == p) {
            return "status";
          }
        }
      } else if (method == "POST") {
        if (path == "/api/watch/redeem") {
          return "redeem";  // only an unused watch link
        }
        if (path == "/api/watch/rotate") {
          return "rotate";  // only a device's watch key
        }
        if (path == "/api/tv-message" || path == "/api/sounds/play") {
          return "messages";
        }
        if (path == "/api/apps/close" || path == "/api/play-limits/extend") {
          return "sessions";
        }
        if (path == "/api/apps/launch") {
          return "launch";
        }
      }
      return {};
    }

    /// Whether this token may make this request. A watch token may also read the game's name.
    bool allows(const std::string &token, const std::string &method, const std::string &path) {
      auto scope = scope_of(method, path);
      if (scope.empty() || token.size() < 20) {
        return false;
      }
      const auto hash = hash_of(token);
      std::lock_guard lg {lock};
      prepare();
      const auto now = now_s();
      for (auto &t : tokens) {
        const bool previous = !t.prev_hash.empty() && t.prev_hash == hash && now < t.prev_until;
        if (t.hash != hash && !previous) {
          continue;
        }
        if (t.expires && t.expires < now) {
          return false;
        }
        // A watch password is only ever typed into the Watch page, never sent as a token
        if (t.password) {
          return false;
        }
        // A watch link only swaps itself for a key; only a key changes itself
        if (t.invite || scope == "redeem") {
          return t.invite && scope == "redeem";
        }
        if (scope == "rotate") {
          return t.rotating && !previous;
        }
        bool ok = scope == "any" || std::find(t.scopes.begin(), t.scopes.end(), scope) != t.scopes.end();
        if (!ok && scope == "status" && path == "/api/status") {
          ok = std::find(t.scopes.begin(), t.scopes.end(), "watch") != t.scopes.end();
        }
        if (ok && now - t.last_used > 60) {
          t.last_used = now;
          save();
        }
        return ok;
      }
      return false;
    }

    /// The token a request carries: "Authorization: Bearer ...", or ?token= on a GET (share links)
    std::string from_request(const req_https_t &request) {
      for (auto &[name, value] : request->header) {
        if (boost::iequals(name, "authorization") && boost::istarts_with(value, "Bearer ")) {
          return value.substr(7);
        }
      }
      if (request->method == "GET") {
        auto args = request->parse_query_string();
        if (auto it = args.find("token"); it != args.end()) {
          return it->second;
        }
      }
      return {};
    }

    /// The token's id and name, for a list of who is watching; empty when it isn't a token.
    std::pair<std::string, std::string> who(const std::string &token) {
      if (token.empty()) {
        return {};
      }
      const auto hash = hash_of(token);
      const auto now = now_s();
      std::lock_guard lg {lock};
      prepare();
      for (const auto &t : tokens) {
        if (t.hash == hash || (!t.prev_hash.empty() && t.prev_hash == hash && now < t.prev_until)) {
          return {t.id, t.name};
        }
      }
      return {};
    }

    /// Remove a token, and the watch password it came from (with that password's other keys).
    std::string remove_with_source(const std::string &id) {
      std::lock_guard lg {lock};
      prepare();
      std::string parent;
      std::string name;
      for (const auto &t : tokens) {
        if (t.id == id) {
          parent = t.parent;
          name = t.name;
        }
      }
      std::erase_if(tokens, [&](const token_t &t) {
        return t.id == id || (!parent.empty() && (t.id == parent || t.parent == parent));
      });
      save();
      return name;
    }

    /// "iPad", "Android phone"...: a hint at the device, from its browser
    std::string device_hint(const req_https_t &request) {
      auto ua = request->header.find("User-Agent");
      if (ua == request->header.end()) {
        return {};
      }
      const auto &s = ua->second;
      auto has = [&](const char *x) {
        return s.find(x) != std::string::npos;
      };
      if (has("iPad") || (has("Macintosh") && has("Mobile"))) {
        return "iPad";
      }
      if (has("iPhone")) {
        return "iPhone";
      }
      if (has("Android")) {
        return has("Mobile") ? "Android phone" : "Android tablet";
      }
      if (has("CrOS")) {
        return "Chromebook";
      }
      if (has("Windows")) {
        return "Windows PC";
      }
      if (has("Macintosh")) {
        return "Mac";
      }
      if (has("Linux")) {
        return "Linux PC";
      }
      return {};
    }
  }  // namespace api_tokens

  /**
   * @brief Log the request details.
   * @param request The HTTP request object.
   */
  void print_req(const req_https_t &request) {
    BOOST_LOG(debug) << "METHOD :: "sv << request->method;
    BOOST_LOG(debug) << "DESTINATION :: "sv << request->path;
    for (auto &[name, val] : request->header) {
      const bool secret = boost::iequals(name, "authorization") || boost::iequals(name, "cookie");
      BOOST_LOG(debug) << name << " -- " << (secret ? "CREDENTIALS REDACTED" : val);
    }
    BOOST_LOG(debug) << " [--] "sv;
    for (auto &[name, val] : request->parse_query_string()) {
      BOOST_LOG(debug) << name << " -- " << (name == "token" ? "REDACTED" : val);
    }
    BOOST_LOG(debug) << " [--] "sv;
  }

  /**
   * @brief Send a response.
   * @param response The HTTP response object.
   * @param output_tree The JSON tree to send.
   */
  void send_response(resp_https_t response, const nlohmann::json &output_tree) {
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(output_tree.dump(), headers);
  }

  /**
   * @brief Send a 401 Unauthorized response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void send_unauthorized(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- not authorized"sv;
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_unauthorized;
    nlohmann::json tree;
    tree["status_code"] = code;
    tree["status"] = false;
    tree["error"] = "Unauthorized";
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Content-Type", "application/json"},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a redirect response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param path The path to redirect to.
   */
  void send_redirect(resp_https_t response, req_https_t request, const char *path) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    BOOST_LOG(info) << "Web UI: ["sv << address << "] -- redirecting"sv;
    const SimpleWeb::CaseInsensitiveMultimap headers {
      {"Location", path},
      {"X-Frame-Options", "DENY"},
      {"Content-Security-Policy", "frame-ancestors 'none';"}
    };
    response->write(SimpleWeb::StatusCode::redirection_temporary_redirect, headers);
  }

  /**
   * @brief Retrieve the value of a key from a cookie string.
   * @param cookieString The cookie header string.
   * @param key The key to search.
   * @return The value if found, empty string otherwise.
   */
  std::string getCookieValue(const std::string& cookieString, const std::string& key) {
    std::string keyWithEqual = key + "=";
    std::size_t startPos = cookieString.find(keyWithEqual);
    if (startPos == std::string::npos)
      return "";
    startPos += keyWithEqual.length();
    std::size_t endPos = cookieString.find(";", startPos);
    if (endPos == std::string::npos)
      return cookieString.substr(startPos);
    return cookieString.substr(startPos, endPos - startPos);
  }

  /**
   * @brief Check if the IP origin is allowed.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @return True if allowed, false otherwise.
   */
  bool checkIPOrigin(resp_https_t response, req_https_t request) {
    auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    auto ip_type = net::from_address(address);
    if (ip_type > http::origin_web_ui_allowed) {
      BOOST_LOG(info) << "Web UI: ["sv << address << "] -- denied"sv;
      response->write(SimpleWeb::StatusCode::client_error_forbidden);
      return false;
    }
    return true;
  }

  /**
   * @brief Authenticate the request.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param needsRedirect Whether to redirect on failure.
   * @return True if authenticated, false otherwise.
   *
   * This function uses session cookies (if set) and ensures they have not expired.
   */
  bool authenticate(resp_https_t response, req_https_t request, bool needsRedirect = false) {
    if (!checkIPOrigin(response, request))
      return false;
    // If credentials not set, redirect to welcome.
    if (config::tunastream.username.empty()) {
      send_redirect(response, request, "/welcome");
      return false;
    }
    // Guard: on failure, redirect if requested.
    auto fg = util::fail_guard([&]() {
      if (needsRedirect) {
        std::string redir_path = "/login?redir=.";
        redir_path += request->path;
        send_redirect(response, request, redir_path.c_str());
      } else {
        send_unauthorized(response, request);
      }
    });
    // A script or a share link with an access token: only what the token allows
    if (auto token = api_tokens::from_request(request); !token.empty()) {
      if (!api_tokens::allows(token, request->method, request->path)) {
        return false;
      }
      fg.disable();
      return true;
    }
    auto cookies = request->header.find("cookie");
    if (cookies == request->header.end())
      return false;
    auto authCookie = getCookieValue(cookies->second, "auth");
    if (authCookie.empty() ||
        !web_sessions::valid(util::hex(crypto::hash(authCookie + config::tunastream.salt)).to_string()))
      return false;
    fg.disable();
    return true;
  }

  /**
   * @brief Send a 404 Not Found response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void not_found(resp_https_t response, [[maybe_unused]] req_https_t request) {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_not_found;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["error"] = "Not Found";
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }

  /**
   * @brief Send a 400 Bad Request response.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param error_message The error message.
   */
  void bad_request(resp_https_t response, [[maybe_unused]] req_https_t request, const std::string &error_message = "Bad Request") {
    constexpr SimpleWeb::StatusCode code = SimpleWeb::StatusCode::client_error_bad_request;
    nlohmann::json tree;
    tree["status_code"] = static_cast<int>(code);
    tree["status"] = false;
    tree["error"] = error_message;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");

    response->write(code, tree.dump(), headers);
  }


  /**
   * @brief Validate the request content type and send bad request when mismatch.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * @param contentType The required content type.
   */
  bool validateContentType(resp_https_t response, req_https_t request, const std::string_view& contentType) {
    auto requestContentType = request->header.find("content-type");
    if (requestContentType == request->header.end()) {
      bad_request(response, request, "Content type not provided");
      return false;
    }

    // Extract the media type part before any parameters (e.g., charset)
    std::string actualContentType = requestContentType->second;
    size_t semicolonPos = actualContentType.find(';');
    if (semicolonPos != std::string::npos) {
      actualContentType = actualContentType.substr(0, semicolonPos);
    }

    // Trim whitespace and convert to lowercase for case-insensitive comparison
    boost::algorithm::trim(actualContentType);
    boost::algorithm::to_lower(actualContentType);

    std::string expectedContentType(contentType);
    boost::algorithm::to_lower(expectedContentType);

    if (actualContentType != expectedContentType) {
      bad_request(response, request, "Content type mismatch");
      return false;
    }
    return true;

    return true;
  }

  /**
   * @brief Get the index page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getIndexPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "index.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the PIN page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPinPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "pin.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the apps page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getAppsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "apps.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    headers.emplace("Access-Control-Allow-Origin", "https://images.igdb.com/");
    response->write(content, headers);
  }

  /**
   * @brief Get the clients page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getClientsPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "clients.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the configuration page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfigPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "config.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the password page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getPasswordPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "password.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the login page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo Combine this function with getWelcomePage if appropriate.
   */
  void getLoginPage(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }

    if (config::tunastream.username.empty()) {
      send_redirect(response, request, "/welcome");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "login.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the welcome page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getWelcomePage(resp_https_t response, req_https_t request) {
    print_req(request);

    if (!config::tunastream.username.empty()) {
      send_redirect(response, request, "/");
      return;
    }

    std::string content = file_handler::read_file(WEB_DIR "welcome.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the setup walkthrough page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getSetupPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "setup.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the help page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getHelpPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "help.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the troubleshooting page.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getTroubleshootingPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "troubleshooting.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Get the favicon image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getFaviconImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/tunastream.ico", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/x-icon");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the TunaStream logo image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @todo combine function with getFaviconImage and possibly getNodeModules
   * @todo use mime_types map
   */
  void getTunaStreamLogoImage(resp_https_t response, req_https_t request) {
    print_req(request);

    std::ifstream in(WEB_DIR "images/logo-tunastream-45.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief The home-screen icon for the phone page: /images/tunastream-app-512.png or -180.png.
   */
  void getAppIcon(resp_https_t response, req_https_t request) {
    const bool small = request->path.find("180") != std::string::npos;
    std::ifstream in(small ? WEB_DIR "images/tunastream-app-180.png" : WEB_DIR "images/tunastream-app-512.png", std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "image/png");
    headers.emplace("Cache-Control", "public, max-age=86400");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Lets a tablet or phone add the Watch page to its home screen like an app.
   */
  void getWatchManifest(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request)) {
      return;
    }
    nlohmann::json manifest {
      {"name", "TunaStream Watch"},
      {"short_name", "Watch"},
      {"start_url", "./watch"},
      {"scope", "./"},
      {"display", "standalone"},
      {"orientation", "any"},
      {"background_color", "#000000"},
      {"theme_color", "#041431"},
      {"icons", nlohmann::json::array({
        {{"src", "./images/tunastream-app-180.png"}, {"sizes", "180x180"}, {"type", "image/png"}},
        {{"src", "./images/tunastream-app-512.png"}, {"sizes", "512x512"}, {"type", "image/png"}},
      })},
    };
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/manifest+json");
    response->write(SimpleWeb::StatusCode::success_ok, manifest.dump(), headers);
  }

  /**
   * @brief Lets a phone add the Live page to its home screen like an app.
   */
  void getLiveManifest(resp_https_t response, req_https_t request) {
    nlohmann::json manifest {
      {"name", "TunaStream Live"},
      {"short_name", "TunaStream"},
      {"start_url", "./live"},
      {"scope", "./"},
      {"display", "standalone"},
      {"background_color", "#041431"},
      {"theme_color", "#041431"},
      {"icons", nlohmann::json::array({
        {{"src", "./images/tunastream-app-180.png"}, {"sizes", "180x180"}, {"type", "image/png"}},
        {{"src", "./images/tunastream-app-512.png"}, {"sizes", "512x512"}, {"type", "image/png"}},
      })},
    };
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/manifest+json");
    response->write(SimpleWeb::StatusCode::success_ok, manifest.dump(), headers);
  }

  /**
   * @brief The test checklist page: what to try on the real setup, with what was found.
   */
  void getChecklistPage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "checklist.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief Keep my games in sync: GET the settings and last sync; POST {"enabled", "days",
   *        "forgetRemoved", "now"} to change them or sync now.
   */
  void getLibrarySync(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    auto out = library_sync::status();
    out["status"] = true;
    send_response(response, out);
  }

  void postLibrarySync(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      if (input.value("now", false)) {
        library_sync::run_soon();
      } else {
        library_sync::configure(input);
      }
      auto out = library_sync::status();
      out["status"] = true;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Signed in with the password (not a token): token management never accepts a token.
   */
  bool authenticate_owner(resp_https_t response, req_https_t request) {
    if (!api_tokens::from_request(request).empty()) {
      send_unauthorized(response, request);
      return false;
    }
    return authenticate(response, request);
  }

  /**
   * @brief Access tokens: {"tokens": [{"id", "name", "scopes", "created", "expires", "lastUsed"}], "scopes": [...]}.
   */
  void getApiTokens(resp_https_t response, req_https_t request) {
    if (!authenticate_owner(response, request)) {
      return;
    }
    nlohmann::json list = nlohmann::json::array();
    {
      std::lock_guard lg {api_tokens::lock};
      api_tokens::prepare();
      for (const auto &t : api_tokens::tokens) {
        if (t.expires && t.expires < api_tokens::now_s() && (t.invite || t.rotating)) {
          continue;  // watch links and keys that ran out
        }
        list.push_back({{"id", t.id}, {"name", t.name}, {"scopes", t.scopes}, {"created", t.created}, {"expires", t.expires}, {"lastUsed", t.last_used}, {"invite", t.invite}, {"rotating", t.rotating}, {"password", t.password}, {"parent", t.parent}});
      }
    }
    nlohmann::json out;
    out["status"] = true;
    out["tokens"] = list;
    out["scopes"] = api_tokens::SCOPES;
#ifdef _WIN32
    // For connecting an AI assistant: "tunastream.exe --mcp" with a token
    out["exePath"] = (platf::appdata().parent_path() / "tunastream.exe").string();
#endif
    send_response(response, out);
  }

  /**
   * @brief GET /api/tokens/self: what the token this request carries may do, {"name", "scopes",
   *        "expires"}. Signed in with the password, everything.
   */
  void getApiTokenSelf(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json out;
    out["status"] = true;
    const auto token = api_tokens::from_request(request);
    if (token.empty()) {
      out["name"] = "Signed in with the password";
      out["scopes"] = api_tokens::SCOPES;
      out["expires"] = 0;
      send_response(response, out);
      return;
    }
    const auto hash = api_tokens::hash_of(token);
    std::lock_guard lg {api_tokens::lock};
    api_tokens::prepare();
    for (const auto &t : api_tokens::tokens) {
      if (t.hash == hash) {
        out["name"] = t.name;
        out["scopes"] = t.scopes;
        out["expires"] = t.expires;
        break;
      }
    }
    send_response(response, out);
  }

  /**
   * @brief Make a token: {"name", "scopes": [...], "days": 0 for never}. Answers the token, once.
   */
  void postApiToken(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      api_tokens::token_t t;
      t.name = input.value("name", std::string {});
      if (t.name.size() > 60) {
        t.name.resize(60);
      }
      for (const auto &s : input.value("scopes", std::vector<std::string> {})) {
        if (std::find(api_tokens::SCOPES.begin(), api_tokens::SCOPES.end(), s) != api_tokens::SCOPES.end() &&
            std::find(t.scopes.begin(), t.scopes.end(), s) == t.scopes.end()) {
          t.scopes.push_back(s);
        }
      }
      if (t.name.empty() || t.scopes.empty()) {
        bad_request(response, request, "Give the token a name and at least one thing it may do.");
        return;
      }
      const int days = std::clamp(input.value("days", 0), 0, 3650);
      const std::string token = "tst_" + crypto::rand_alphabet(40);
      t.id = crypto::rand_alphabet(12);
      t.hash = api_tokens::hash_of(token);
      t.created = api_tokens::now_s();
      t.expires = days ? t.created + days * 86400LL : 0;
      {
        std::lock_guard lg {api_tokens::lock};
        api_tokens::prepare();
        if (api_tokens::tokens.size() >= 50) {
          bad_request(response, request, "There are 50 tokens already. Remove some first.");
          return;
        }
        api_tokens::tokens.push_back(t);
        api_tokens::save();
      }
      BOOST_LOG(info) << "Access token made: "sv << t.name;
      nlohmann::json out;
      out["status"] = true;
      out["id"] = t.id;
      out["token"] = token;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Remove a token: {"id"}.
   */
  void postApiTokenDelete(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto id = nlohmann::json::parse(ss.str()).value("id", std::string {});
      {
        std::lock_guard lg {api_tokens::lock};
        api_tokens::prepare();
        std::erase_if(api_tokens::tokens, [&](const api_tokens::token_t &t) {
          return t.id == id || (!id.empty() && t.parent == id);  // a watch password takes its devices' keys along
        });
        api_tokens::save();
      }
      nlohmann::json out;
      out["status"] = true;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  namespace watch_keys {
    std::string read_body(req_https_t request) {
      std::stringstream ss;
      ss << request->content.rdbuf();
      return ss.str();
    }

    /// Give this token a new secret; the old one keeps working for a few minutes. With the lock held.
    std::string renew_locked(api_tokens::token_t &t, bool keep_previous) {
      const std::string fresh = "tst_" + crypto::rand_alphabet(40);
      const auto now = api_tokens::now_s();
      if (keep_previous) {
        t.prev_hash = t.hash;
        t.prev_until = now + api_tokens::ROTATE_GRACE;
      } else {
        t.prev_hash.clear();
        t.prev_until = 0;
      }
      t.hash = api_tokens::hash_of(fresh);
      t.rotated = now;
      t.expires = now + api_tokens::ROTATING_LIFETIME;
      t.last_used = now;
      api_tokens::save();
      return fresh;
    }
  }  // namespace watch_keys

  /**
   * @brief POST /api/watch/invite {"name"}: a watch link to share. It works once, within a day:
   *        the device that opens it gets its own watch key, which changes every day.
   */
  void postWatchInvite(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      auto input = nlohmann::json::parse(watch_keys::read_body(request));
      api_tokens::token_t t;
      t.name = input.value("name", std::string {"Watch link"});
      if (t.name.size() > 60) {
        t.name.resize(60);
      }
      t.scopes = {"watch"};
      t.invite = true;
      const std::string invite = "tst_" + crypto::rand_alphabet(40);
      t.id = crypto::rand_alphabet(12);
      t.hash = api_tokens::hash_of(invite);
      t.created = api_tokens::now_s();
      t.expires = t.created + 86400;
      {
        std::lock_guard lg {api_tokens::lock};
        api_tokens::prepare();
        // Old links nobody opened go first
        std::erase_if(api_tokens::tokens, [&](const api_tokens::token_t &x) {
          return x.expires && x.expires < t.created;
        });
        if (api_tokens::tokens.size() >= 50) {
          bad_request(response, request, "There are 50 tokens and watch links already. Remove some under Change Password > Access tokens first.");
          return;
        }
        api_tokens::tokens.push_back(t);
        api_tokens::save();
      }
      BOOST_LOG(info) << "Watch link made: "sv << t.name;
      nlohmann::json out;
      out["status"] = true;
      out["invite"] = invite;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief POST /api/watch/redeem with a watch link (Authorization: Bearer): the link stops
   *        working, and this device gets its own watch key, {"key", "rotateAfter"}.
   */
  void postWatchRedeem(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    const auto hash = api_tokens::hash_of(api_tokens::from_request(request));
    std::lock_guard lg {api_tokens::lock};
    api_tokens::prepare();
    for (auto &t : api_tokens::tokens) {
      if (t.hash == hash && t.invite) {
        t.invite = false;
        t.rotating = true;
        const auto key = watch_keys::renew_locked(t, false);
        BOOST_LOG(info) << "Watch link opened on a device: "sv << t.name;
        nlohmann::json out;
        out["status"] = true;
        out["key"] = key;
        out["rotateAfter"] = api_tokens::ROTATE_EVERY;
        send_response(response, out);
        return;
      }
    }
    bad_request(response, request, "This watch link was already used or has expired.");
  }

  namespace watch_passwords {
    std::mutex lock;
    std::map<std::string, std::pair<int, std::int64_t>> failures;  // address: count, since

    /// Too many wrong watch passwords from this address lately: 5 in 10 minutes
    bool blocked(const std::string &address) {
      std::lock_guard lg {lock};
      auto it = failures.find(address);
      if (it == failures.end()) {
        return false;
      }
      if (api_tokens::now_s() - it->second.second > 600) {
        failures.erase(it);
        return false;
      }
      return it->second.first >= 5;
    }

    void failed(const std::string &address) {
      std::lock_guard lg {lock};
      auto &f = failures[address];
      if (f.first == 0 || api_tokens::now_s() - f.second > 600) {
        f = {0, api_tokens::now_s()};
      }
      ++f.first;
    }
  }  // namespace watch_passwords

  /**
   * @brief POST /api/watch/passwords {"name", "password"}: a watch password that doesn't change.
   *        Typed on the Watch page, it lets that device watch (with its own daily-changing key).
   */
  void postWatchPasswordMake(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      auto input = nlohmann::json::parse(watch_keys::read_body(request));
      api_tokens::token_t t;
      t.name = input.value("name", std::string {});
      const auto password = input.value("password", std::string {});
      if (t.name.empty() || t.name.size() > 60) {
        bad_request(response, request, "Give the watch password a name, up to 60 characters.");
        return;
      }
      if (password.size() < 8 || password.size() > 128) {
        bad_request(response, request, "A watch password needs at least 8 characters.");
        return;
      }
      t.scopes = {"watch"};
      t.password = true;
      t.id = crypto::rand_alphabet(12);
      t.hash = api_tokens::hash_of("watch-password:" + password);
      t.created = api_tokens::now_s();
      {
        std::lock_guard lg {api_tokens::lock};
        api_tokens::prepare();
        for (const auto &x : api_tokens::tokens) {
          if (x.password && x.hash == t.hash) {
            bad_request(response, request, "That watch password is already in use. Choose another.");
            return;
          }
        }
        if (api_tokens::tokens.size() >= 50) {
          bad_request(response, request, "There are 50 tokens and watch passwords already. Remove some first.");
          return;
        }
        api_tokens::tokens.push_back(t);
        api_tokens::save();
      }
      BOOST_LOG(info) << "Watch password made: "sv << t.name;
      nlohmann::json out;
      out["status"] = true;
      out["id"] = t.id;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief POST /api/watch/password {"password"}: from the Watch page on a device, no sign-in.
   *        A right watch password gives this device its own watch key, {"key", "rotateAfter"}.
   *        Five wrong tries from one address wait ten minutes.
   */
  void postWatchPasswordUse(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }
    const auto address = net::addr_to_normalized_string(request->remote_endpoint().address());
    if (watch_passwords::blocked(address)) {
      bad_request(response, request, "Too many wrong tries. Wait ten minutes, then try again.");
      return;
    }
    try {
      auto input = nlohmann::json::parse(watch_keys::read_body(request));
      const auto hash = api_tokens::hash_of("watch-password:" + input.value("password", std::string {}));
      std::lock_guard lg {api_tokens::lock};
      api_tokens::prepare();
      std::string parent_id;
      std::string parent_name;
      for (auto &t : api_tokens::tokens) {
        if (t.password && t.hash == hash) {
          parent_id = t.id;
          parent_name = t.name;
          t.last_used = api_tokens::now_s();
        }
      }
      if (parent_id.empty()) {
        watch_passwords::failed(address);
        bad_request(response, request, "That watch password isn't right.");
        return;
      }
      // Each device keeps at most a few keys from one password: the oldest go first
      std::vector<std::size_t> mine;
      for (std::size_t i = 0; i < api_tokens::tokens.size(); ++i) {
        if (api_tokens::tokens[i].parent == parent_id) {
          mine.push_back(i);
        }
      }
      if (mine.size() >= 10) {
        api_tokens::tokens.erase(api_tokens::tokens.begin() + (std::ptrdiff_t) mine.front());
      }
      api_tokens::token_t key;
      key.id = crypto::rand_alphabet(12);
      key.name = parent_name;
      key.scopes = {"watch"};
      key.rotating = true;
      key.parent = parent_id;
      key.created = api_tokens::now_s();
      api_tokens::tokens.push_back(key);
      const auto fresh = watch_keys::renew_locked(api_tokens::tokens.back(), false);
      BOOST_LOG(info) << "Watch password used on a device: "sv << parent_name;
      nlohmann::json out;
      out["status"] = true;
      out["key"] = fresh;
      out["rotateAfter"] = api_tokens::ROTATE_EVERY;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief POST /api/watch/rotate with a device's watch key: a new key for tomorrow, {"key",
   *        "rotateAfter"}. The old key stops working a few minutes later.
   */
  void postWatchRotate(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    const auto hash = api_tokens::hash_of(api_tokens::from_request(request));
    std::lock_guard lg {api_tokens::lock};
    api_tokens::prepare();
    for (auto &t : api_tokens::tokens) {
      if (t.hash == hash && t.rotating) {
        nlohmann::json out;
        out["status"] = true;
        out["key"] = watch_keys::renew_locked(t, true);
        out["rotateAfter"] = api_tokens::ROTATE_EVERY;
        send_response(response, out);
        return;
      }
    }
    bad_request(response, request, "This device's watch key isn't valid any more. Ask for a new watch link.");
  }

  /**
   * @brief The Watch page: the stream in a browser, to watch only.
   */
  void getWatchPage(resp_https_t response, req_https_t request) {
    // Open to the network like the sign-in page: it holds nothing, and what it shows needs the
    // password, a watch link or a device's watch key
    if (!checkIPOrigin(response, request)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "watch.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief GET /api/watch/share: Share my screen's state, {"on", "minutesLeft", "showingScreen", "viewers"}.
   */
  void getWatchShare(resp_https_t response, req_https_t request) {
    if (!authenticate_owner(response, request)) {
      return;
    }
    auto out = watch::sharing();
    out["monitors"] = watch::monitors();
    out["status"] = true;
    send_response(response, out);
  }

  /**
   * @brief GET /api/watch/viewers: who is watching now, {"viewers": [{"id", "name", "since",
   *        "signedIn"}], "sharper"}.
   */
  void getWatchViewers(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json out;
    out["status"] = true;
    out["viewers"] = watch::watching_now();
    out["sharper"] = watch::sharper_picture();
    send_response(response, out);
  }

  /**
   * @brief POST /api/watch/viewers/remove {"id"}: stop one viewer and take away its access: its
   *        watch key or token, and the watch password it came from. Only with the password.
   */
  void postWatchViewerRemove(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      const auto token_id = watch::remove_viewer(input.value("id", std::string {}));
      if (!token_id.empty()) {
        const auto name = api_tokens::remove_with_source(token_id);
        BOOST_LOG(info) << "Watch access removed: "sv << name;
      }
      nlohmann::json out;
      out["status"] = true;
      out["viewers"] = watch::watching_now();
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief POST /api/watch/quality {"sharper": true}: 1080p for browsers watching (720p when
   *        false). Kept across restarts. Only with the password.
   */
  void postWatchQuality(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      watch::set_sharper_picture(input.value("sharper", false));
      nlohmann::json out;
      out["status"] = true;
      out["sharper"] = watch::sharper_picture();
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief POST /api/watch/share: {"on": true, "minutes": 120} shows this PC's own screen to
   *        browsers watching while nobody streams; {"on": false} stops. Only with the password.
   */
  void postWatchShare(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate_owner(response, request)) {
      return;
    }
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      const bool on = input.value("on", false);
      if (on && !video::parallel_encoding()) {
        bad_request(response, request, "This PC's video encoder can't make a second picture at the same time. Sharing needs a graphics card encoder.");
        return;
      }
      watch::share_screen(on, input.value("minutes", 120), input.value("monitor", std::string {}));
      auto out = watch::sharing();
      out["status"] = true;
      send_response(response, out);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief GET /api/watch/stream: the stream for the Watch page, as one long response of short
   *        messages (see watch.cpp). Ends when the stream does, or the browser goes away.
   */
  void getWatchStream(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    // Who: the watch key's or token's name, or the owner, with a hint at the device
    const auto [token_id, token_name] = api_tokens::who(api_tokens::from_request(request));
    std::string name = token_id.empty() ? std::string {"Signed in with the password"} : token_name;
    if (const auto hint = api_tokens::device_hint(request); !hint.empty()) {
      name += " (" + hint + ")";
    }
    std::string error;
    auto viewer = watch::join(error, name, token_id);
    if (!viewer) {
      nlohmann::json out;
      out["status"] = false;
      out["error"] = error;
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      response->write(SimpleWeb::StatusCode::client_error_conflict, out.dump(), headers);
      return;
    }

    response->close_connection_after_response = true;
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/octet-stream");
    headers.emplace("Cache-Control", "no-store");
    headers.emplace("X-Content-Type-Options", "nosniff");
    response->write(SimpleWeb::StatusCode::success_ok, headers);

    // Sending happens on a thread of its own, one write at a time, so a slow browser only ever
    // holds one write: what it can't take is skipped ahead to the next key frame in watch.cpp.
    std::thread([response, viewer]() {
      struct flow_t {
        std::mutex lock;
        std::condition_variable cv;
        bool in_flight = false;
        bool failed = false;
      };
      auto flow = std::make_shared<flow_t>();
      auto done = util::fail_guard([&]() {
        watch::leave(viewer);
      });

      auto send = [&](std::string &&bytes) -> bool {
        {
          std::unique_lock lk {flow->lock};
          flow->cv.wait_for(lk, 5s, [&]() {
            return !flow->in_flight || flow->failed;
          });
          if (flow->failed || flow->in_flight) {
            return false;
          }
          flow->in_flight = true;
        }
        response->write(bytes.data(), (std::streamsize) bytes.size());
        response->send([flow](const SimpleWeb::error_code &ec) {
          std::lock_guard lk {flow->lock};
          flow->in_flight = false;
          if (ec) {
            flow->failed = true;
          }
          flow->cv.notify_all();
        });
        return true;
      };

      if (!send(std::string {})) {
        return;
      }
      std::string message;
      while (watch::next(*viewer, message, 1s)) {
        if (message.empty()) {
          continue;
        }
        // Whatever else is ready goes in the same write
        std::string batch = std::move(message);
        while (batch.size() < 256 * 1024 && watch::next(*viewer, message, 0ms) && !message.empty()) {
          batch += message;
        }
        if (!send(std::move(batch))) {
          break;
        }
      }
      std::unique_lock lk {flow->lock};
      flow->cv.wait_for(lk, 2s, [&]() {
        return !flow->in_flight;
      });
    }).detach();
  }

  namespace checklist {
    std::mutex lock;

    std::filesystem::path file() {
      return platf::appdata() / "test_checklist.json";
    }

    nlohmann::json load() {
      std::ifstream in {file()};
      if (!in) {
        return nlohmann::json::object();
      }
      try {
        auto j = nlohmann::json::parse(in);
        return j.is_object() ? j : nlohmann::json::object();
      } catch (const std::exception &) {
        return nlohmann::json::object();
      }
    }
  }  // namespace checklist

  /**
   * @brief What was found for each checklist item: {"results": {"<id>": {"state", "note", "at"}}}.
   */
  void getChecklist(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    std::scoped_lock guard {checklist::lock};
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["results"] = checklist::load();
    send_response(response, output_tree);
  }

  /**
   * @brief Record one item: {"id", "state": "works"|"problem"|"" (not tried), "note"}.
   *        {"reset": true} forgets everything.
   */
  void postChecklist(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      std::scoped_lock guard {checklist::lock};
      auto results = checklist::load();
      if (input.value("reset", false)) {
        results = nlohmann::json::object();
      } else {
        const auto id = input.value("id", std::string {});
        const auto state = input.value("state", std::string {});
        auto note = input.value("note", std::string {});
        const bool plain_id = !id.empty() && id.size() <= 40 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
          return std::isalnum(c) || c == '_';
        });
        if (!plain_id || (state != "works" && state != "problem" && !state.empty())) {
          bad_request(response, request, "Unknown checklist item or result");
          return;
        }
        if (note.size() > 1000) {
          note.resize(1000);
        }
        if (state.empty()) {
          results.erase(id);
        } else if (results.size() < 200 || results.contains(id)) {
          const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
          results[id] = {{"state", state}, {"note", note}, {"at", now}};
        }
      }
      std::error_code ec;
      const auto path = checklist::file();
      auto partial = path;
      partial += ".part";
      {
        std::ofstream out {partial, std::ios::trunc};
        out << results.dump();
      }
      std::filesystem::rename(partial, path, ec);
      nlohmann::json output_tree;
      output_tree["status"] = !ec;
      output_tree["results"] = results;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief The Live page: what's playing, on a phone.
   */
  void getLivePage(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request, true)) {
      return;
    }

    print_req(request);

    std::string content = file_handler::read_file(WEB_DIR "live.html");
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/html; charset=utf-8");
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(content, headers);
  }

  /**
   * @brief An app's picture, by name, for the Live page: GET /api/cover?app=...
   */
  void getCover(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    auto args = request->parse_query_string();
    const auto name = args.find("app") != args.end() ? args.find("app")->second : std::string {};
    std::string path;
    for (const auto &app : proc::proc.get_apps()) {
      if (app.name == name) {
        try {
          path = proc::proc.get_app_image(std::stoi(app.id));
        } catch (const std::exception &) {
        }
        break;
      }
    }
    if (path.empty()) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
      return;
    }
    std::ifstream in(path, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", box_art::content_type_of(path));
    headers.emplace("Cache-Control", "private, max-age=600");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Check if a path is a child of another path.
   * @param base The base path.
   * @param query The path to check.
   * @return True if the path is a child of the base path, false otherwise.
   */
  bool isChildPath(fs::path const &base, fs::path const &query) {
    auto relPath = fs::relative(base, query);
    return *(relPath.begin()) != fs::path("..");
  }

  /**
   * @brief Get an asset from the node_modules directory.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getNodeModules(resp_https_t response, req_https_t request) {
    print_req(request);

    fs::path webDirPath(WEB_DIR);
    fs::path nodeModulesPath(webDirPath / "assets");

    // .relative_path is needed to shed any leading slash that might exist in the request path
    auto filePath = fs::weakly_canonical(webDirPath / fs::path(request->path).relative_path());

    // Don't do anything if file does not exist or is outside the assets directory
    if (!isChildPath(filePath, nodeModulesPath)) {
      BOOST_LOG(warning) << "Someone requested a path " << filePath << " that is outside the assets folder";
      bad_request(response, request);
      return;
    }

    if (!fs::exists(filePath)) {
      not_found(response, request);
      return;
    }

    auto relPath = fs::relative(filePath, webDirPath);
    // get the mime type from the file extension mime_types map
    // remove the leading period from the extension
    auto mimeType = mime_types.find(relPath.extension().string().substr(1));
    if (mimeType == mime_types.end()) {
      bad_request(response, request);
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", mimeType->second);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    std::ifstream in(filePath.string(), std::ios::binary);
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Get the list of available applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps| GET| null}
   */
  void getApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json file_tree = nlohmann::json::parse(content);

      file_tree["current_app"] = proc::proc.get_running_app_uuid();
      file_tree["host_uuid"] = http::unique_id;
      file_tree["host_name"] = config::nvhttp.tunastream_name;

      send_response(response, file_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "GetApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Save an application. To save a new application the UUID must be empty.
   *        To update an existing application, you must provide the current UUID of the application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "name": "Application Name",
   *   "output": "Log Output Path",
   *   "cmd": "Command to run the application",
   *   "exclude-global-prep-cmd": false,
   *   "elevated": false,
   *   "auto-detach": true,
   *   "wait-all": true,
   *   "exit-timeout": 5,
   *   "prep-cmd": [
   *     {
   *       "do": "Command to prepare",
   *       "undo": "Command to undo preparation",
   *       "elevated": false
   *     }
   *   ],
   *   "detached": [
   *     "Detached command"
   *   ],
   *   "image-path": "Full path to the application image. Must be a png file.",
   *   "uuid": "aaaa-bbbb"
   * }
   * @endcode
   *
   * @api_examples{/api/apps| POST| {"name":"Hello, World!","uuid": "aaaa-bbbb"}}
   */
  void saveApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();

    BOOST_LOG(info) << config::stream.file_apps;
    try {
      // TODO: Input Validation

      // Read the input JSON from the request body.
      nlohmann::json inputTree = nlohmann::json::parse(ss.str());

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Migrate/merge the new app into the file tree.
      proc::migrate_apps(&fileTree, &inputTree);

      // Write the updated file tree back to disk.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      pairing_backup::save();
      proc::refresh(config::stream.file_apps);

      // Prepare and send the output response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief List the games and programs installed on this PC, to offer as new apps.
   */
  void discoverApps(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json games = nlohmann::json::array();
    for (const auto &game : game_library::find_installed_games()) {
      games.push_back({
        {"source", game.source},
        {"id", game.id},
        {"name", game.name},
        {"cmd", game.cmd},
        {"detached", game.detached},
        {"workingDir", game.working_dir},
      });
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["games"] = games;
    send_response(response, output_tree);
  }

  /**
   * @brief Add several apps at once, as picked from the list of installed games and programs.
   * @details Body: {"apps": [{"name", "cmd", "detached", "workingDir"}]}. Refused while an app is
   *          running, because reloading the app list ends it.
   */
  void addApps(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    if (proc::proc.running()) {
      output_tree["status"] = false;
      output_tree["error"] = "A game is running. End the session first, then add apps.";
      send_response(response, output_tree);
      return;
    }

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());

      auto file_tree = nlohmann::json::parse(file_handler::read_file(config::stream.file_apps.c_str()));
      int added = 0;
      for (const auto &item : input.at("apps")) {
        auto name = item.value("name", "");
        auto cmd = item.value("cmd", "");
        auto detached = item.value("detached", "");
        if (name.empty() || (cmd.empty() && detached.empty())) {
          continue;
        }

        nlohmann::json app {
          {"name", name},
          {"cmd", cmd},
          {"working-dir", item.value("workingDir", "")},
          {"detached", detached.empty() ? nlohmann::json::array() : nlohmann::json::array({detached})},
          {"image-path", ""},
          {"auto-detach", true},
          {"wait-all", true},
          {"exit-timeout", 5},
          {"elevated", false},
          {"exclude-global-prep-cmd", false},
          {"exclude-global-state-cmd", false},
          {"prep-cmd", nlohmann::json::array()},
          {"state-cmd", nlohmann::json::array()},
          {"scale-factor", 100},
          {"allow-client-commands", true},
          {"virtual-display", false},
          {"terminate-on-pause", false},
        };
        proc::migrate_apps(&file_tree, &app);
        ++added;
      }

      file_handler::write_file(config::stream.file_apps.c_str(), file_tree.dump(4));
      pairing_backup::save();
      proc::refresh(config::stream.file_apps);
      BOOST_LOG(info) << "Added "sv << added << " apps from this PC"sv;

      output_tree["status"] = true;
      output_tree["added"] = added;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "AddApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Close the currently running application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/close| POST| null}
   */
  void closeApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Rumble the controllers of every connected client for a moment.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/rumble-test| POST| null}
   */
  void rumbleTest(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["clients"] = stream::session::test_rumble();
    send_response(response, output_tree);
  }

  /**
   * @brief What is being streamed right now: the running app and each connected controller.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/status| GET| null}
   */
  /**
   * @brief The weekly recap: play time per day, most-played apps and how smooth the streams were.
   * @details Query: days (default 7), tz (the viewer's offset from UTC in minutes, east positive).
   */
  void getRecap(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    int days = 7;
    int tz = 0;
    auto args = request->parse_query_string();
    try {
      if (auto it = args.find("days"); it != args.end()) {
        days = std::stoi(it->second);
      }
      if (auto it = args.find("tz"); it != args.end()) {
        tz = std::stoi(it->second);
      }
    } catch (const std::exception &) {
      // Keep the defaults
    }

    auto output_tree = play_history::recap_with_milestones(days, tz);
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Show a short message on the screen of a device that is streaming.
   * @details Body: {"text": "...", "device": "name"} (device empty or left out: every device
   *          that is streaming). TunaReceive shows it within about five seconds.
   */
  void postTvMessage(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      const auto text = input.value("text", "");
      const auto device = input.value("device", "");
      const auto sound = input.value("sound", "");
      if (!sound.empty() && !sound_board::file_for(sound)) {
        bad_request(response, request, "There is no sound called " + sound);
        return;
      }

      std::vector<std::string> streaming;
      for (const auto &h : stream::session::health()) {
        if (device.empty() || h.device == device) {
          streaming.push_back(h.device);
        }
      }
      if (streaming.empty()) {
        output_tree["status"] = false;
        output_tree["error"] = device.empty() ? "Nobody is streaming right now." : device + " isn't streaming right now.";
        send_response(response, output_tree);
        return;
      }
      const auto id = tv_messages::post(device, text, sound);
      if (!id) {
        bad_request(response, request, "The message is empty");
        return;
      }
      BOOST_LOG(info) << "Message for "sv << (device.empty() ? "every streaming device"s : device) << " queued"sv;
      output_tree["status"] = true;
      output_tree["id"] = id;
      output_tree["devices"] = streaming;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "TvMessage: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief The newest screenshots and clips: {"captures": [{"name", "kind", "modified", "size"}]}.
   */
  void getCaptures(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto &c : captures::recent(24)) {
      list.push_back({{"name", c.name}, {"kind", c.kind}, {"modified", c.modified}, {"size", c.size}});
    }
    auto folder = [](const std::filesystem::path &p) {
      auto u8 = p.u8string();
      return std::string(u8.begin(), u8.end());
    };
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["captures"] = list;
    output_tree["screenshotFolder"] = folder(captures::screenshot_dir());
    output_tree["clipFolder"] = folder(captures::clip_dir());
    send_response(response, output_tree);
  }

  /**
   * @brief Make this week's highlights from the instant replays saved: POST /api/highlights.
   */
  void postHighlights(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::string name;
    auto error = captures::make_highlights(name);
    nlohmann::json output_tree;
    output_tree["status"] = error.empty();
    if (error.empty()) {
      output_tree["name"] = name;
    } else {
      output_tree["error"] = error;
    }
    send_response(response, output_tree);
  }

  /**
   * @brief One screenshot or clip: GET /api/captures/file?kind=screenshot|clip&name=...
   */
  void getCaptureFile(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    auto args = request->parse_query_string();
    auto kind = args.find("kind") != args.end() ? args.find("kind")->second : std::string {};
    auto name = args.find("name") != args.end() ? args.find("name")->second : std::string {};
    auto path = captures::file_for(kind, name);
    if (path.empty()) {
      response->write(SimpleWeb::StatusCode::client_error_not_found);
      return;
    }
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    const char *type = ext == ".png" ? "image/png" : (ext == ".jpg" || ext == ".jpeg") ? "image/jpeg" : ext == ".mp4" ? "video/mp4" : ext == ".mkv" ? "video/x-matroska" : "application/octet-stream";
    std::ifstream in(path, std::ios::binary);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", type);
    headers.emplace("Cache-Control", "private, max-age=3600");
    response->write(SimpleWeb::StatusCode::success_ok, in, headers);
  }

  /**
   * @brief Wi-Fi networks this PC can hear, for the channel advice: GET /api/wifi?scan=1.
   * @details The adapter is only asked to scan when nobody is streaming, since a scan briefly
   *          takes the Wi-Fi off its channel.
   */
  void getWifi(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    auto args = request->parse_query_string();
    const bool asked = args.find("scan") != args.end();
    const bool streaming = rtsp_stream::session_count() > 0;
    const auto survey = platf::wifi_survey(asked && !streaming);
    nlohmann::json networks = nlohmann::json::array();
    for (const auto &n : survey.networks) {
      networks.push_back({{"name", n.name}, {"channel", n.channel}, {"band", n.band}, {"signal", n.signal}, {"connected", n.connected}});
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["available"] = survey.available;
    output_tree["scanned"] = asked && !streaming;
    output_tree["streaming"] = streaming;
    output_tree["networks"] = networks;
    send_response(response, output_tree);
  }

  /**
   * @brief Play-time limits: GET lists every device with its limits and today's time.
   */
  void getPlayLimits(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["devices"] = play_limits::status();
    send_response(response, output_tree);
  }

  /**
   * @brief Set the limits: {"devices": {"<name>": {"weekday": minutes, "weekend": minutes}}}.
   */
  void postPlayLimits(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      play_limits::set_limits(input.value("devices", nlohmann::json::object()));
      nlohmann::json output_tree;
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Give a device more time today: {"device", "minutes"}.
   */
  void postPlayExtend(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      play_limits::extend(input.value("device", ""), input.value("minutes", 15));
      nlohmann::json output_tree;
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief The sound board: {"sounds": [{"name", "builtIn"}], "maxBytes"}.
   */
  void getSounds(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json sounds = nlohmann::json::array();
    for (const auto &sound : sound_board::list()) {
      sounds.push_back({{"name", sound.name}, {"builtIn", sound.built_in}});
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["sounds"] = sounds;
    output_tree["maxBytes"] = sound_board::MAX_BYTES;
    send_response(response, output_tree);
  }

  /**
   * @brief Add a sound to the sound board. Body: {"name", "extension", "data" (base64)}.
   */
  void addSound(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      auto bytes = SimpleWeb::Crypto::Base64::decode(input.value("data", ""));
      auto error = sound_board::add(input.value("name", ""), input.value("extension", ""), bytes);
      output_tree["status"] = error.empty();
      if (!error.empty()) {
        output_tree["error"] = error;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "AddSound: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Remove one of your own sounds. Body: {"name"}.
   */
  void removeSound(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      output_tree["status"] = sound_board::remove(input.value("name", ""));
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Which devices recent messages have reached: {"messages": [{"id", "deliveredTo": [...]}]}.
   */
  void getTvMessages(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json messages = nlohmann::json::array();
    for (const auto &[id, devices] : tv_messages::delivered()) {
      messages.push_back({{"id", id}, {"deliveredTo", devices}});
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["messages"] = messages;
    send_response(response, output_tree);
  }

  void getStatus(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["app"] = proc::proc.get_last_run_app_name();
    // Browsers watching the stream from the Watch page
    output_tree["watchers"] = watch::viewers();
    output_tree["sharingScreen"] = watch::sharing();
    output_tree["sessions"] = rtsp_stream::session_count();
    output_tree["version"] = PROJECT_VERSION;
    nlohmann::json controllers = nlohmann::json::array();
    for (const auto &gamepad : input::gamepad_status()) {
      controllers.push_back({
        {"device", gamepad.device},
        {"controller", gamepad.controller},
        {"emulated", gamepad.emulated},
        {"battery", gamepad.battery},
        {"charging", gamepad.charging},
      });
    }
    output_tree["controllers"] = controllers;

    nlohmann::json streams = nlohmann::json::array();
    for (const auto &h : stream::session::health()) {
      streams.push_back({
        {"device", h.device},
        {"targetFps", h.target_fps},
        {"fps", h.fps},
        {"skippedPerSec", h.skipped_per_sec},
        {"gaps", h.gaps},
        {"pcMsAvg", h.pc_ms_avg},
        {"pcMsMax", h.pc_ms_max},
        {"sendMsAvg", h.send_ms_avg},
        {"sendMsMax", h.send_ms_max},
        {"recoveries", h.recoveries},
        {"bitrateKbps", h.bitrate_kbps},
        {"sendRateMbps", h.send_rate_mbps},
        {"fecPercent", h.fec_percent},
        {"pictureScale", h.picture_scale},
        {"splitFrames", h.split_frames},
        {"windowS", h.window_s},
        {"slowGameMinutes", h.slow_game_minutes},
        {"slowGameFps", h.slow_game_fps},
        {"width", h.width},
        {"height", h.height},
        {"runningS", h.running_s},
      });
      if (h.view.known) {
        streams.back()["deviceView"] = {
          {"lossPct", h.view.loss_pct},
          {"slowPct", h.view.slow_pct},
          {"fps", h.view.fps},
          {"decodeMs", h.view.decode_ms},
          {"latencyMs", h.view.latency_ms},
          {"kbps", h.view.kbps},
          {"settingKbps", h.view.setting_kbps},
          {"hdrPeakNits", h.view.hdr_peak_nits},
          {"avgFps", h.view.avg_fps},
          {"late30Pct", h.view.late30_pct},
          {"lossSeconds", h.view.loss_seconds},
          {"scalePct", h.view.scale_pct},
          {"codec", h.view.codec},
          {"bitDepth", h.view.bit_depth},
          {"rttMs", h.view.rtt_ms},
          {"rttVarMs", h.view.rtt_var_ms},
          {"net", h.view.net},
          {"band", h.view.band},
          {"signal", h.view.signal},
          {"rssiDbm", h.view.rssi_dbm},
          {"linkMbps", h.view.link_mbps},
        };
      }
    }
    output_tree["streams"] = streams;

    // How this PC reaches the network. Asking Windows is slow enough to be worth remembering for a while.
    {
      static std::mutex network_lock;
      static platf::network_link_t network;
      static std::chrono::steady_clock::time_point network_checked;
      std::lock_guard lg {network_lock};
      const auto now = std::chrono::steady_clock::now();
      if (network_checked == std::chrono::steady_clock::time_point {} || now - network_checked > std::chrono::seconds(30)) {
        network = platf::network_link();
        network_checked = now;
      }
      if (network.known) {
        output_tree["network"] = {
          {"wifi", network.wifi},
          {"name", network.name},
          {"mbps", network.mbps},
          {"signal", network.signal},
          {"channel", network.channel},
        };
      }
    }

    nlohmann::json hosts = nlohmann::json::array();
    for (const auto &host : other_hosts::installed()) {
      hosts.push_back({{"name", host.name}, {"service", host.service}, {"running", host.running}});
    }
    output_tree["otherHosts"] = hosts;
#ifdef _WIN32
    output_tree["vdisplayFailed"] = proc::vdisplay_failed.load();
    output_tree["vdisplayStatus"] = (int) proc::vDisplayDriverStatus;
    output_tree["vdisplayRepairing"] = proc::vdisplay_repair_running();
    output_tree["onVirtualDisplay"] = proc::proc.on_virtual_display();
    output_tree["displayRestoreFailed"] = proc::display_restore_failed.load();
#endif
    send_response(response, output_tree);
  }

  /**
   * @brief Start installing the latest release, or report how far an update has got.
   * @details POST starts it; GET reports progress. The release is always looked up by the
   *          server itself, never taken from the request.
   *
   * @api_examples{/api/update| POST| null}
   */
  /**
   * @brief Reinstall the virtual display driver and reconnect to it.
   * @details Refused while streaming, since reinstalling removes the stream's screen.
   *          Progress shows in /api/status as vdisplayRepairing and vdisplayStatus.
   */
  void repairVirtualDisplay(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
#ifdef _WIN32
    if (rtsp_stream::session_count() > 0) {
      output_tree["status"] = false;
      output_tree["error"] = "Stop streaming first: repairing removes the stream's screen.";
    } else {
      output_tree["status"] = true;
      output_tree["started"] = proc::start_vdisplay_repair();
    }
#else
    output_tree["status"] = false;
    output_tree["error"] = "Only available on Windows.";
#endif
    send_response(response, output_tree);
  }

  /**
   * @brief Move the paused app from the stream's screen to this PC's own monitor.
   */
  void continueHere(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    if (rtsp_stream::session_count() > 0) {
      output_tree["status"] = false;
      output_tree["error"] = "Someone is still streaming it.";
    } else if (proc::proc.continue_on_host()) {
      output_tree["status"] = true;
    } else {
      output_tree["status"] = false;
      output_tree["error"] = "There is no paused game on the stream's screen to move.";
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Pause or resume Sunshine or Apollo when they are installed next to TunaStream.
   * @details Body: {"service": "SunshineService", "run": false}. Only those two services are accepted.
   */
  void setOtherHost(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      auto error = other_hosts::set_running(input.value("service", ""), input.value("run", false));
      output_tree["status"] = error.empty();
      if (!error.empty()) {
        output_tree["error"] = error;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  void startUpdate(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    // {"verifyOnly": true} downloads and checks the latest installer without installing it
    bool verify_only = false;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto body = ss.str();
      if (!body.empty()) {
        auto input = nlohmann::json::parse(body);
        verify_only = input.is_object() && input.value("verifyOnly", false);
      }
    } catch (const std::exception &) {
      // An unreadable body just means a normal update
    }

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["started"] = updater::start(verify_only);
    send_response(response, output_tree);
  }

  void getUpdate(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    auto state = updater::state();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["stage"] = state.stage;
    output_tree["version"] = state.version;
    output_tree["error"] = state.error;
    send_response(response, output_tree);
  }

  /**
   * @brief Reorder applications.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/reorder| POST| {"order": ["aaaa-bbbb", "cccc-dddd"]}}
   */
  void reorderApps(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();

      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;

      // Read the existing apps file.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Get the desired order of UUIDs from the request.
      if (!input_tree.contains("order") || !input_tree["order"].is_array()) {
        throw std::runtime_error("Missing or invalid 'order' array in request body");
      }
      const auto& order_uuids_json = input_tree["order"];

      // Get the original apps array from the fileTree.
      // Default to an empty array if "apps" key is missing or if it's present but not an array (after logging an error).
      nlohmann::json original_apps_list = nlohmann::json::array();
      if (fileTree.contains("apps")) {
        if (fileTree["apps"].is_array()) {
          original_apps_list = fileTree["apps"];
        } else {
          // "apps" key exists but is not an array. This is a malformed state.
          BOOST_LOG(error) << "ReorderApps: 'apps' key in apps configuration file ('" << config::stream.file_apps
                           << "') is present but not an array.";
          throw std::runtime_error("'apps' in file is not an array, cannot reorder.");
        }
      } else {
        // "apps" key is missing. Treat as an empty list. Reordering an empty list is valid.
        BOOST_LOG(debug) << "ReorderApps: 'apps' key missing in apps configuration file ('" << config::stream.file_apps
                         << "'). Treating as an empty list for reordering.";
        // original_apps_list is already an empty array, so no specific action needed here.
      }

      nlohmann::json reordered_apps_list = nlohmann::json::array();
      std::vector<bool> item_moved(original_apps_list.size(), false);

      // Phase 1: Place apps according to the 'order' array from the request.
      // Iterate through the desired order of UUIDs.
      for (const auto& uuid_json_value : order_uuids_json) {
        if (!uuid_json_value.is_string()) {
          BOOST_LOG(warning) << "ReorderApps: Encountered a non-string UUID in the 'order' array. Skipping this entry.";
          continue;
        }
        std::string target_uuid = uuid_json_value.get<std::string>();
        bool found_match_for_ordered_uuid = false;

        // Find the first unmoved app in the original list that matches the current target_uuid.
        for (size_t i = 0; i < original_apps_list.size(); ++i) {
          if (item_moved[i]) {
            continue; // This specific app object has already been placed.
          }

          const auto& app_item = original_apps_list[i];
          // Ensure the app item is an object and has a UUID to match against.
          if (app_item.is_object() && app_item.contains("uuid") && app_item["uuid"].is_string()) {
            if (app_item["uuid"].get<std::string>() == target_uuid) {
              reordered_apps_list.push_back(app_item); // Add the found app object to the new list.
              item_moved[i] = true;                    // Mark this specific object as moved.
              found_match_for_ordered_uuid = true;
              break; // Found an app for this UUID, move to the next UUID in the 'order' array.
            }
          }
        }

        if (!found_match_for_ordered_uuid) {
          // This means a UUID specified in the 'order' array was not found in the original_apps_list
          // among the currently available (unmoved) app objects.
          // Per instruction "If the uuid is missing from the original json file, omit it."
          BOOST_LOG(debug) << "ReorderApps: UUID '" << target_uuid << "' from 'order' array not found in available apps list or its matching app was already processed. Omitting.";
        }
      }

      // Phase 2: Append any remaining apps from the original list that were not explicitly ordered.
      // These are app objects that were not marked 'item_moved' in Phase 1.
      for (size_t i = 0; i < original_apps_list.size(); ++i) {
        if (!item_moved[i]) {
          reordered_apps_list.push_back(original_apps_list[i]);
        }
      }

      // Update the fileTree with the new, reordered list of apps.
      fileTree["apps"] = reordered_apps_list;

      // Write the modified fileTree back to the apps configuration file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      pairing_backup::save();

      // Notify relevant parts of the system that the apps configuration has changed.
      proc::refresh(config::stream.file_apps);

      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "ReorderApps: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Delete an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/apps/delete | POST| { uuid: 'aaaa-bbbb' }}
   */
  void deleteApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      auto uuid = input_tree["uuid"].get<std::string>();

      // Read the apps file into a nlohmann::json object.
      std::string content = file_handler::read_file(config::stream.file_apps.c_str());
      nlohmann::json fileTree = nlohmann::json::parse(content);

      // Remove any app with the matching uuid directly from the "apps" array.
      if (fileTree.contains("apps") && fileTree["apps"].is_array()) {
        auto& apps = fileTree["apps"];
        apps.erase(
          std::remove_if(apps.begin(), apps.end(), [&uuid](const nlohmann::json& app) {
            return app.value("uuid", "") == uuid;
          }),
          apps.end()
        );
      }

      // Write the updated JSON back to the file.
      file_handler::write_file(config::stream.file_apps.c_str(), fileTree.dump(4));
      pairing_backup::save();
      proc::refresh(config::stream.file_apps);

      // Prepare and send the response.
      nlohmann::json outputTree;
      outputTree["status"] = true;
      send_response(response, outputTree);
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "DeleteApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the list of paired clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/list| GET| null}
   */
  void getClients(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json named_certs = nvhttp::get_all_clients();
    nlohmann::json output_tree;
    output_tree["named_certs"] = named_certs;
#ifdef _WIN32
    output_tree["platform"] = "windows";
#endif
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Update client information.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "uuid": "<uuid>",
   *   "name": "<Friendly Name>",
   *   "display_mode": "1920x1080x59.94",
   *   "do": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "undo": [ { "cmd": "<command>", "elevated": false }, ... ],
   *   "perm": <uint32_t>
   * }
   * @endcode
   */
  void updateClient(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      std::string name = input_tree.value("name", "");
      std::string display_mode = input_tree.value("display_mode", "");
      bool enable_legacy_ordering = input_tree.value("enable_legacy_ordering", true);
      bool allow_client_commands = input_tree.value("allow_client_commands", true);
      bool always_use_virtual_display = input_tree.value("always_use_virtual_display", false);
      auto do_cmds = nvhttp::extract_command_entries(input_tree, "do");
      auto undo_cmds = nvhttp::extract_command_entries(input_tree, "undo");
      auto perm = static_cast<crypto::PERM>(input_tree.value("perm", static_cast<uint32_t>(crypto::PERM::_no)) & static_cast<uint32_t>(crypto::PERM::_all));
      output_tree["status"] = nvhttp::update_device_info(
        uuid,
        name,
        display_mode,
        do_cmds,
        undo_cmds,
        perm,
        enable_legacy_ordering,
        allow_client_commands,
        always_use_virtual_display
      );
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Update Client: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *  "uuid": "<uuid>"
   * }
   * @endcode
   *
   * @api_examples{/api/clients/unpair| POST| {"uuid":"1234"}}
   */
  void unpair(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::unpair_client(uuid);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Unpair: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Unpair all clients.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/clients/unpair-all| POST| null}
   */
  void unpairAll(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nvhttp::erase_all_clients();
    proc::proc.terminate();
    nlohmann::json output_tree;
    output_tree["status"] = true;
    send_response(response, output_tree);
  }

  /**
   * @brief Get the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void getConfig(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["platform"] = TUNASTREAM_PLATFORM;
    output_tree["version"] = PROJECT_VERSION;
    // For the phone QR code on the home page
    output_tree["lanAddress"] = platf::get_local_ip_for_gateway();
#ifdef _WIN32
    output_tree["vdisplayStatus"] = (int)proc::vDisplayDriverStatus;
#endif
    auto vars = config::parse_config(file_handler::read_file(config::tunastream.config_file.c_str()));
    for (auto &[name, value] : vars) {
      output_tree[name] = value;
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Get the locale setting.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/configLocale| GET| null}
   */
  void getLocale(resp_https_t response, req_https_t request) {
    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["locale"] = config::tunastream.locale;
    // Every page asks for this first, so the colour theme comes along with it
    output_tree["theme"] = config::effective_theme();
    send_response(response, output_tree);
  }

  /**
   * @brief Save the configuration settings.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   * The body for the post request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "key": "value"
   * }
   * @endcode
   *
   * @attention{It is recommended to ONLY save the config settings that differ from the default behavior.}
   *
   * @api_examples{/api/config| POST| {"key":"value"}}
   */
  void saveConfig(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      // TODO: Input Validation
      std::stringstream config_stream;
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss);
      for (const auto &[k, v] : input_tree.items()) {
        if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
          continue;
        }

        // v.dump() will dump valid json, which we do not want for strings in the config right now
        // we should migrate the config file to straight json and get rid of all this nonsense
        config_stream << k << " = " << (v.is_string() ? v.get<std::string>() : v.dump()) << std::endl;
      }
      file_handler::write_file(config::tunastream.config_file.c_str(), config_stream.str());
      pairing_backup::save();
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SaveConfig: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send to device: put a file in a paired device's outbox. The device collects it the
   *        next time it checks (TunaReceive checks while it's open).
   *        The body is the file itself; the device and the name are in the query string.
   *
   * @api_examples{/api/files/send?uuid=ABC&name=photo.jpg| POST| (file bytes)}
   */
  void sendFile(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/octet-stream") || !authenticate(response, request)) {
      return;
    }
    print_req(request);

    auto args = request->parse_query_string();
    auto uuid_it = args.find("uuid");
    auto name_it = args.find("name");
    if (uuid_it == args.end() || name_it == args.end()) {
      bad_request(response, request, "Choose a device and a file");
      return;
    }
    const std::string uuid = uuid_it->second;
    const std::string name = name_it->second;

    bool paired = false;
    for (auto &client : nvhttp::get_all_clients()) {
      if (client.value("uuid", "") == uuid) {
        paired = true;
      }
    }
    if (!paired) {
      bad_request(response, request, "That device isn't paired");
      return;
    }

    std::string error;
    auto id = file_outbox::add(uuid, name, request->content, error);
    nlohmann::json output_tree;
    output_tree["status"] = !id.empty();
    if (id.empty()) {
      output_tree["error"] = error;
    } else {
      output_tree["id"] = id;
    }
    send_response(response, output_tree);
  }

  /**
   * @brief Send to device: files still waiting for each device, as {"devices": {uuid: [{id, name, size}]}}.
   *
   * @api_examples{/api/files| GET| null}
   */
  void getFiles(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    print_req(request);
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["devices"] = file_outbox::all();
    output_tree["maxSize"] = file_outbox::max_file_size;
    send_response(response, output_tree);
  }

  /**
   * @brief Send to device: take back a file the device hasn't collected yet.
   *
   * @api_examples{/api/files/cancel| POST| {"uuid":"ABC","id":"1764012345678-photo.jpg"}}
   */
  void cancelFile(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }
    print_req(request);
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      auto input = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      output_tree["status"] = file_outbox::remove(input.value("uuid", ""), input.value("id", ""));
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Upload a cover image.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/covers/upload| POST| {"key":"igdb_1234","url":"https://images.igdb.com/igdb/image/upload/t_cover_big_2x/abc123.png"}}
   */
  void uploadCover(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    std::stringstream ss;

    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string key = input_tree.value("key", "");
      if (key.empty()) {
        bad_request(response, request, "Cover key is required");
        return;
      }
      std::string url = input_tree.value("url", "");
      const std::string coverdir = platf::appdata().string() + "/covers/";
      file_handler::make_directory(coverdir);
      std::string path = coverdir + http::url_escape(key) + ".png";
      if (!url.empty()) {
        if (http::url_get_host(url) != "images.igdb.com") {
          bad_request(response, request, "Only images.igdb.com is allowed");
          return;
        }
        if (!http::download_file(url, path)) {
          bad_request(response, request, "Failed to download cover");
          return;
        }
      } else {
        auto data = SimpleWeb::Crypto::Base64::decode(input_tree.value("data", ""));
        std::ofstream imgfile(path);
        imgfile.write(data.data(), static_cast<int>(data.size()));
      }
      output_tree["status"] = true;
      output_tree["path"] = path;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "UploadCover: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get the logs from the log file.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/logs| GET| null}
   */
  void getLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);
    std::string content = file_handler::read_file(config::tunastream.log_file.c_str());
    SimpleWeb::CaseInsensitiveMultimap headers;
    std::string contentType = "text/plain";
  #ifdef _WIN32
    contentType += "; charset=";
    contentType += currentCodePageToCharset();
  #endif
    headers.emplace("Content-Type", contentType);
    headers.emplace("X-Frame-Options", "DENY");
    headers.emplace("Content-Security-Policy", "frame-ancestors 'none';");
    response->write(SimpleWeb::StatusCode::success_ok, content, headers);
  }

  /**
   * @brief Update existing credentials.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "currentUsername": "Current Username",
   *   "currentPassword": "Current Password",
   *   "newUsername": "New Username",
   *   "newPassword": "New Password",
   *   "confirmNewPassword": "Confirm New Password"
   * }
   * @endcode
   *
   * @api_examples{/api/password| POST| {"currentUsername":"admin","currentPassword":"admin","newUsername":"admin","newPassword":"admin","confirmNewPassword":"admin"}}
   */
  void savePassword(resp_https_t response, req_https_t request) {
    if ((!config::tunastream.username.empty() && !authenticate(response, request)) || !validateContentType(response, request, "application/json"))
      return;
    print_req(request);
    std::vector<std::string> errors;
    std::stringstream ss;
    ss << request->content.rdbuf();
    try {
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string username = input_tree.value("currentUsername", "");
      std::string newUsername = input_tree.value("newUsername", "");
      std::string password = input_tree.value("currentPassword", "");
      std::string newPassword = input_tree.value("newPassword", "");
      std::string confirmPassword = input_tree.value("confirmNewPassword", "");
      if (newUsername.empty())
        newUsername = username;
      if (newUsername.empty()) {
        errors.push_back("Invalid Username");
      } else {
        auto hash = util::hex(crypto::hash(password + config::tunastream.salt)).to_string();
        if (config::tunastream.username.empty() ||
            (boost::iequals(username, config::tunastream.username) && hash == config::tunastream.password)) {
          if (newPassword.empty() || newPassword != confirmPassword)
            errors.push_back("Password Mismatch");
          else {
            http::save_user_creds(config::tunastream.credentials_file, newUsername, newPassword);
            http::reload_user_creds(config::tunastream.credentials_file);
            web_sessions::clear();  // everyone signs in with the new password
            output_tree["status"] = true;
          }
        } else {
          errors.push_back("Invalid Current Credentials");
        }
      }
      if (!errors.empty()) {
        std::string error = std::accumulate(errors.begin(), errors.end(), std::string(),
                                              [](const std::string &a, const std::string &b) {
                                                return a.empty() ? b : a + ", " + b;
                                              });
        bad_request(response, request, error);
        return;
      }
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePassword: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Get a one-time password (OTP).
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/otp| GET| null}
   */
  void getOTP(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      std::string passphrase = input_tree.value("passphrase", "");
      if (passphrase.empty())
        throw std::runtime_error("Passphrase not provided!");
      if (passphrase.size() < 4)
        throw std::runtime_error("Passphrase too short!");

      std::string deviceName = input_tree.value("deviceName", "");
      output_tree["otp"] = nvhttp::request_otp(passphrase, deviceName);
      output_tree["ip"] = platf::get_local_ip_for_gateway();
      output_tree["name"] = config::nvhttp.tunastream_name;
      output_tree["status"] = true;
      output_tree["message"] = "OTP created, effective within 3 minutes.";
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "OTP creation failed: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Send a PIN code to the host.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "pin": "<pin>",
   *   "name": "Friendly Client Name"
   * }
   * @endcode
   *
   * @api_examples{/api/pin| POST| {"pin":"1234","name":"My PC"}}
   */
  void savePin(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      nlohmann::json output_tree;
      std::string pin = input_tree.value("pin", "");
      std::string name = input_tree.value("name", "");
      output_tree["status"] = nvhttp::pin(pin, name);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "SavePin: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Reset the display device persistence.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/reset-display-device-persistence| POST| null}
   */
  void resetDisplayDevicePersistence(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    output_tree["status"] = display_device::reset_persistence();
    send_response(response, output_tree);
  }

  /**
   * @brief Restart TunaStream.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * @api_examples{/api/restart| POST| null}
   */
  void restart(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    proc::proc.terminate();

    // We may not return from this call
    platf::restart();
  }

  /**
   * @brief Download a backup of TunaStream's settings: GET /api/backup?pairings=1 to include pairings.
   */
  void getBackup(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    auto args = request->parse_query_string();
    const bool pairings = args.find("pairings") != args.end() && args.find("pairings")->second == "1";
    const auto backup = settings_backup::make(pairings);
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local {};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char date[16];
    std::strftime(date, sizeof(date), "%Y-%m-%d", &local);
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Content-Disposition", "attachment; filename=\"TunaStream backup "s + date + (pairings ? " with pairings"s : ""s) + ".json\"");
    headers.emplace("Cache-Control", "no-store");
    response->write(SimpleWeb::StatusCode::success_ok, backup.dump(), headers);
  }

  /**
   * @brief Restore a backup (the body is the backup file), then restart TunaStream.
   */
  void postRestore(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    nlohmann::json output_tree;
    std::string error;
    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      error = settings_backup::restore(nlohmann::json::parse(ss.str()));
    } catch (const std::exception &) {
      error = "That file isn't a TunaStream backup.";
    }
    output_tree["status"] = error.empty();
    if (!error.empty()) {
      output_tree["error"] = error;
      send_response(response, output_tree);
      return;
    }
    send_response(response, output_tree);

    // Restart a moment later so the answer gets back first; the restored settings load on start
    std::thread([]() {
      std::this_thread::sleep_for(1500ms);
      proc::proc.terminate();
      platf::restart();
    }).detach();
  }

  /**
   * @brief TunaReceive backups kept on this PC: {"backups": [{"name", "device", "made", "size"}]}.
   */
  void getReceiverBackups(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto &b : settings_backup::receiver_backups()) {
      list.push_back({{"name", b.name}, {"device", b.device}, {"made", b.made}, {"size", b.size}});
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["backups"] = list;
    send_response(response, output_tree);
  }

  /**
   * @brief Logs sent from TVs (TunaReceive > PC tools > Send logs to PC):
   *        {"logs": [{"name", "device", "made", "size"}]}.
   */
  void getReceiverLogs(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto &l : settings_backup::receiver_logs()) {
      list.push_back({{"name", l.name}, {"device", l.device}, {"made", l.made}, {"size", l.size}});
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["logs"] = list;
    send_response(response, output_tree);
  }

  /**
   * @brief One log from a TV as text: GET /api/receiver-log?name=...&download=1 to save it.
   */
  void getReceiverLog(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    auto args = request->parse_query_string();
    const auto name = args.find("name") != args.end() ? args.find("name")->second : std::string {};
    const bool download = args.find("download") != args.end() && args.find("download")->second == "1";
    const auto text = settings_backup::receiver_log(name);
    if (text.empty()) {
      bad_request(response, request, "That log isn't on this PC any more.");
      return;
    }
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "text/plain; charset=utf-8");
    headers.emplace("Cache-Control", "no-store");
    headers.emplace("X-Content-Type-Options", "nosniff");
    if (download) {
      headers.emplace("Content-Disposition", "attachment; filename=\"TunaReceive log "s + name + "\"");
    }
    response->write(SimpleWeb::StatusCode::success_ok, text, headers);
  }

  /**
   * @brief TunaReceive's settings on each TV that has shared them: {"tvs": [...]}.
   */
  void getReceiverSettings(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }
    nlohmann::json output_tree;
    output_tree["status"] = true;
    output_tree["tvs"] = receiver_settings::list();
    send_response(response, output_tree);
  }

  /**
   * @brief Change a TV's settings: {"uuid", "changes": {key: value or null}}.
   */
  void postReceiverSettings(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      auto input = nlohmann::json::parse(ss.str());
      auto error = receiver_settings::change(input.value("uuid", ""), input.value("changes", nlohmann::json::object()));
      if (!error.empty()) {
        bad_request(response, request, error);
        return;
      }
      nlohmann::json output_tree;
      output_tree["status"] = true;
      send_response(response, output_tree);
    } catch (std::exception &e) {
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Quit TunaStream.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * On Windows, if running in a service, a special shutdown code is returned.
   */
  void quit(resp_https_t response, req_https_t request) {
    if (!authenticate(response, request)) {
      return;
    }

    print_req(request);

    BOOST_LOG(warning) << "Requested quit from config page!"sv;

    proc::proc.terminate();

#ifdef _WIN32
    if (GetConsoleWindow() == NULL) {
      lifetime::exit_tunastream(ERROR_SHUTDOWN_IN_PROGRESS, true);
    } else
#endif
    {
      lifetime::exit_tunastream(0, true);
    }
    // If exit fails, write a response after 5 seconds.
    std::thread write_resp([response]{
      std::this_thread::sleep_for(5s);
      response->write();
    });
    write_resp.detach();
  }

  /**
   * @brief Launch an application.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void launchApp(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());

      // Check for required uuid field in body
      if (!input_tree.contains("uuid") || !input_tree["uuid"].is_string()) {
        bad_request(response, request, "Missing or invalid uuid in request body");
        return;
      }
      std::string uuid = input_tree["uuid"].get<std::string>();

      nlohmann::json output_tree;
      const auto &apps = proc::proc.get_apps();
      for (auto &app : apps) {
        if (app.uuid == uuid) {
          crypto::named_cert_t named_cert {
            .name = "",
            .uuid = http::unique_id,
            .perm = crypto::PERM::_all,
          };
          BOOST_LOG(info) << "Launching app ["sv << app.name << "] from web UI"sv;
          auto launch_session = nvhttp::make_launch_session(true, false, request->parse_query_string(), &named_cert);
          auto err = proc::proc.execute(app, launch_session);
          if (err) {
            bad_request(response, request, err == 503 ?
                        "Failed to initialize video capture/encoding. Is a display connected and turned on?" :
                        "Failed to start the specified application");
          } else {
            output_tree["status"] = true;
            send_response(response, output_tree);
          }
          return;
        }
      }
      BOOST_LOG(error) << "Couldn't find app with uuid ["sv << uuid << ']';
      bad_request(response, request, "Cannot find requested application");
    }
    catch (std::exception &e) {
      BOOST_LOG(warning) << "LaunchApp: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Disconnect a client.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   */
  void disconnect(resp_https_t response, req_https_t request) {
    if (!validateContentType(response, request, "application/json") || !authenticate(response, request)) {
      return;
    }

    print_req(request);

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json output_tree;
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string uuid = input_tree.value("uuid", "");
      output_tree["status"] = nvhttp::find_and_stop_session(uuid, true);
      send_response(response, output_tree);
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Disconnect: "sv << e.what();
      bad_request(response, request, e.what());
    }
  }

  /**
   * @brief Login the user.
   * @param response The HTTP response object.
   * @param request The HTTP request object.
   *
   * The body for the POST request should be JSON serialized in the following format:
   * @code{.json}
   * {
   *   "username": "<username>",
   *   "password": "<password>"
   * }
   * @endcode
   */
  void login(resp_https_t response, req_https_t request) {
    if (!checkIPOrigin(response, request) || !validateContentType(response, request, "application/json")) {
      return;
    }

    auto fg = util::fail_guard([&]{
      response->write(SimpleWeb::StatusCode::client_error_unauthorized);
    });

    try {
      std::stringstream ss;
      ss << request->content.rdbuf();
      nlohmann::json input_tree = nlohmann::json::parse(ss.str());
      std::string username = input_tree.value("username", "");
      std::string password = input_tree.value("password", "");
      std::string hash = util::hex(crypto::hash(password + config::tunastream.salt)).to_string();
      if (!boost::iequals(username, config::tunastream.username) || hash != config::tunastream.password)
        return;
      std::string sessionCookieRaw = crypto::rand_alphabet(64);
      web_sessions::add(util::hex(crypto::hash(sessionCookieRaw + config::tunastream.salt)).to_string());
      // Keep me signed in: 15 days. Otherwise until the browser closes.
      const bool remember = input_tree.value("remember", true);
      const SimpleWeb::CaseInsensitiveMultimap headers {
        { "Set-Cookie", "auth=" + sessionCookieRaw + "; Secure; SameSite=Strict; " + (remember ? "Max-Age=1296000; "s : ""s) + "HttpOnly; Path=/" }
      };
      response->write(headers);
      fg.disable();
    } catch (std::exception &e) {
      BOOST_LOG(warning) << "Web UI Login failed: ["sv << net::addr_to_normalized_string(request->remote_endpoint().address())
                               << "]: "sv << e.what();
      response->write(SimpleWeb::StatusCode::server_error_internal_server_error);
      fg.disable();
      return;
    }
  }

  /**
   * @brief Start the HTTPS server.
   */
  void start() {
    auto shutdown_event = mail::man->event<bool>(mail::shutdown);
    auto port_https = net::map_port(PORT_HTTPS);
    auto address_family = net::af_from_enum_string(config::tunastream.address_family);
    https_server_t server { config::nvhttp.cert, config::nvhttp.pkey };
    server.default_resource["DELETE"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PATCH"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["POST"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["PUT"] = [](resp_https_t response, req_https_t request) {
      bad_request(response, request);
    };
    server.default_resource["GET"] = not_found;
    server.resource["^/$"]["GET"] = getIndexPage;
    server.resource["^/pin/?$"]["GET"] = getPinPage;
    server.resource["^/apps/?$"]["GET"] = getAppsPage;
    server.resource["^/config/?$"]["GET"] = getConfigPage;
    server.resource["^/password/?$"]["GET"] = getPasswordPage;
    server.resource["^/welcome/?$"]["GET"] = getWelcomePage;
    server.resource["^/login/?$"]["GET"] = getLoginPage;
    server.resource["^/troubleshooting/?$"]["GET"] = getTroubleshootingPage;
    server.resource["^/setup/?$"]["GET"] = getSetupPage;
    server.resource["^/help/?$"]["GET"] = getHelpPage;
    server.resource["^/api/login"]["POST"] = login;
    server.resource["^/api/pin$"]["POST"] = savePin;
    server.resource["^/api/otp$"]["POST"] = getOTP;
    server.resource["^/api/apps$"]["GET"] = getApps;
    server.resource["^/api/apps$"]["POST"] = saveApp;
    server.resource["^/api/apps/reorder$"]["POST"] = reorderApps;
    server.resource["^/api/apps/delete$"]["POST"] = deleteApp;
    server.resource["^/api/apps/launch$"]["POST"] = launchApp;
    server.resource["^/api/apps/close$"]["POST"] = closeApp;
    server.resource["^/api/rumble-test$"]["POST"] = rumbleTest;
    server.resource["^/api/status$"]["GET"] = getStatus;
    server.resource["^/api/recap$"]["GET"] = getRecap;
    server.resource["^/api/tv-message$"]["POST"] = postTvMessage;
    server.resource["^/api/tv-message$"]["GET"] = getTvMessages;
    server.resource["^/api/sounds$"]["GET"] = getSounds;
    server.resource["^/api/play-limits$"]["GET"] = getPlayLimits;
    server.resource["^/api/play-limits$"]["POST"] = postPlayLimits;
    server.resource["^/api/play-limits/extend$"]["POST"] = postPlayExtend;
    server.resource["^/api/captures$"]["GET"] = getCaptures;
    server.resource["^/api/wifi$"]["GET"] = getWifi;
    server.resource["^/api/captures/file$"]["GET"] = getCaptureFile;
    server.resource["^/api/highlights$"]["POST"] = postHighlights;
    server.resource["^/api/sounds$"]["POST"] = addSound;
    server.resource["^/api/sounds/delete$"]["POST"] = removeSound;
    server.resource["^/api/update$"]["GET"] = getUpdate;
    server.resource["^/api/update$"]["POST"] = startUpdate;
    server.resource["^/api/vdisplay/repair$"]["POST"] = repairVirtualDisplay;
    server.resource["^/api/apps/continue-here$"]["POST"] = continueHere;
    server.resource["^/api/apps/discover$"]["GET"] = discoverApps;
    server.resource["^/api/other-hosts$"]["POST"] = setOtherHost;
    server.resource["^/api/apps/add-many$"]["POST"] = addApps;
    server.resource["^/api/logs$"]["GET"] = getLogs;
    server.resource["^/api/config$"]["GET"] = getConfig;
    server.resource["^/api/config$"]["POST"] = saveConfig;
    server.resource["^/api/configLocale$"]["GET"] = getLocale;
    server.resource["^/api/restart$"]["POST"] = restart;
    server.resource["^/api/backup$"]["GET"] = getBackup;
    server.resource["^/api/restore$"]["POST"] = postRestore;
    server.resource["^/api/receiver-backups$"]["GET"] = getReceiverBackups;
    server.resource["^/api/receiver-logs$"]["GET"] = getReceiverLogs;
    server.resource["^/api/receiver-log$"]["GET"] = getReceiverLog;
    server.resource["^/api/receiver-settings$"]["GET"] = getReceiverSettings;
    server.resource["^/api/receiver-settings$"]["POST"] = postReceiverSettings;
    server.resource["^/api/quit$"]["POST"] = quit;
    server.resource["^/api/reset-display-device-persistence$"]["POST"] = resetDisplayDevicePersistence;
    server.resource["^/api/password$"]["POST"] = savePassword;
    server.resource["^/api/clients/unpair-all$"]["POST"] = unpairAll;
    server.resource["^/api/clients/list$"]["GET"] = getClients;
    server.resource["^/api/files$"]["GET"] = getFiles;
    server.resource["^/api/files/send$"]["POST"] = sendFile;
    server.resource["^/api/files/cancel$"]["POST"] = cancelFile;
    server.resource["^/api/clients/update$"]["POST"] = updateClient;
    server.resource["^/api/clients/unpair$"]["POST"] = unpair;
    server.resource["^/api/clients/disconnect$"]["POST"] = disconnect;
    server.resource["^/api/covers/upload$"]["POST"] = uploadCover;
    server.resource["^/images/tunastream.ico$"]["GET"] = getFaviconImage;
    server.resource["^/images/logo-tunastream-45.png$"]["GET"] = getTunaStreamLogoImage;
    server.resource["^/images/tunastream-app-(180|512).png$"]["GET"] = getAppIcon;
    server.resource["^/live.webmanifest$"]["GET"] = getLiveManifest;
    server.resource["^/watch.webmanifest$"]["GET"] = getWatchManifest;
    server.resource["^/live/?$"]["GET"] = getLivePage;
    server.resource["^/checklist/?$"]["GET"] = getChecklistPage;
    server.resource["^/watch/?$"]["GET"] = getWatchPage;
    server.resource["^/api/tokens$"]["GET"] = getApiTokens;
    server.resource["^/api/tokens/self$"]["GET"] = getApiTokenSelf;
    server.resource["^/api/library-sync$"]["GET"] = getLibrarySync;
    server.resource["^/api/library-sync$"]["POST"] = postLibrarySync;
    server.resource["^/api/tokens$"]["POST"] = postApiToken;
    server.resource["^/api/tokens/delete$"]["POST"] = postApiTokenDelete;
    server.resource["^/api/watch/stream$"]["GET"] = getWatchStream;
    server.resource["^/api/watch/share$"]["GET"] = getWatchShare;
    server.resource["^/api/watch/share$"]["POST"] = postWatchShare;
    server.resource["^/api/watch/viewers$"]["GET"] = getWatchViewers;
    server.resource["^/api/watch/viewers/remove$"]["POST"] = postWatchViewerRemove;
    server.resource["^/api/watch/quality$"]["POST"] = postWatchQuality;
    server.resource["^/api/watch/invite$"]["POST"] = postWatchInvite;
    server.resource["^/api/watch/redeem$"]["POST"] = postWatchRedeem;
    server.resource["^/api/watch/rotate$"]["POST"] = postWatchRotate;
    server.resource["^/api/watch/passwords$"]["POST"] = postWatchPasswordMake;
    server.resource["^/api/watch/password$"]["POST"] = postWatchPasswordUse;
    server.resource["^/api/checklist$"]["GET"] = getChecklist;
    server.resource["^/api/checklist$"]["POST"] = postChecklist;
    server.resource["^/api/cover$"]["GET"] = getCover;
    server.resource["^/assets\\/.+$"]["GET"] = getNodeModules;
    server.config.reuse_address = true;
    server.config.address = net::af_to_any_address_string(address_family);
    server.config.port = port_https;

    auto accept_and_run = [&](auto *server) {
      try {
        server->start([port_https](unsigned short port) {
          BOOST_LOG(info) << "Configuration UI available at [https://localhost:"sv << port << "]";
        });
      } catch (boost::system::system_error &err) {
        // It's possible the exception gets thrown after calling server->stop() from a different thread
        if (shutdown_event->peek())
          return;
        BOOST_LOG(fatal) << "Couldn't start Configuration HTTPS server on port ["sv << port_https << "]: "sv << err.what();
        shutdown_event->raise(true);
        return;
      }
    };
    std::thread tcp { accept_and_run, &server };

    // Wait for any event
    shutdown_event->view();

    server.stop();

    tcp.join();
  }
}  // namespace confighttp
