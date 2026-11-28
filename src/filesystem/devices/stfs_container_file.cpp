/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/stfs_container_entry.h>
#include <rex/filesystem/devices/stfs_container_file.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#include <rex/math.h>

namespace rex::filesystem {

namespace {
// Open files of one package share its FILE*; each seek and read pair must not interleave.
std::mutex read_mutex;
}  // namespace

StfsContainerFile::StfsContainerFile(uint32_t file_access, StfsContainerEntry* entry)
    : File(file_access, entry), entry_(entry) {}

StfsContainerFile::~StfsContainerFile() = default;

void StfsContainerFile::Destroy() {
  delete this;
}

X_STATUS StfsContainerFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                     size_t* out_bytes_read) {
  if (byte_offset >= entry_->size()) {
    return X_STATUS_END_OF_FILE;
  }

  size_t src_offset = 0;
  uint8_t* p = buffer.data();
  size_t remaining_length = std::min(buffer.size(), entry_->size() - byte_offset);

  *out_bytes_read = 0;
  for (size_t i = 0; i < entry_->block_list().size(); i++) {
    auto& record = entry_->block_list()[i];
    if (src_offset + record.length <= byte_offset) {
      // Doesn't begin in this region. Skip it.
      src_offset += record.length;
      continue;
    }

    size_t read_offset = (byte_offset > src_offset) ? byte_offset - src_offset : 0;
    size_t read_length = std::min(record.length - read_offset, remaining_length);

    auto& file = entry_->files()->at(record.file);
    size_t num_read = 0;
    {
      std::lock_guard lock(read_mutex);
      if (rex::filesystem::Seek(file, record.offset + read_offset, SEEK_SET)) {
        num_read = fread(p, 1, read_length, file);
      }
    }

    *out_bytes_read += num_read;
    if (num_read != read_length) {
      return X_STATUS_UNSUCCESSFUL;
    }
    p += num_read;
    src_offset += record.length;
    remaining_length -= read_length;
    if (remaining_length == 0) {
      break;
    }
  }

  return X_STATUS_SUCCESS;
}

}  // namespace rex::filesystem
