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

} // namespace security

#endif // SECURITY_H
