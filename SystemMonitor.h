#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <string>
#include <vector>
#include <map>
#include "Process.h"

// ---------------------------------------------------------------------------
// SystemMonitor
// ---------------------------------------------------------------------------
// The "brain" of the program. It reads the /proc filesystem on every refresh
// and turns raw kernel data into numbers we can display:
//   - overall CPU usage %
//   - RAM / swap usage
//   - the list of processes
//
// It keeps the PREVIOUS CPU sample as member state, because CPU usage is a
// RATE: you can only know "how busy was the CPU" by comparing two snapshots
// taken a moment apart.
// ---------------------------------------------------------------------------
class SystemMonitor {
public:
    SystemMonitor();

    // Called once per refresh. Reads everything fresh from /proc.
    void update();

    // --- Getters used by the display layer ---
    double getCpuPercent() const { return cpuPercent_; }
    long getTotalMemKB()  const { return totalMemKB_; }
    long getUsedMemKB()   const { return usedMemKB_; }
    long getTotalSwapKB() const { return totalSwapKB_; }
    long getUsedSwapKB()  const { return usedSwapKB_; }
    int  getNumCores()    const { return numCores_; }
    const std::vector<double>& getCorePercents() const { return corePercents_; }
    const std::vector<Process>& getProcesses() const { return processes_; }

    // Send a signal to a process (used by the "kill" feature).
    // Returns true on success. This demonstrates OS signals in action.
    static bool killProcess(int pid, int signal);

private:
    void readMemInfo();       // parse /proc/meminfo
    void readCpuStat();       // parse /proc/stat  -> overall CPU %
    void readProcesses();     // walk /proc/<pid>/ -> per-process info

    // Overall CPU accounting (jiffies from the aggregate "cpu" line).
    long prevTotalJiffies_ = 0;
    long prevIdleJiffies_  = 0;
    long totalJiffiesDelta_ = 0;   // reused when computing per-process CPU%
    double cpuPercent_ = 0.0;

    // Per-core CPU accounting: one entry per core, plus its previous sample.
    std::vector<double> corePercents_;
    std::vector<long>   prevCoreTotal_;
    std::vector<long>   prevCoreIdle_;

    long totalMemKB_ = 0, usedMemKB_ = 0;
    long totalSwapKB_ = 0, usedSwapKB_ = 0;
    int  numCores_ = 1;

    std::vector<Process> processes_;

    // Remembers each process's previous CPU time across refreshes,
    // keyed by pid, so we can compute a per-process delta.
    std::map<int, long> prevProcTime_;
};

#endif // SYSTEM_MONITOR_H
