#include "memoryusage.h"
#ifdef Q_OS_MACOS
#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif
namespace MemoryUsage {
Sample read(const QList<qint64> &children) {
    Sample s;
#ifdef Q_OS_MACOS
    mach_task_basic_info_data_t task{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&task),
                  &count) == KERN_SUCCESS)
        s.current = task.resident_size;
    vm_statistics64_data_t vm{};
    count = HOST_VM_INFO64_COUNT;
    auto host = mach_host_self();
    vm_size_t page = 0;
    host_page_size(host, &page);
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) ==
        KERN_SUCCESS)
        s.available = (quint64(vm.free_count) + vm.inactive_count) * page;
    mach_port_deallocate(mach_task_self(), host);
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        s.peak = usage.ru_maxrss;
    for (auto pid : children) {
        proc_taskinfo info{};
        if (proc_pidinfo(int(pid), PROC_PIDTASKINFO, 0, &info, sizeof(info)) == sizeof(info))
            s.current += info.pti_resident_size;
    }
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
        s.current = info.WorkingSetSize;
        s.peak = info.PeakWorkingSetSize;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
        s.available = memory.ullAvailPhys;
    for (auto pid : children) {
        auto process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, DWORD(pid));
        if (process) {
            if (GetProcessMemoryInfo(process, &info, sizeof(info)))
                s.current += info.WorkingSetSize;
            CloseHandle(process);
        }
    }
#else
    auto resident = [](qint64 pid) -> quint64 {
        QFile file(QString("/proc/%1/statm").arg(pid));
        if (!file.open(QIODevice::ReadOnly))
            return 0;
        auto values = file.readAll().simplified().split(' ');
        return values.value(1).toULongLong() * quint64(sysconf(_SC_PAGESIZE));
    };
    s.current = resident(QCoreApplication::applicationPid());
    for (auto pid : children)
        s.current += resident(pid);
    QFile file("/proc/meminfo");
    if (file.open(QIODevice::ReadOnly)) {
        const auto lines = file.readAll().split('\n');
        for (const auto &line : lines)
            if (line.startsWith("MemAvailable:"))
                s.available = line.simplified().split(' ').value(1).toULongLong() * 1024;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
        s.peak = quint64(usage.ru_maxrss) * 1024;
#endif
    s.peak = qMax(s.peak, s.current);
    return s;
}
} // namespace MemoryUsage
