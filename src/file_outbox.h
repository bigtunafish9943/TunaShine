/**
 * @file src/file_outbox.h
 * @brief Files waiting to be sent from this PC to a paired device ("Send to device").
 *
 * The web interface puts a file in a paired device's outbox; the device (TunaReceive) lists
 * its outbox over its paired connection, downloads each file and then removes it. Each device
 * only ever sees its own outbox.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <istream>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace file_outbox {
  // Files are held in memory while they are sent, so keep them to a sensible size
  constexpr std::uintmax_t max_file_size = 256ull * 1024 * 1024;

  struct entry_t {
    std::string id;  ///< Unique within the device's outbox, also the file's name on disk
    std::string name;  ///< The name the device saves it under
    std::uintmax_t size;
  };

  /// A device id as stored for paired devices: letters, digits and dashes
  bool valid_uuid(const std::string &uuid);

  /**
   * @brief Adds a file to a device's outbox.
   * @return The new entry's id, or an empty string with @p error set.
   */
  std::string add(const std::string &uuid, const std::string &name, std::istream &data, std::string &error);

  /// The device's waiting files, oldest first.
  std::vector<entry_t> list(const std::string &uuid);

  /// Where a waiting file is on disk, or an empty path if there is no such file.
  std::filesystem::path path_of(const std::string &uuid, const std::string &id);

  /// Removes a waiting file. Returns false if there was no such file.
  bool remove(const std::string &uuid, const std::string &id);

  /// Every device's waiting files, as {uuid: [{id, name, size}]}.
  nlohmann::json all();

  /// Removes a device's whole outbox, for when it is unpaired.
  void clear(const std::string &uuid);
}  // namespace file_outbox
