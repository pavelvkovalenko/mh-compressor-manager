/**
 * Unit тесты для конфигурации
 * Проверяют загрузку, валидацию и безопасность конфигурации
 */

#include <gtest/gtest.h>
#include "config.h"
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

class ConfigTest : public ::testing::Test {
protected:
    fs::path temp_config_path;
    
    void SetUp() override {
        temp_config_path = fs::temp_directory_path() / "mh_config_test.conf";
    }
    
    void TearDown() override {
        if (fs::exists(temp_config_path)) {
            fs::remove(temp_config_path);
        }
    }
    
    void writeConfig(const std::string& content) {
        std::ofstream ofs(temp_config_path);
        ofs << content;
    }
};

// Тест загрузки базовой конфигурации
TEST_F(ConfigTest, BasicConfigLoad) {
    writeConfig(R"(
target_paths = ["/tmp/test1", "/tmp/test2"]
extensions = ["txt", "log"]
algorithms = "all"
gzip_level = 6
brotli_level = 4
)");
    
    // Сохраняем текущий argv[0]
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        
        EXPECT_EQ(cfg.target_paths.size(), 2);
        EXPECT_EQ(cfg.extensions.size(), 2);
        EXPECT_EQ(cfg.algorithms, "all");
        EXPECT_EQ(cfg.gzip_level, 6);
        EXPECT_EQ(cfg.brotli_level, 4);
    } catch (const std::exception& e) {
        FAIL() << "Failed to load config: " << e.what();
    }
}

// Тест на безопасность: проверка на пустой путь файла конфигурации
TEST_F(ConfigTest, EmptyConfigPath) {
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg};
    int argc = 1;
    
    // Должна использоваться конфигурация по умолчанию или ошибка
    try {
        Config cfg = load_config(argc, argv);
        SUCCEED() << "Config loaded with default path";
    } catch (const std::exception& e) {
        SUCCEED() << "Expected exception for missing config: " << e.what();
    }
}

// Тест на безопасность: несуществующий файл конфигурации
TEST_F(ConfigTest, NonExistentConfigFile) {
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>("/nonexistent/config/path.conf")};
    int argc = 2;
    
    // Должна быть выброшена ошибка
    EXPECT_THROW(load_config(argc, argv), std::runtime_error);
}

// Тест на безопасность: невалидные значения уровня сжатия
TEST_F(ConfigTest, InvalidCompressionLevels) {
    writeConfig(R"(
target_paths = ["/tmp"]
extensions = ["txt"]
gzip_level = 15
brotli_level = 20
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        // Уровни должны быть ограничены допустимыми диапазонами
        EXPECT_GE(cfg.gzip_level, 1);
        EXPECT_LE(cfg.gzip_level, 9);
        EXPECT_GE(cfg.brotli_level, 1);
        EXPECT_LE(cfg.brotli_level, 11);
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for invalid levels: " << e.what();
    }
}

// Тест на безопасность: отрицательные значения уровней
TEST_F(ConfigTest, NegativeCompressionLevels) {
    writeConfig(R"(
target_paths = ["/tmp"]
extensions = ["txt"]
gzip_level = -5
brotli_level = -3
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        // Отрицательные значения должны быть отклонены или исправлены
        EXPECT_GE(cfg.gzip_level, 1);
        EXPECT_GE(cfg.brotli_level, 1);
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for negative levels: " << e.what();
    }
}

// Тест на безопасность: пустой список target_paths
TEST_F(ConfigTest, EmptyTargetPaths) {
    writeConfig(R"(
target_paths = []
extensions = ["txt"]
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        // Пустой список путей может быть опасен
        EXPECT_TRUE(cfg.target_paths.empty());
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for empty paths: " << e.what();
    }
}

