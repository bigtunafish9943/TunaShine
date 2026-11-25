/**
 * @file src/mcp_bridge.cpp
 * @brief "tunastream.exe --mcp": TunaStream as tools for an AI assistant.
 *
 * An MCP (Model Context Protocol) server on standard input and output, for Claude Desktop,
 * Claude Code and other assistants that start MCP servers as programs. Each tool calls
 * TunaStream's own web API with an access token (TUNASTREAM_TOKEN), so an assistant can do
 * exactly what that token allows and nothing more: the PC checks every call, as it does for
 * any script. Tools the token can't use aren't offered.
 *
 * Setup (Claude Desktop, claude_desktop_config.json):
 *   {"mcpServers": {"tunastream": {"command": "C:\\Program Files\\TunaStream\\tunastream.exe",
 *     "args": ["--mcp"], "env": {"TUNASTREAM_TOKEN": "tst_..."}}}}
 * Change Password > Access tokens shows this, filled in, when a token is made.
 *
 * Options: --url https://host:port (default: this PC's TunaStream, from its settings).
 * Nothing is written to standard output except MCP messages; problems go to standard error.
 */
// standard includes
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local includes
#include "mcp_bridge.h"
#include "platform/common.h"

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
#endif

namespace mcp_bridge {
  namespace {
    using json = nlohmann::json;

    std::string base_url;
    std::string token;
    bool local = true;

