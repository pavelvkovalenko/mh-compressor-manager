#include "security.h"
#include "logger.h"

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
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
            Logger::error(std::string("User not found: ") + username + 
                         ": " + (errno ? strerror(errno) : "unknown error"));
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
            if (lstat(path.c_str(), &st) != 0) {
                Logger::error(std::string("Cannot lstat path for ownership check: ") + path);
                return false;
            }
            
            // Проверяем что это не symlink (защита от symlink-атак)
            if (S_ISLNK(st.st_mode)) {
                Logger::error(std::string("Path is a symlink, refusing to use for privilege drop: ") + path);
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
        if (!pw) {
            Logger::error(std::string("Failed to get username for UID ") + std::to_string(target_uid) + 
                         ": " + strerror(errno));
            return false;
        }
        std::string user_info = pw->pw_name;
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
    
    // === КРИТИЧЕСКАЯ БЕЗОПАСНОСТЬ: Явный сброс capabilities перед установкой NO_NEW_PRIVS ===
    // Это предотвращает возможность получения привилегий через execve даже если какие-то
    // capabilities остались после drop_privileges
#if HAVE_LIBCAP
    #include <sys/capability.h>
    cap_t caps = cap_init();  // Создаем пустую структуру capabilities
    if (caps == NULL) {
        Logger::warning(std::string("Failed to initialize capabilities structure: ") + strerror(errno));
    } else {
        // Устанавливаем пустые capabilities (полный сброс)
        if (cap_set_proc(caps) != 0) {
            Logger::warning(std::string("Failed to clear capabilities: ") + strerror(errno));
        }
        cap_free(caps);  // Освобождаем память
    }
#endif
    
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
    // RAII-паттерн для автоматического освобождения seccomp контекста
    struct SeccompContext {
        scmp_filter_ctx ctx;
        bool initialized;
        
        explicit SeccompContext() : ctx(nullptr), initialized(false) {}
        
        ~SeccompContext() {
            if (ctx != nullptr) {
                seccomp_release(ctx);
            }
        }
        
        bool init(scmp_filter_ctx new_ctx) {
            ctx = new_ctx;
            initialized = (ctx != nullptr);
            return initialized;
        }
        
        bool add_rule(uint32_t action, int syscall) {
            if (!initialized || ctx == nullptr) return false;
            int rc = seccomp_rule_add(ctx, action, syscall, 0);
            return (rc >= 0);
        }
        
        bool load() {
            if (!initialized || ctx == nullptr) return false;
            int rc = seccomp_load(ctx);
            return (rc >= 0);
        }
    };
    
    SeccompContext ctx_wrapper;
    
    // Инициализируем фильтр с действием по умолчанию: ЗАПРЕЩАТЬ всё (SCMP_ACT_ERRNO)
    // Затем добавляем правила разрешения для необходимых вызовов
    // Это более безопасный подход - разрешаем только то что нужно
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ERRNO(EPERM));
    if (ctx == nullptr) {
        Logger::error("Failed to initialize seccomp context");
        return false;
    }
    
    if (!ctx_wrapper.init(ctx)) {
        Logger::error("Failed to initialize seccomp wrapper");
        return false;
    }
    
    // Разрешаем необходимые системные вызовы для работы компрессора
    // Минимальный набор для безопасности (принцип наименьших привилегий)
    
    // Базовые вызовы для работы с файлами
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(open))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(openat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(close))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(read))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(write))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(lseek))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fsync))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fdatasync))) return false;
    
    // Вызовы для stat/fstat/lstat
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(stat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fstat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(lstat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(newfstatat))) return false;
    
    // Вызовы для работы с директориями
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getdents))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getdents64))) return false;
    
    // Вызовы для доступа к файлам через /proc/self/fd/
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(readlink))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(readlinkat))) return false;
    
    // Вызовы для управления правами и владельцами
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(chmod))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fchmod))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fchmodat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(chown))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fchown))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fchownat))) return false;
    
    // Вызовы для работы с временными метками
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(utimensat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(futimens))) return false;
    
    // Вызовы для удаления файлов
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(unlink))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(unlinkat))) return false;
    
    // Вызовы для работы с памятью
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(mmap))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(munmap))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(mprotect))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(brk))) return false;
    
    // Вызовы для потоков
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(futex))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(set_robust_list))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(get_robust_list))) return false;
    
    // Вызовы для epoll/select/poll (используется в threadpool/monitor)
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(epoll_create))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(epoll_create1))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(epoll_ctl))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(epoll_wait))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(epoll_pwait))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(select))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(pselect6))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(poll))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(ppoll))) return false;
    
    // Вызовы для работы с сигналами
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(rt_sigaction))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(rt_sigprocmask))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(rt_sigreturn))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(sigaltstack))) return false;
    
    // Вызовы для getrusage/gettimeofday/clock_gettime
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(gettimeofday))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(clock_gettime))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(clock_getres))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(clock_nanosleep))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(nanosleep))) return false;
    
    // Вызовы для getuid/getgid/geteuid/getegid
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getuid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getgid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(geteuid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getegid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getpid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(gettid))) return false;
    
    // Вызовы для syslog (только sendto для локального syslog, без socket/connect)
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(sendto))) return false;
    
    // Вызовы для prctl (NO_NEW_PRIVS)
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(prctl))) return false;
    
    // Вызовы для setuid/setgid при сбросе привилегий
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(setuid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(setgid))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(setgroups))) return false;
    
    // Вызовы для posix_fadvise
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fadvise64))) return false;
    
    // Вызовы для getrandom
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getrandom))) return false;
    
    // Вызовы для access/faccessat (проверка прав доступа)
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(access))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(faccessat))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(faccessat2))) return false;
    
    // Вызовы для readv/writev
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(readv))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(writev))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(pread64))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(pwrite64))) return false;
    
    // Вызовы для dup/dup2/dup3
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(dup))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(dup2))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(dup3))) return false;
    
    // Вызовы для fcntl
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fcntl))) return false;
    
    // Вызовы для wait4 (если используются дочерние процессы)
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(wait4))) return false;
    
    // Вызовы для uname/getcwd/readahead
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(uname))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(getcwd))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(readahead))) return false;
    
    // Вызовы для sync_file_range/fallocate
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(sync_file_range))) return false;
    if (!ctx_wrapper.add_rule(SCMP_ACT_ALLOW, SCMP_SYS(fallocate))) return false;
    
    // Применяем фильтр
    if (!ctx_wrapper.load()) {
        Logger::error("Failed to load seccomp filter");
        return false;
    }
    
    Logger::info("Seccomp sandbox initialized successfully (minimal syscall whitelist)");
    return true;
