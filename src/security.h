#ifndef SECURITY_H
#define SECURITY_H

#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>

namespace security {

/**
 * Инициализирует песочницу системных вызовов через seccomp-bpf.
 * Разрешает только минимально необходимый набор syscall для работы демона.
 * 
 * @return true если успешно, false если seccomp не поддерживается или отключен
 */
bool init_seccomp();

/**
 * Сбрасывает привилегии процесса до указанного пользователя.
 * Если username пустой, определяет владельца первой целевой директории.
 * 
 * @param username Имя пользователя для сброса прав (опционально)
 * @param target_paths Список целевых директорий для определения владельца
 * @return true если успешно, false если ошибка
 */
bool drop_privileges(const std::string& username, const std::vector<std::string>& target_paths);

/**
 * Проверяет, запущен ли процесс от root.
 * 
 * @return true если UID == 0
 */
bool is_running_as_root();

/**
 * Проверяет файл на безопасность перед сжатием.
 * 
 * @param path Путь к файлу
 * @return true если файл безопасен для сжатия
 */
bool validate_file_for_compression(const std::string& path);

/**
 * Безопасно открывает файл с защитой от TOCTOU атак.
 * 
 * @param path Путь к файлу
 * @param flags Флаги открытия
 * @return Дескриптор файла или -1 при ошибке
 */
int safe_open_file(const std::string& path, int flags);

/**
 * Проверяет имя файла на наличие null-byte инъекций и других опасных символов.
 * 
 * @param filename Имя файла для проверки
 * @return true если имя файла безопасно
 */
bool validate_filename(const std::string& filename);

/**
 * Класс для rate limiting операций сжатия.
 * Защищает от DoS-атак путем ограничения количества операций в единицу времени.
 */
class RateLimiter {
public:
    /**
     * Конструктор rate limiter.
     * 
     * @param max_operations Максимальное количество операций за окно времени
     * @param window_seconds Размер окна времени в секундах
     */
    explicit RateLimiter(size_t max_operations = 100, size_t window_seconds = 60);
    
    /**
     * Проверяет и регистрирует операцию.
     * 
     * @return true если операция разрешена, false если превышен лимит
     */
    bool try_acquire();
    
    /**
     * Сбрасывает счетчики rate limiter.
     */
    void reset();
    
    /**
     * Возвращает количество оставшихся операций в текущем окне.
     * 
     * @return Количество доступных операций
     */
    size_t available() const;

private:
    void cleanup_old_entries();
    
    const size_t m_max_operations;
    const size_t m_window_seconds;
    mutable std::mutex m_mutex;
    std::vector<std::chrono::steady_clock::time_point> m_timestamps;
};

/**
 * Глобальный rate limiter для всех операций сжатия.
 */
extern RateLimiter g_compression_rate_limiter;

} // namespace security

#endif // SECURITY_H
