#include "performance_optimizer.h"
#include "logger.h"
#include "i18n.h"
#include <sched.h>
#include <sys/sysinfo.h>
#include <cstring>

bool PerformanceOptimizer::set_cpu_affinity(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (core_id < 0 || core_id >= get_cpu_count()) {
        Logger::warning(_("Invalid CPU core ID: %d"), core_id);
        return false;
    }

    CPU_SET(core_id, &cpuset);

    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0) {
        Logger::error(_("Failed to set CPU affinity to core %d: %s"),
                                  core_id, strerror(errno));
        return false;
    }

    Logger::debug(_("CPU affinity set to core %d"), core_id);
    return true;
}

int PerformanceOptimizer::get_cpu_count() {
    int count = sysconf(_SC_NPROCESSORS_ONLN);
    return (count > 0) ? count : 1;
}
