#pragma once

#include <filesystem>

namespace ckpt {

void merge_b1_ply(const std::filesystem::path& left,
                  const std::filesystem::path& right,
                  const std::filesystem::path& output);

}  // namespace ckpt