    // TunaStream's web page port: the base port in tunastream.conf (47989 unless changed) + 1
    std::string default_url() {
      int port = 47989;
      std::ifstream in {platf::appdata() / "tunastream.conf"};
      std::string line;
      while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) {
          continue;
        }
        auto key = line.substr(0, eq);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                    return std::isspace(c);
                  }),
                  key.end());
        if (key == "port") {
          try {
            port = std::stoi(line.substr(eq + 1));
          } catch (const std::exception &) {
          }
        }
      }
      return "https://localhost:" + std::to_string(port + 1);
    }

    struct reply_t {
      long code = 0;
      std::string body;
      std::string error;
    };

    std::size_t collect(char *data, std::size_t size, std::size_t count, void *user) {
      auto out = static_cast<std::string *>(user);
      if (out->size() < 8 * 1024 * 1024) {
        out->append(data, size * count);
      }
      return size * count;
    }

    reply_t call(const std::string &method, const std::string &path, const json *body = nullptr) {
      reply_t r;
      CURL *curl = curl_easy_init();
      if (!curl) {
        r.error = "couldn't start a connection";
        return r;
      }
      const std::string url = base_url + path;
      const std::string payload = body ? body->dump() : std::string {};
      curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
      headers = curl_slist_append(headers, "Accept: application/json");
      if (method == "POST") {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) payload.size());
      }
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      // TunaStream's certificate is its own (self-signed), as for the browser: on this PC there's
      // nothing else on its port to talk to
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
      const auto res = curl_easy_perform(curl);
      if (res != CURLE_OK) {
        r.error = curl_easy_strerror(res);
      } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.code);
      }
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return r;
    }

    std::string url_encode(const std::string &s) {
      std::string out;
      char buf[4];
      for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
          out.push_back((char) c);
        } else {
          std::snprintf(buf, sizeof(buf), "%%%02X", c);
          out += buf;
        }
      }
      return out;
    }

    /// A tool's answer: text for the assistant, and whether it failed
    struct result_t {
      std::string text;
      bool error = false;
    };

    result_t from_reply(const reply_t &r, const std::function<json(const json &)> &shape = nullptr) {
      if (!r.error.empty()) {
        return {"TunaStream couldn't be reached at " + base_url + " (" + r.error + "). Is it running?", true};
      }
      if (r.code == 401 || r.code == 403) {
        return {"The access token isn't allowed to do this, or has expired or been removed. Make one under Change Password > Access tokens in TunaStream.", true};
      }
      json j;
      try {
        j = json::parse(r.body);
      } catch (const std::exception &) {
        if (r.code >= 400) {
          return {"TunaStream answered " + std::to_string(r.code) + ".", true};
        }
        return {r.body, false};
      }
      if (j.is_object() && j.contains("status") && j["status"].is_boolean() && !j["status"].get<bool>()) {
        return {j.value("error", "TunaStream said no."), true};
      }
      if (j.is_object()) {
        j.erase("status");
        j.erase("status_code");
      }
      return {(shape ? shape(j) : j).dump(2), r.code >= 400};
    }

    std::string str_arg(const json &args, const char *name) {
      if (args.contains(name) && args[name].is_string()) {
        return args[name].get<std::string>();
      }
      return {};
    }

    struct tool_t {
      std::string name;
      std::string scope;
      std::string description;
      json schema;
      std::function<result_t(const json &)> run;
    };

    json object_schema(json properties = json::object(), json required = json::array()) {
      json s {{"type", "object"}, {"properties", properties}};
      if (!required.empty()) {
        s["required"] = required;
      }
      return s;
    }

    std::vector<tool_t> make_tools() {
      std::vector<tool_t> tools;

      tools.push_back({"get_status", "status",
                       "What's happening on the gaming PC now: the game being streamed, each TV or device streaming with its frame rate, "
                       "bitrate and connection quality, controllers and their batteries, whether the virtual display works, and whether "
                       "the screen is being shared with browsers.",
                       object_schema(), [](const json &) {
                         return from_reply(call("GET", "/api/status"));
                       }});

      tools.push_back({"list_devices", "status", "The devices (TVs, phones, tablets) paired with TunaStream, and which are connected now.",
                       object_schema(), [](const json &) {
                         return from_reply(call("GET", "/api/clients/list"), [](const json &j) {
                           json out = json::array();
                           for (const auto &c : j.value("named_certs", json::array())) {
                             out.push_back({{"name", c.value("name", "")}, {"connected", c.value("connected", false)}});
                           }
                           return out;
                         });
                       }});

      tools.push_back({"list_games", "status", "The games and apps that can be streamed, with the id launch_game needs.", object_schema(), [](const json &) {
                         return from_reply(call("GET", "/api/apps"), [](const json &j) {
                           json out = json::array();
                           for (const auto &a : j.value("apps", json::array())) {
                             out.push_back({{"name", a.value("name", "")}, {"id", a.value("uuid", "")}});
                           }
                           return out;
                         });
                       }});

      tools.push_back({"launch_game", "launch",
                       "Start a game or app on the PC so a TV can stream it. Give its id from list_games, or its name. "
                       "Starting a game ends the one running now.",
                       object_schema({{"id", {{"type", "string"}, {"description", "The id from list_games"}}},
                                      {"name", {{"type", "string"}, {"description", "The game's name, when the id isn't known"}}}}),
                       [](const json &args) {
                         auto id = str_arg(args, "id");
                         const auto name = str_arg(args, "name");
                         if (id.empty() && !name.empty()) {
                           auto r = call("GET", "/api/apps");
                           try {
                             auto lower = [](std::string s) {
                               std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                                 return (char) std::tolower(c);
                               });
                               return s;
                             };
                             for (const auto &a : json::parse(r.body).value("apps", json::array())) {
                               if (lower(a.value("name", "")) == lower(name)) {
                                 id = a.value("uuid", "");
                                 break;
                               }
                             }
                           } catch (const std::exception &) {
                           }
                           if (id.empty()) {
                             return result_t {"There's no game called \"" + name + "\" (or this token can't list games). Use list_games for the ids.", true};
                           }
                         }
                         if (id.empty()) {
                           return result_t {"Give the game's id or name.", true};
                         }
                         json body {{"uuid", id}};
                         return from_reply(call("POST", "/api/apps/launch", &body));
                       }});

      tools.push_back({"end_session", "sessions", "Quit the game being streamed and end the stream for every device.", object_schema(), [](const json &) {
                         json body = json::object();
                         return from_reply(call("POST", "/api/apps/close", &body));
                       }});

      tools.push_back({"give_more_play_time", "sessions", "Give a device more play time today, when a daily play limit is set for it.",
                       object_schema({{"device", {{"type", "string"}, {"description", "The device's name; empty for all"}}},
                                      {"minutes", {{"type", "integer"}, {"description", "Extra minutes, 1 to 240"}, {"minimum", 1}, {"maximum", 240}}}},
                                     json::array({"minutes"})),
                       [](const json &args) {
                         json body {{"device", str_arg(args, "device")}, {"minutes", std::clamp(args.value("minutes", 15), 1, 240)}};
                         return from_reply(call("POST", "/api/play-limits/extend", &body));
                       }});

      tools.push_back({"send_tv_message", "messages",
                       "Show a short message over the game on the TV (\"Dinner's ready!\"), optionally with a sound from list_sounds. "
                       "Only reaches devices that are streaming.",
                       object_schema({{"text", {{"type", "string"}, {"description", "The message, a sentence or two"}}},
                                      {"device", {{"type", "string"}, {"description", "One device's name; empty for every device streaming"}}},
                                      {"sound", {{"type", "string"}, {"description", "A sound's name from list_sounds"}}}}),
                       [](const json &args) {
                         json body {{"text", str_arg(args, "text")}, {"device", str_arg(args, "device")}, {"sound", str_arg(args, "sound")}};
                         return from_reply(call("POST", "/api/tv-message", &body));
                       }});

      tools.push_back({"list_sounds", "status", "The sounds send_tv_message can play on the TV.", object_schema(), [](const json &) {
                         return from_reply(call("GET", "/api/sounds"), [](const json &j) {
                           json out = json::array();
                           for (const auto &s : j.value("sounds", json::array())) {
                             out.push_back(s.value("name", ""));
                           }
                           return out;
                         });
                       }});

      tools.push_back({"get_play_recap", "status", "Play time for each of the last seven days, the most-played games and how smooth the streams were.",
                       object_schema(), [](const json &) {
                         return from_reply(call("GET", "/api/recap"));
                       }});

      tools.push_back({"read_pc_log", "logs", "The end of TunaStream's own log on the PC, for working out why something went wrong.",
                       object_schema({{"lines", {{"type", "integer"}, {"description", "How many lines from the end, up to 2000"}, {"minimum", 10}, {"maximum", 2000}}}}),
                       [](const json &args) {
                         auto r = call("GET", "/api/logs");
                         if (!r.error.empty() || r.code != 200) {
                           return from_reply(r);
                         }
                         const auto want = (std::size_t) std::clamp(args.value("lines", 300), 10, 2000);
                         std::size_t pos = r.body.size();
                         std::size_t n = 0;
                         while (pos > 0 && n <= want) {
                           pos = r.body.rfind('\n', pos - 1);
                           if (pos == std::string::npos) {
                             pos = 0;
                             break;
                           }
                           ++n;
                         }
                         return result_t {r.body.substr(pos), false};
                       }});

      tools.push_back({"list_tv_logs", "logs", "Logs TVs have sent to the PC (TunaReceive > PC tools > Send logs to PC), newest first.", object_schema(), [](const json &) {
                         return from_reply(call("GET", "/api/receiver-logs"));
                       }});

      tools.push_back({"read_tv_log", "logs", "One log a TV sent, by its name from list_tv_logs.",
                       object_schema({{"name", {{"type", "string"}, {"description", "The log's name from list_tv_logs"}}}}, json::array({"name"})),
                       [](const json &args) {
                         auto r = call("GET", "/api/receiver-log?name=" + url_encode(str_arg(args, "name")));
                         if (r.error.empty() && r.code == 200) {
                           // Plain text; the most recent part matters most
                           if (r.body.size() > 200 * 1024) {
                             r.body = "[earlier part left out]\n" + r.body.substr(r.body.size() - 200 * 1024);
                           }
                           return result_t {r.body, false};
                         }
                         return from_reply(r);
                       }});

      return tools;
    }

    std::vector<tool_t> tools;
    std::set<std::string> allowed;  // the token's scopes
    bool scopes_known = false;

    void load_scopes() {
      if (scopes_known) {
        return;
      }
      auto r = call("GET", "/api/tokens/self");
      if (r.error.empty() && r.code == 200) {
        try {
          auto j = json::parse(r.body);
          for (const auto &s : j.value("scopes", json::array())) {
            allowed.insert(s.get<std::string>());
          }
          scopes_known = true;
        } catch (const std::exception &) {
        }
      } else if (!r.error.empty()) {
        std::cerr << "TunaStream couldn't be reached at " << base_url << ": " << r.error << std::endl;
      } else {
        std::cerr << "TunaStream didn't accept the access token (" << r.code << ")" << std::endl;
      }
    }

    bool usable(const tool_t &t) {
      // Not known (TunaStream not running yet): offer everything, and each call says why it can't
      return !scopes_known || allowed.count(t.scope);
    }

    json error_reply(const json &id, int code, const std::string &message) {
      return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
    }

    /// One JSON-RPC request; null for a notification (no reply)
    json handle(const json &req) {
      if (!req.is_object() || !req.contains("method")) {
        return error_reply(nullptr, -32600, "Invalid request");
      }
      const bool notification = !req.contains("id");
      const json id = notification ? json(nullptr) : req["id"];
      const auto method = req.value("method", "");
      const json params = req.contains("params") && req["params"].is_object() ? req["params"] : json::object();

      if (method == "initialize") {
        static const std::vector<std::string> versions {"2025-06-18", "2025-03-26", "2024-11-05"};
        auto asked = params.value("protocolVersion", "");
        auto version = std::find(versions.begin(), versions.end(), asked) != versions.end() ? asked : versions.front();
        load_scopes();
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {
          {"protocolVersion", version},
          {"capabilities", {{"tools", {{"listChanged", false}}}}},
          {"serverInfo", {{"name", "tunastream"}, {"title", "TunaStream"}, {"version", PROJECT_VERSION}}},
          {"instructions", "TunaStream streams games from this gaming PC to TVs and tablets on the home network. "
                           "Use get_status to see what's playing before acting. Starting a game or ending a session affects "
                           "whoever is playing, so check with the person first unless they asked for it."},
        }}};
      }
      if (notification) {
        return nullptr;  // notifications/initialized and the like
      }
      if (method == "ping") {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
      }
      if (method == "tools/list") {
        load_scopes();
        json list = json::array();
        for (const auto &t : tools) {
          if (usable(t)) {
            list.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
          }
        }
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", list}}}};
      }
      if (method == "tools/call") {
        const auto name = params.value("name", "");
        const json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
        for (const auto &t : tools) {
          if (t.name != name) {
            continue;
          }
          load_scopes();
          result_t res;
          if (!usable(t)) {
            res = {"This access token isn't allowed to " + t.scope + ". Make one that is under Change Password > Access tokens in TunaStream.", true};
          } else {
            try {
              res = t.run(args);
            } catch (const std::exception &e) {
              res = {std::string {"Something went wrong: "} + e.what(), true};
            }
          }
          return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", json::array({{{"type", "text"}, {"text", res.text}}})}, {"isError", res.error}}}}};
        }
        return error_reply(id, -32602, "Unknown tool: " + name);
      }
      return error_reply(id, -32601, "Method not found: " + method);
    }
  }  // namespace

  int run(int argc, char *argv[]) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::ios::sync_with_stdio(false);

    for (int i = 2; i + 1 < argc; ++i) {
      if (std::string {argv[i]} == "--url") {
        base_url = argv[i + 1];
      }
    }
    if (base_url.empty()) {
      if (auto env = std::getenv("TUNASTREAM_URL"); env && *env) {
        base_url = env;
      } else {
        base_url = default_url();
      }
    }
    while (!base_url.empty() && base_url.back() == '/') {
      base_url.pop_back();
    }
    if (auto env = std::getenv("TUNASTREAM_TOKEN"); env && *env) {
      token = env;
    }
    if (token.empty()) {
      std::cerr << "Set TUNASTREAM_TOKEN to an access token from TunaStream (Change Password > Access tokens)." << std::endl;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    tools = make_tools();

    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.find_first_not_of(" \t") == std::string::npos) {
        continue;
      }
      json out;
      try {
        auto req = json::parse(line);
        if (req.is_array()) {
          out = json::array();
          for (const auto &r : req) {
            auto one = handle(r);
            if (!one.is_null()) {
              out.push_back(one);
            }
          }
          if (out.empty()) {
            continue;
          }
        } else {
          out = handle(req);
          if (out.is_null()) {
            continue;
          }
        }
      } catch (const std::exception &) {
        out = error_reply(nullptr, -32700, "Parse error");
      }
      std::cout << out.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
      std::cout.flush();
    }
    curl_global_cleanup();
    return 0;
  }
}  // namespace mcp_bridge
