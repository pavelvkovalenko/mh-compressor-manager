#include "security.h"
#include "logger.h"

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
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
    
    // Инициализируем фильтр с действием по умолчанию: ЗАПРЕЩАТЬ всё (SCMP_ACT_ERRNO)
    // Затем добавляем правила разрешения для необходимых вызовов
    // Это более безопасный подход - разрешаем только то что нужно
    ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (ctx == nullptr) {
        Logger::error("Failed to initialize seccomp context");
        return false;
    }
    
    // Разрешаем необходимые системные вызовы для работы компрессора
    // Минимальный набор для безопасности (принцип наименьших привилегий)
    
    // Базовые вызовы для работы с файлами
    int rc;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(open), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(openat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(close), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(read), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(write), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lseek), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fsync), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fdatasync), 0);
    if (rc < 0) goto error;
    
    // Вызовы для stat/fstat/lstat
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(stat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fstat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(lstat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(newfstatat), 0);
    if (rc < 0) goto error;
    
    // Вызовы для работы с директориями
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getdents), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getdents64), 0);
    if (rc < 0) goto error;
    
    // Вызовы для доступа к файлам через /proc/self/fd/
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlink), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readlinkat), 0);
    if (rc < 0) goto error;
    
    // Вызовы для управления правами и владельцами
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(chmod), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fchmod), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fchmodat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(chown), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fchown), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fchownat), 0);
    if (rc < 0) goto error;
    
    // Вызовы для работы с временными метками
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(utimensat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futimens), 0);
    if (rc < 0) goto error;
    
    // Вызовы для удаления файлов
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(unlink), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(unlinkat), 0);
    if (rc < 0) goto error;
    
    // Вызовы для работы с памятью
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mmap), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(munmap), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(mprotect), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(brk), 0);
    if (rc < 0) goto error;
    
    // Вызовы для потоков
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(futex), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(set_robust_list), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(get_robust_list), 0);
    if (rc < 0) goto error;
    
    // Вызовы для epoll/select/poll (используется в threadpool/monitor)
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_create), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_create1), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_ctl), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_wait), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(epoll_pwait), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(select), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(pselect6), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(poll), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(ppoll), 0);
    if (rc < 0) goto error;
    
    // Вызовы для работы с сигналами
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigaction), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(rt_sigreturn), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(sigaltstack), 0);
    if (rc < 0) goto error;
    
    // Вызовы для getrusage/gettimeofday/clock_gettime
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(gettimeofday), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clock_gettime), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clock_getres), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clock_nanosleep), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(nanosleep), 0);
    if (rc < 0) goto error;
    
    // Вызовы для getuid/getgid/geteuid/getegid
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getuid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getgid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(geteuid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getegid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getpid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(gettid), 0);
    if (rc < 0) goto error;
    
    // Вызовы для syslog (только sendto для локального syslog, без socket/connect)
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(sendto), 0);
    if (rc < 0) goto error;
    
    // Вызовы для prctl (NO_NEW_PRIVS)
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(prctl), 0);
    if (rc < 0) goto error;
    
    // Вызовы для setuid/setgid при сбросе привилегий
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setuid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setgid), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(setgroups), 0);
    if (rc < 0) goto error;
    
    // Вызовы для posix_fadvise
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fadvise64), 0);
    if (rc < 0) goto error;
    
    // Вызовы для getrandom
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getrandom), 0);
    if (rc < 0) goto error;
    
    // Вызовы для access/faccessat (проверка прав доступа)
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(access), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(faccessat), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(faccessat2), 0);
    if (rc < 0) goto error;
    
    // Вызовы для readv/writev
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readv), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(writev), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(pread64), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(pwrite64), 0);
    if (rc < 0) goto error;
    
    // Вызовы для dup/dup2/dup3
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(dup), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(dup2), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(dup3), 0);
    if (rc < 0) goto error;
    
    // Вызовы для fcntl
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fcntl), 0);
    if (rc < 0) goto error;
    
    // Вызовы для wait4 (если используются дочерние процессы)
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(wait4), 0);
    if (rc < 0) goto error;
    
    // Вызовы для uname/getcwd/readahead
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(uname), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(getcwd), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(readahead), 0);
    if (rc < 0) goto error;
    
    // Вызовы для sync_file_range/fallocate
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(sync_file_range), 0);
    if (rc < 0) goto error;
    rc = seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(fallocate), 0);
    if (rc < 0) goto error;
    
    // Применяем фильтр
    rc = seccomp_load(ctx);
    if (rc < 0) {
        Logger::error(std::string("Failed to load seccomp filter: ") + strerror(-rc));
        seccomp_release(ctx);
        return false;
    }
    
    seccomp_release(ctx);
    Logger::info("Seccomp sandbox initialized successfully (minimal syscall whitelist)");
    return true;
    
error:
    Logger::error(std::string("Failed to add seccomp rule: ") + strerror(-rc));
    seccomp_release(ctx);
    return false;
#else
    Logger::warning("Seccomp support not available (libseccomp not installed)");
    return false;
#endif
}

} // namespace security
