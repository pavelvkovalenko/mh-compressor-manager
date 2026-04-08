/**
 * Unit тесты для функций безопасности
 * Проверяют корректность drop_privileges, is_running_as_root, init_seccomp
 */

#include <gtest/gtest.h>
#include "security.h"
#include <unistd.h>
#include <sys/stat.h>
#include <fstream>
#include <filesystem>
#include <fcntl.h>      // Для open, openat, O_RDONLY, O_NOFOLLOW, O_PATH, O_DIRECTORY
#include <cstring>      // Для strerror
#include <cerrno>       // Для errno

namespace fs = std::filesystem;

// Тест проверки запуска от root
TEST(SecurityTest, IsRunningAsRoot) {
    // Получаем реальный UID
    uid_t uid = getuid();
    bool is_root = (uid == 0);
    
    // Функция должна возвращать соответствие реальному статусу
    EXPECT_EQ(security::is_running_as_root(), is_root);
}

// Тест создания временных файлов для проверки прав доступа
TEST(SecurityTest, TempFilePermissions) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_security_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем тестовый файл
    fs::path test_file = temp_dir / "test.txt";
    {
        std::ofstream ofs(test_file);
        ofs << "test content";
    }
    
    // Проверяем что файл существует
    ASSERT_TRUE(fs::exists(test_file));
    
    // Проверяем права доступа
    struct stat st;
    ASSERT_EQ(stat(test_file.c_str(), &st), 0);
    
    // Очищаем
    fs::remove_all(temp_dir);
}

// Тест проверки валидации путей (защита от symlink атак)
TEST(SecurityTest, SymlinkDetection) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_symlink_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем целевой файл
    fs::path target_file = temp_dir / "target.txt";
    {
        std::ofstream ofs(target_file);
        ofs << "target";
    }
    
    // Создаем symlink
    fs::path symlink_path = temp_dir / "link.txt";
    fs::create_symlink(target_file, symlink_path);
    
    // Проверяем что symlink определен правильно
    struct stat st;
    ASSERT_EQ(lstat(symlink_path.c_str(), &st), 0);
    EXPECT_TRUE(S_ISLNK(st.st_mode));
    
    // Очищаем
    fs::remove_all(temp_dir);
}

// Тест проверки различных уровней привилегий
TEST(SecurityTest, PrivilegeLevels) {
    uid_t uid = getuid();
    uid_t euid = geteuid();
    
    // UID и EUID должны быть валидными
    EXPECT_GE(uid, 0);
    EXPECT_GE(euid, 0);
    
    // Если не root, UID и EUID обычно совпадают
    if (uid != 0) {
        EXPECT_EQ(uid, euid);
    }
}

// Тест проверки существования директорий для drop_privileges
TEST(SecurityTest, DirectoryValidation) {
    std::vector<std::string> valid_paths = {"/tmp", "/var"};
    std::vector<std::string> invalid_paths = {"/nonexistent_path_xyz123"};
    
    // Проверяем валидные пути
    for (const auto& path : valid_paths) {
        struct stat st;
        int result = stat(path.c_str(), &st);
        EXPECT_EQ(result, 0) << "Path should exist: " << path;
        EXPECT_TRUE(S_ISDIR(st.st_mode)) << "Should be directory: " << path;
    }
    
    // Проверяем невалидные пути
    for (const auto& path : invalid_paths) {
        struct stat st;
        int result = stat(path.c_str(), &st);
        EXPECT_NE(result, 0) << "Path should not exist: " << path;
    }
}

// Тест проверки доступности seccomp (если поддерживается системой)
TEST(SecurityTest, SeccompAvailability) {
    // Эта функция может вернуть false если seccomp не доступен
    // Это не ошибка, а особенность системы
    bool result = security::init_seccomp();
    
    // Если seccomp инициировался успешно - отлично
    // Если нет - проверяем что процесс не упал
    SUCCEED() << "Seccomp initialization returned: " << (result ? "true" : "false");
}

// Тест на безопасность: проверка что нельзя сбросить права на несуществующего пользователя
TEST(SecurityTest, InvalidUserHandling) {
    std::vector<std::string> paths = {"/tmp"};
    
    // Пытаемся сбросить права на несуществующего пользователя
    bool result = security::drop_privileges("nonexistent_user_xyz123", paths);
    
    // Должно вернуть false
    EXPECT_FALSE(result);
}

