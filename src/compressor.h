#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

class Compressor {
public:
    static bool compress_gzip(const fs::path& input, const fs::path& output, int level);
    static bool compress_brotli(const fs::path& input, const fs::path& output, int level);
    static bool copy_metadata(const fs::path& source, const fs::path& dest);
    
    // Безопасное удаление сжатых копий с проверками
    static bool safe_remove_compressed(const fs::path& original_path);
    static bool validate_path_in_directory(const fs::path& path, const std::vector<std::string>& allowed_dirs);
    static bool check_file_ownership(const fs::path& path, uid_t expected_uid);
    static bool is_symlink_attack(const fs::path& path);
};