#include "thread_policy.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

namespace teleop {
namespace {
long long migrations() noexcept
{
    char path[96]{};
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/sched", syscall(SYS_gettid));
    FILE* file = std::fopen(path, "r");
    if (!file) return -1;
    char line[256]{};
    long long count = -1;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strstr(line, "nr_migrations")) {
            const char* colon = std::strchr(line, ':');
            if (colon) (void)std::sscanf(colon + 1, "%lld", &count);
            break;
        }
    }
    std::fclose(file);
    return count;
}
double milliseconds(const timeval& value)
{
    return static_cast<double>(value.tv_sec) * 1000.0 + static_cast<double>(value.tv_usec) / 1000.0;
}
} // namespace

CpuLatencyRequest::CpuLatencyRequest(int microseconds)
{
    if (microseconds < 0) return;
    fd_ = open("/dev/cpu_dma_latency", O_RDWR | O_CLOEXEC);
    if (fd_ < 0) throw std::runtime_error("CPU latency QoS open: " + std::string{std::strerror(errno)});
    const std::int32_t value = microseconds;
    ssize_t written;
    do { written = write(fd_, &value, sizeof(value)); } while (written < 0 && errno == EINTR);
    std::int32_t effective = -1;
    ssize_t received = -1;
    if (written == static_cast<ssize_t>(sizeof(value))) {
        do { received = read(fd_, &effective, sizeof(effective)); } while (received < 0 && errno == EINTR);
    }
    if (written != static_cast<ssize_t>(sizeof(value)) || received != static_cast<ssize_t>(sizeof(effective)) ||
        effective < 0 || effective > value) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("CPU latency QoS request/readback failed");
    }
    std::printf("cpu-latency: requested_us=%d effective_us=%d\n", microseconds, effective);
}
CpuLatencyRequest::~CpuLatencyRequest() { if (fd_ >= 0) close(fd_); }

void configure_worker(std::string_view role, int cpu, int rr_priority)
{
    const std::string name = "teleop-" + std::string{role};
    const int named = pthread_setname_np(pthread_self(), name.c_str());
    if (named != 0) throw std::runtime_error("worker name: " + std::string{std::strerror(named)});
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0) {
        throw std::runtime_error("could not read worker CPU affinity");
    }
    if (cpu >= 0) {
        if (cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) {
            throw std::runtime_error(std::string{role} + ": requested CPU is outside inherited allowed affinity");
        }
        cpu_set_t selected;
        CPU_ZERO(&selected);
        CPU_SET(cpu, &selected);
        const int applied = pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected);
        if (applied != 0) throw std::runtime_error(std::string{role} + ": CPU affinity: " + std::strerror(applied));
    }
    if (rr_priority != 0) {
        const sched_param requested{.sched_priority = rr_priority};
        const int applied = pthread_setschedparam(pthread_self(), SCHED_RR, &requested);
        if (applied != 0) throw std::runtime_error(std::string{role} + ": SCHED_RR: " + std::strerror(applied));
    }
    int policy = 0;
    sched_param effective{};
    if (pthread_getschedparam(pthread_self(), &policy, &effective) != 0 ||
        pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) != 0) {
        throw std::runtime_error("could not read effective worker scheduling settings");
    }
    const char* policy_name = policy == SCHED_RR ? "rr" : policy == SCHED_FIFO ? "fifo" :
                              policy == SCHED_OTHER ? "other" : "other-policy";
    std::string affinity;
    for (int first = 0; first < CPU_SETSIZE; ++first) {
        if (!CPU_ISSET(first, &allowed)) continue;
        int last = first;
        while (last + 1 < CPU_SETSIZE && CPU_ISSET(last + 1, &allowed)) ++last;
        if (!affinity.empty()) affinity += ',';
        affinity += std::to_string(first);
        if (last != first) affinity += '-' + std::to_string(last);
        first = last;
    }
    std::printf("thread-policy: role=%.*s tid=%ld policy=%s priority=%d nice=%d affinity=%s cpu=%d requested_cpu=%d\n",
        static_cast<int>(role.size()), role.data(), syscall(SYS_gettid), policy_name, effective.sched_priority,
        getpriority(PRIO_PROCESS, 0), affinity.c_str(), sched_getcpu(), cpu);
}

ThreadObservation::ThreadObservation(std::string_view role) noexcept : role_(role)
{
    usage_valid_ = getrusage(RUSAGE_THREAD, &before_) == 0;
    migrations_before_ = migrations();
}
ThreadObservation::~ThreadObservation()
{
    rusage after{};
    const auto migrations_after = migrations();
    if (!usage_valid_ || getrusage(RUSAGE_THREAD, &after) != 0) return;
    const long long migrated = migrations_before_ >= 0 && migrations_after >= migrations_before_ ?
                              migrations_after - migrations_before_ : -1;
    std::printf("thread-run: role=%.*s user_ms=%.6f system_ms=%.6f voluntary_switches=%ld involuntary_switches=%ld migrations=%lld\n",
        static_cast<int>(role_.size()), role_.data(), milliseconds(after.ru_utime) - milliseconds(before_.ru_utime),
        milliseconds(after.ru_stime) - milliseconds(before_.ru_stime), after.ru_nvcsw - before_.ru_nvcsw,
        after.ru_nivcsw - before_.ru_nivcsw, migrated);
}
} // namespace teleop
