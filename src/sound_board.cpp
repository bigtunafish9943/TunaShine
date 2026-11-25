/**
 * @file src/sound_board.cpp
 * @brief Short sounds that can be played on a streaming device's TV from the home page.
 *
 * Built-in sounds are installed in assets/sounds. Your own are kept in the sounds folder in
 * TunaStream's settings folder. TunaReceive fetches a sound (/tuna/sound) when it is sent.
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

// local includes
#include "config.h"
#include "logging.h"
#include "platform/common.h"
#include "sound_board.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace sound_board {
  namespace {
    const std::set<std::string> extensions {".ogg", ".mp3", ".wav"};

    fs::path own_dir() {
      return platf::appdata() / "sounds";
    }

    fs::path built_in_dir() {
      return fs::path {TUNASTREAM_ASSETS_DIR} / "sounds";
    }

    std::string lower(std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char) std::tolower(c);
      });
      return s;
    }

    /// Letters, digits, spaces, hyphens, apostrophes and underscores; 1 to 40 characters
    bool valid_name(const std::string &name) {
      if (name.empty() || name.size() > 40 || name.front() == ' ' || name.back() == ' ') {
        return false;
      }
      return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '!';
      });
    }

    std::vector<std::pair<std::string, fs::path>> sounds_in(const fs::path &dir) {
      std::vector<std::pair<std::string, fs::path>> out;
      std::error_code ec;
      for (const auto &entry : fs::directory_iterator {dir, ec}) {
        if (!entry.is_regular_file(ec) || !extensions.contains(lower(entry.path().extension().string()))) {
          continue;
        }
        try {
          auto name = entry.path().stem().string();
          if (valid_name(name)) {
            out.emplace_back(name, entry.path());
          }
        } catch (const std::exception &) {
          // A file name that isn't plain text; not one of ours
        }
      }
      std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return lower(a.first) < lower(b.first);
      });
      return out;
    }

    bool looks_like_audio(const std::string &bytes, const std::string &extension) {
      if (bytes.size() < 64) {
        return false;
      }
      if (extension == ".ogg") {
        return bytes.compare(0, 4, "OggS") == 0;
      }
      if (extension == ".wav") {
        return bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WAVE") == 0;
      }
      // MP3: an ID3 tag, or a frame sync
      return bytes.compare(0, 3, "ID3") == 0 ||
             ((unsigned char) bytes[0] == 0xFF && ((unsigned char) bytes[1] & 0xE0) == 0xE0);
    }
  }  // namespace

  std::vector<sound_t> list() {
    std::vector<sound_t> out;
    std::set<std::string> seen;
    for (const auto &[name, path] : sounds_in(own_dir())) {
      if (seen.insert(lower(name)).second) {
        out.push_back({name, false});
      }
    }
    for (const auto &[name, path] : sounds_in(built_in_dir())) {
      if (seen.insert(lower(name)).second) {
        out.push_back({name, true});
      }
    }
    return out;
  }

  std::optional<std::string> file_for(const std::string &name) {
    if (!valid_name(name)) {
      return std::nullopt;
    }
    for (const auto &dir : {own_dir(), built_in_dir()}) {
      for (const auto &[n, path] : sounds_in(dir)) {
        if (lower(n) == lower(name)) {
          return path.string();
        }
      }
    }
    return std::nullopt;
  }

  std::string content_type_of(const std::string &path) {
    auto ext = lower(fs::path {path}.extension().string());
    if (ext == ".ogg") {
      return "audio/ogg";
    }
    if (ext == ".mp3") {
      return "audio/mpeg";
    }
    return "audio/wav";
  }

  std::string add(const std::string &name, const std::string &extension, const std::string &bytes) {
    const auto ext = lower(extension.starts_with('.') ? extension : "." + extension);
    if (!valid_name(name)) {
      return "Give the sound a name of up to 40 letters, numbers and spaces.";
    }
    if (!extensions.contains(ext)) {
      return "Sounds can be OGG, MP3 or WAV files.";
    }
    if (bytes.size() > MAX_BYTES) {
      return "That file is too large. Sounds can be up to 2 MB; a few seconds is plenty.";
    }
    if (!looks_like_audio(bytes, ext)) {
      return "That file doesn't look like an " + ext.substr(1) + " sound.";
    }
    for (const auto &s : list()) {
      if (s.built_in && lower(s.name) == lower(name)) {
        return "A built-in sound already has that name. Pick another.";
      }
    }

    std::error_code ec;
    fs::create_directories(own_dir(), ec);
    // Replace any of your own sounds with the same name, whatever its type
    for (const auto &[n, path] : sounds_in(own_dir())) {
      if (lower(n) == lower(name)) {
        fs::remove(path, ec);
      }
    }
    std::ofstream out(own_dir() / (name + ext), std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), (std::streamsize) bytes.size());
    if (!out) {
      return "The sound couldn't be saved.";
    }
    BOOST_LOG(info) << "Sound added to the sound board: "sv << name;
    return {};
  }

  bool remove(const std::string &name) {
    std::error_code ec;
    bool removed = false;
    for (const auto &[n, path] : sounds_in(own_dir())) {
      if (lower(n) == lower(name)) {
        removed = fs::remove(path, ec) || removed;
      }
    }
    return removed;
  }
}  // namespace sound_board