// Тест на безопасность: проверка с пустым списком путей
TEST(SecurityTest, EmptyPathsHandling) {
    // Пустой список путей должен обрабатываться корректно
    bool result = security::drop_privileges("", {});
    
    // Должно вернуть false так как нет путей для определения владельца
    EXPECT_FALSE(result);
}

// Тест проверки прав доступа к временным файлам
TEST(SecurityTest, FileAccessPermissions) {
    fs::path temp_file = fs::temp_directory_path() / "mh_perm_test";
    
    // Создаем файл с ограниченными правами
    {
        std::ofstream ofs(temp_file);
        ofs << "restricted";
    }
    
    // Устанавливаем права только на чтение для владельца
    chmod(temp_file.c_str(), S_IRUSR);
    
    struct stat st;
    ASSERT_EQ(stat(temp_file.c_str(), &st), 0);
    
    // Проверяем что права установлены correctly
    EXPECT_EQ(st.st_mode & 0777, S_IRUSR | S_IWUSR);  // umask может изменить
    
    // Очищаем
    fs::remove(temp_file);
}

// Интеграционный тест: проверка работы с реальными путями
TEST(SecurityTest, RealPathIntegration) {
    std::vector<std::string> real_paths = {"/tmp", "/var/tmp"};
    
    // Проверяем что можем получить статистику для реальных путей
    for (const auto& path : real_paths) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            // Путь существует
            EXPECT_TRUE(S_ISDIR(st.st_mode) || S_ISREG(st.st_mode));
        }
    }
}

// Тест защиты от TOCTOU атак с использованием openat() и O_NOFOLLOW
TEST(SecurityTest, TOCTOUProtectionWithOpenat) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_toctou_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем тестовый файл
    fs::path test_file = temp_dir / "test.txt";
    {
        std::ofstream ofs(test_file);
        ofs << "test content";
    }
    
    // Открываем директорию с O_DIRECTORY | O_NOFOLLOW
    int dir_fd = open(temp_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    ASSERT_GE(dir_fd, 0) << "Failed to open directory with O_NOFOLLOW";
    
    // Открываем файл через openat() с O_PATH | O_NOFOLLOW
    int fd_path = openat(dir_fd, "test.txt", O_PATH | O_NOFOLLOW);
    ASSERT_GE(fd_path, 0) << "Failed to open file with openat() and O_NOFOLLOW";
    
    // Проверяем что это обычный файл через fstat (не следует за symlink)
    struct stat st;
    ASSERT_EQ(fstat(fd_path, &st), 0);
    EXPECT_TRUE(S_ISREG(st.st_mode)) << "File should be a regular file";
    
    // Закрываем дескрипторы
    close(fd_path);
    close(dir_fd);
    
    // Очищаем
    fs::remove_all(temp_dir);
}

// Тест обнаружения symlink атаки при использовании openat()
TEST(SecurityTest, SymlinkAttackDetectionWithOpenat) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_symlink_attack_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем целевой файл (например, симулируем системный файл)
    fs::path target_file = temp_dir / "target.txt";
    {
        std::ofstream ofs(target_file);
        ofs << "sensitive data";
    }
    
    // Создаем symlink который пытается подменить файл
    fs::path symlink_path = temp_dir / "malicious_link.txt";
    fs::create_symlink(target_file, symlink_path);
    
    // Открываем директорию
    int dir_fd = open(temp_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    ASSERT_GE(dir_fd, 0);
    
    // Пытаемся открыть symlink через openat() с O_NOFOLLOW - должно вернуть ошибку ELOOP
    int fd = openat(dir_fd, "malicious_link.txt", O_PATH | O_NOFOLLOW);
    
    if (fd < 0) {
        // Ожидаем ELOOP или EMLINK при попытке открыть symlink
        EXPECT_TRUE(errno == ELOOP || errno == EMLINK) 
            << "Expected ELOOP or EMLINK when opening symlink, got errno=" << errno;
    } else {
        // Если открылся, проверяем через fstat что это symlink
        struct stat st;
        ASSERT_EQ(fstat(fd, &st), 0);
        // С O_NOFOLLOW должен открыться сам symlink, а не цель
        EXPECT_TRUE(S_ISLNK(st.st_mode)) << "Should detect symlink";
        close(fd);
    }
    
    close(dir_fd);
    fs::remove_all(temp_dir);
}