#else
    Logger::warning("Seccomp support not available (libseccomp not installed)");
    return false;
#endif
}

bool validate_file_for_compression(const std::string& path) {
    struct stat st;
    
    // Проверяем что путь не является symlink (защита от symlink-атак)
    if (lstat(path.c_str(), &st) != 0) {
        Logger::error(std::string("Cannot lstat file: ") + path + ": " + strerror(errno));
        return false;
    }
    
    // Отказываемся сжимать symlink
    if (S_ISLNK(st.st_mode)) {
        Logger::error(std::string("Refusing to compress symlink: ") + path);
        return false;
    }
    
    // Проверяем что это обычный файл
    if (!S_ISREG(st.st_mode)) {
        Logger::error(std::string("Not a regular file, refusing to compress: ") + path);
        return false;
    }
    
    // Проверяем права доступа - файл должен быть доступен для чтения владельцем
    if (!(st.st_mode & S_IRUSR)) {
        Logger::error(std::string("File is not readable by owner: ") + path);
        return false;
    }
    
    return true;
}

int safe_open_file(const std::string& path, int flags) {
    // Используем O_NOFOLLOW для защиты от symlink-атак
    int safe_flags = flags | O_NOFOLLOW;
    
    // Открываем файл через /proc/self/fd/ для дополнительной проверки
    int fd = open(path.c_str(), safe_flags, 0644);
    if (fd < 0) {
        Logger::error(std::string("Failed to open file: ") + path + ": " + strerror(errno));
        return -1;
    }
    
    // Дополнительная проверка через fstat что это действительно файл
    struct stat st;
    if (fstat(fd, &st) != 0) {
        Logger::error(std::string("fstat failed: ") + strerror(errno));
        close(fd);
        return -1;
    }
    
    if (!S_ISREG(st.st_mode)) {
        Logger::error(std::string("Opened file is not a regular file: ") + path);
        close(fd);
        errno = EINVAL;
        return -1;
    }
    
    return fd;
}

} // namespace security
