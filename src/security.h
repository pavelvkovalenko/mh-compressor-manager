#ifndef SECURITY_H
#define SECURITY_H

#include <string>
#include <vector>

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

} // namespace security

#endif // SECURITY_H
