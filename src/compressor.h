#pragma once
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

class Compressor {
public:
    static bool compress_gzip(const fs::path& input, const fs::path& output, int level);
    static bool compress_brotli(const fs::path& input, const fs::path& output, int level);
    static bool copy_metadata(const fs::path& source, const fs::path& dest);
};