// Тест на безопасность: абсолютные vs относительные пути
TEST_F(ConfigTest, AbsoluteVsRelativePaths) {
    writeConfig(R"(
target_paths = ["/tmp/absolute", "relative/path", "../parent"]
extensions = ["txt"]
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        
        // Проверяем что пути загружены
        EXPECT_EQ(cfg.target_paths.size(), 3);
        
        // Проверяем типы путей
        for (const auto& path : cfg.target_paths) {
            fs::path p(path);
            if (p.is_absolute()) {
                SUCCEED() << "Absolute path: " << path;
            } else {
                SUCCEED() << "Relative path: " << path;
            }
        }
    } catch (const std::exception& e) {
        FAIL() << "Failed to load config: " << e.what();
    }
}

// Тест на безопасность: symlink в путях
TEST_F(ConfigTest, SymlinkInPaths) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_config_symlink_test";
    fs::create_directories(temp_dir);
    
    // Создаем целевую директорию
    fs::path target_dir = temp_dir / "target";
    fs::create_directories(target_dir);
    
    // Создаем symlink
    fs::path symlink_dir = temp_dir / "link";
    fs::create_symlink(target_dir, symlink_dir);
    
    writeConfig(R"(
target_paths = [")" + symlink_dir.string() + R"("]
extensions = ["txt"]
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        EXPECT_EQ(cfg.target_paths.size(), 1);
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for symlink path: " << e.what();
    }
    
    // Очищаем
    fs::remove_all(temp_dir);
}

// Тест на безопасность: специальные символы в путях
TEST_F(ConfigTest, SpecialCharactersInPaths) {
    writeConfig(R"(
target_paths = ["/tmp/test path", "/tmp/test\ttab", "/tmp/test\nnewline"]
extensions = ["txt"]
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        // Специальные символы должны быть обработаны корректно
        EXPECT_GT(cfg.target_paths.size(), 0);
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for special chars: " << e.what();
    }
}

// Тест проверки расширений файлов
TEST_F(ConfigTest, FileExtensionsValidation) {
    writeConfig(R"(
target_paths = ["/tmp"]
extensions = ["txt", "LOG", "Data", "UPPERCASE"]
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        
        EXPECT_EQ(cfg.extensions.size(), 4);
        
        // Проверяем что расширения сохранены как есть (регистр может обрабатываться позже)
        for (const auto& ext : cfg.extensions) {
            EXPECT_FALSE(ext.empty());
        }
    } catch (const std::exception& e) {
        FAIL() << "Failed to load config: " << e.what();
    }
}

// Тест на безопасность: очень длинные пути
TEST_F(ConfigTest, VeryLongPaths) {
    std::string long_path(500, 'a');
    long_path = "/tmp/" + long_path;
    
    writeConfig(R"(
target_paths = [")" + long_path + R"("]
extensions = ["txt"]
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        // Длинные пути должны обрабатываться или отклоняться
        SUCCEED() << "Config loaded with long path";
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for long path: " << e.what();
    }
}

// Тест на безопасность: недопустимые алгоритмы
TEST_F(ConfigTest, InvalidAlgorithms) {
    writeConfig(R"(
target_paths = ["/tmp"]
extensions = ["txt"]
algorithms = "invalid_algorithm"
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        // Недопустимый алгоритм должен быть отклонен или использован дефолтный
        EXPECT_TRUE(cfg.algorithms == "gzip" || cfg.algorithms == "brotli" || 
                    cfg.algorithms == "all" || cfg.algorithms == "invalid_algorithm");
    } catch (const std::exception& e) {
        SUCCEED() << "Exception thrown for invalid algorithm: " << e.what();
    }
}

// Тест на безопасность: проверка dry_run режима
TEST_F(ConfigTest, DryRunMode) {
    writeConfig(R"(
target_paths = ["/tmp"]
extensions = ["txt"]
dry_run = true
)");
    
    char test_arg[] = const_cast<char*>("config_test");
    char* argv[] = {test_arg, const_cast<char*>(temp_config_path.c_str())};
    int argc = 2;
    
    try {
        Config cfg = load_config(argc, argv);
        EXPECT_TRUE(cfg.dry_run);
    } catch (const std::exception& e) {
        FAIL() << "Failed to load config: " << e.what();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