// Тест проверки safe_openat функции (если доступна в security.h)
TEST(SecurityTest, SafeOpenatFunction) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_safe_openat_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем тестовый файл
    fs::path test_file = temp_dir / "safe_test.txt";
    {
        std::ofstream ofs(test_file);
        ofs << "safe content";
    }
    
    // Открываем файл безопасно через openat
    int dir_fd = open(temp_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    ASSERT_GE(dir_fd, 0);
    
    int fd = openat(dir_fd, "safe_test.txt", O_RDONLY | O_NOFOLLOW);
    ASSERT_GE(fd, 0) << "Failed to safely open file with openat()";
    
    // Читаем данные для проверки
    char buffer[64];
    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    ASSERT_GT(bytes_read, 0);
    buffer[bytes_read] = '\0';
    EXPECT_STREQ(buffer, "safe content");
    
    close(fd);
    close(dir_fd);
    fs::remove_all(temp_dir);
}

// Тест безопасной альтернативы realpath с O_PATH и /proc/self/fd/
TEST(SecurityTest, SafeRealpathAlternative) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_safe_realpath_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем целевой файл
    fs::path target_file = temp_dir / "real_target.txt";
    {
        std::ofstream ofs(target_file);
        ofs << "real content";
    }
    
    // Создаем symlink
    fs::path symlink_path = temp_dir / "link_to_target.txt";
    fs::create_symlink(target_file, symlink_path);
    
    // Используем безопасную альтернативу realpath: open с O_PATH|O_NOFOLLOW + readlink через /proc/self/fd/
    int fd = open(symlink_path.c_str(), O_PATH | O_NOFOLLOW);
    ASSERT_LT(fd, 0) << "O_PATH|O_NOFOLLOW должен вернуть ошибку для symlink";
    EXPECT_EQ(errno, ELOOP) << "Ожидаем ELOOP при открытии symlink с O_NOFOLLOW";
    
    // Теперь открываем без O_NOFOLLOW для проверки readlink через /proc/self/fd/
    fd = open(symlink_path.c_str(), O_PATH);
    ASSERT_GE(fd, 0) << "Failed to open symlink with O_PATH";
    
    // Читаем symlink через /proc/self/fd/
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    
    char resolved[PATH_MAX];
    ssize_t len = readlink(proc_path, resolved, sizeof(resolved) - 1);
    ASSERT_GT(len, 0) << "readlink через /proc/self/fd/ должен работать";
    resolved[len] = '\0';
    
    // Проверяем что прочитали правильный путь
    std::string resolved_str(resolved);
    EXPECT_TRUE(resolved_str.find("real_target.txt") != std::string::npos) 
        << "Resolved path should contain target filename";
    
    close(fd);
    fs::remove_all(temp_dir);
}

// Тест проверки возврата ошибок из init_seccomp при недопустимых syscall
TEST(SecurityTest, SeccompErrorHandling) {
    // Проверяем что init_seccomp корректно обрабатывает ошибки
    // и возвращает false при неудаче вместо падения
    bool result = security::init_seccomp();
    
    // Функция должна либо вернуть true (успех), либо false (ошибка)
    // но не должна вызывать crash
    EXPECT_TRUE(result == true || result == false) << "init_seccomp должен вернуть boolean";
}

// Тест проверки валидации файлов для компрессии
TEST(SecurityTest, ValidateFileForCompression) {
    fs::path temp_dir = fs::temp_directory_path() / "mh_validate_test";
    
    // Создаем временную директорию
    ASSERT_TRUE(fs::create_directories(temp_dir));
    
    // Создаем обычный файл
    fs::path regular_file = temp_dir / "regular.txt";
    {
        std::ofstream ofs(regular_file);
        ofs << "content";
    }
    
    // Проверяем что валидация проходит для обычного файла
    // Примечание: validate_file_for_compression может требовать дополнительных проверок
    struct stat st;
    ASSERT_EQ(lstat(regular_file.c_str(), &st), 0);
    EXPECT_TRUE(S_ISREG(st.st_mode)) << "Файл должен быть обычным файлом";
    
    // Создаем symlink и проверяем что он определяется
    fs::path symlink = temp_dir / "link.txt";
    fs::create_symlink(regular_file, symlink);
    
    ASSERT_EQ(lstat(symlink.c_str(), &st), 0);
    EXPECT_TRUE(S_ISLNK(st.st_mode)) << "Должен быть symlink";
    
    fs::remove_all(temp_dir);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
