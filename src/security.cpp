#include "security.h"
#include "logger.h"

#include <sys/prctl.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#include <grp.h>
#include <cstring>
#include <algorithm>

#if __has_include(<seccomp.h>)
#include <seccomp.h>
#define HAVE_SECCOMP 1
#else
#define HAVE_SECCOMP 0
#endif

namespace security {

bool is_running_as_root() {
    return geteuid() == 0;
}

bool drop_privileges(const std::string& username, const std::vector<std::string>& target_paths) {
    uid_t target_uid = 0;
    gid_t target_gid = 0;
    
    // Если указан явный пользователь, ищем его
    if (!username.empty()) {
        struct passwd* pw = getpwnam(username.c_str());
        if (!pw) {
            Logger::error(std::string("User not found: ") + username);
            return false;
        }
        target_uid = pw->pw_uid;
        target_gid = pw->pw_gid;
        Logger::info(std::string("Dropping privileges to user: ") + username + 
                    " (UID=" + std::to_string(target_uid) + ", GID=" + std::to_string(target_gid) + ")");
    } else {
        // Автоматическое определение владельца по целевым директориям
        if (target_paths.empty()) {
            Logger::error("No target paths specified for automatic privilege drop");
            return false;
        }
        
        // Проверяем владельцев всех путей - они должны совпадать
        uid_t first_uid = 0;
        gid_t first_gid = 0;
        bool first = true;
        
        for (const auto& path : target_paths) {
            struct stat st;
            if (stat(path.c_str(), &st) != 0) {
                Logger::error(std::string("Cannot stat path for ownership check: ") + path);
                return false;
            }
            
            if (first) {
                first_uid = st.st_uid;
                first_gid = st.st_gid;
                first = false;
            } else {
                if (st.st_uid != first_uid || st.st_gid != first_gid) {
                    Logger::warning(std::string("Target paths have different owners. ") +
                                   "Path '" + path + "' owner differs from first path. " +
                                   "Please specify 'run_as_user' in config explicitly.");
                    Logger::error("Automatic privilege drop failed due to conflicting path owners");
                    return false;
                }
            }
        }
        
        target_uid = first_uid;
        target_gid = first_gid;
        
        // Получаем имя пользователя для логирования
        struct passwd* pw = getpwuid(target_uid);
        std::string user_info = pw ? pw->pw_name : std::to_string(target_uid);
        Logger::info(std::string("Auto-detected owner: ") + user_info + 
                    " (UID=" + std::to_string(target_uid) + ", GID=" + std::to_string(target_gid) + ")");
    }
    
    // Сбрасываем дополнительные группы
    if (setgroups(0, NULL) != 0) {
        Logger::error(std::string("Failed to clear supplementary groups: ") + strerror(errno));
        return false;
    }
    
    // Устанавливаем GID
    if (setgid(target_gid) != 0) {
        Logger::error(std::string("Failed to set GID: ") + strerror(errno));
        return false;
    }
    
    // Устанавливаем UID
    if (setuid(target_uid) != 0) {
        Logger::error(std::string("Failed to set UID: ") + strerror(errno));
        return false;
    }
    
    // Запрещаем получение привилегий через execve
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        Logger::error(std::string("Failed to set NO_NEW_PRIVS: ") + strerror(errno));
        return false;
    }
    
    Logger::info("Privileges dropped successfully");
    return true;
}

bool init_seccomp() {
#if HAVE_SECCOMP
    scmp_filter_ctx ctx;
    
    // Инициализируем фильтр с действием по умолчанию: разрешать всё
    // Затем добавляем правила запрета для опасных вызовов
    ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == nullptr) {
        Logger::error("Failed to initialize seccomp context");
        return false;
    }
    
    // Запрещаем опасные системные вызовы
    
    // Выполнение программ
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(execve), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(execveat), 0);
    
    // Сетевые вызовы (демон не должен иметь сеть)
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(connect), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(accept), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(bind), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(listen), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(sendto), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(recvfrom), 0);
    
    // Отладка и трассировка
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(ptrace), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(process_vm_readv), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(process_vm_writev), 0);
    
    // Монтирование ФС
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(mount), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(umount2), 0);
    
    // Изменение имен процессов (маскировка)
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(prctl), 0);
    
    // Загрузка модулей ядра
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(init_module), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(finit_module), 0);
    seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(delete_module), 0);
    
    // Переименование и создание жестких ссылок (потенциально опасно)
    // seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(link), 0);
    // seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(linkat), 0);
    
    // Применяем фильтр
    int rc = seccomp_load(ctx);
    if (rc < 0) {
        Logger::error(std::string("Failed to load seccomp filter: ") + strerror(-rc));
        seccomp_release(ctx);
        return false;
    }
    
    seccomp_release(ctx);
    Logger::info("Seccomp sandbox initialized successfully");
    return true;
#else
    Logger::warning("Seccomp support not available (libseccomp not installed)");
    return false;
#endif
}

} // namespace security
