/**
 * @file src/tv_messages.cpp
 * @brief Short messages typed on the home page and shown on a streaming device's screen.
 *
 * TunaReceive reports how its stream is going every five seconds (/tuna/report); the answer
 * carries any messages waiting for it, so a message appears within about five seconds.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <set>

// local includes
#include "tv_messages.h"

using namespace std::literals;

namespace tv_messages {
  namespace {
    struct queued_t {
      message_t message;
      std::string device;  // empty: every device
      std::chrono::steady_clock::time_point at;
      std::set<std::string> delivered_to;
    };

    std::mutex lock;
    std::deque<queued_t> queue;
    std::uint32_t next_id = 1;
    constexpr auto KEEP_FOR = 2min;

    void drop_old() {
      const auto now = std::chrono::steady_clock::now();
      while (!queue.empty() && now - queue.front().at > KEEP_FOR) {
        queue.pop_front();
      }
    }

    std::string clean(const std::string &text) {
      std::string out;
      for (unsigned char c : text) {
        if (c == '\n' || c >= 0x20) {
          out.push_back((char) c);
        } else if (c == '\t' || c == '\r') {
          out.push_back(c == '\t' ? ' ' : '\n');
        }
      }
      // Trim spaces and line breaks at both ends
      const auto first = out.find_first_not_of(" \n");
      if (first == std::string::npos) {
        return {};
      }
      out = out.substr(first, out.find_last_not_of(" \n") - first + 1);
      if (out.size() > MAX_LENGTH) {
        // Cut on a character boundary, not in the middle of a UTF-8 sequence
        size_t cut = MAX_LENGTH;
        while (cut > 0 && ((unsigned char) out[cut] & 0xC0) == 0x80) {
          --cut;
        }
        out.resize(cut);
      }
      return out;
    }
  }  // namespace

  std::uint32_t post(const std::string &device, const std::string &text) {
    auto cleaned = clean(text);
    if (cleaned.empty()) {
      return 0;
    }
    std::lock_guard lg {lock};
    drop_old();
    const auto id = next_id++;
    queue.push_back({{id, std::move(cleaned)}, device, std::chrono::steady_clock::now(), {}});
    return id;
  }

  std::vector<message_t> take(const std::string &device) {
    std::lock_guard lg {lock};
    drop_old();
    std::vector<message_t> out;
    for (auto &q : queue) {
      if ((q.device.empty() || q.device == device) && !q.delivered_to.contains(device)) {
        q.delivered_to.insert(device);
        out.push_back(q.message);
      }
    }
    return out;
  }

  std::vector<std::pair<std::uint32_t, std::vector<std::string>>> delivered() {
    std::lock_guard lg {lock};
    drop_old();
    std::vector<std::pair<std::uint32_t, std::vector<std::string>>> out;
    for (const auto &q : queue) {
      out.emplace_back(q.message.id, std::vector<std::string>(q.delivered_to.begin(), q.delivered_to.end()));
    }
    return out;
  }
}  // namespace tv_messages
