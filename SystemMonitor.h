#ifndef SYSTEM_MONITOR_H
#define SYSTEM_MONITOR_H

#include <string>
#include <vector>
#include <map>
#include <chrono>
#include "Process.h"

// ---------------------------------------------------------------------------
// One network interface's live throughput (Phase 2).
// rx/txKBps are RATES computed the same two-sample way as CPU%: diff the
// kernel's cumulative byte counters between refreshes and divide by elapsed
// time.
// ---------------------------------------------------------------------------
struct NetInterface {
    std::string name;
    double rxKBps = 0.0;      // download rate  (KB/s)
    double txKBps = 0.0;      // upload rate    (KB/s)
};

// ---------------------------------------------------------------------------
// One mounted filesystem's capacity (Phase 2), from statvfs().
// ---------------------------------------------------------------------------
struct DiskMount {
    std::string device;       // e.g. /dev/nvme0n1p2
    std::string mountPoint;   // e.g. /
    std::string fsType;       // e.g. ext4
    long totalKB = 0;
    long usedKB  = 0;
    double usedPct = 0.0;
};

// ---------------------------------------------------------------------------
// SystemMonitor
// ---------------------------------------------------------------------------
// The "brain" of the program. It reads the /proc and /sys filesystems on every
// refresh and turns raw kernel data into numbers we can display:
//   - overall + per-core CPU usage %
//   - RAM / swap usage
//   - CPU temperature                              (Phase 2)
//   - network throughput per interface             (Phase 2)
//   - disk usage per mount + aggregate disk I/O    (Phase 2)
//   - the list of processes (with full command line)
//
// Several metrics are RATES (CPU%, network, disk I/O): you can only know a rate
// by comparing two snapshots taken a moment apart, so we keep the previous
// sample as member state and diff it each refresh.
// ---------------------------------------------------------------------------
class SystemMonitor {
public:
    SystemMonitor();

    // Called once per refresh. Reads everything fresh from /proc and /sys.
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

    // Phase 2 getters.
    bool   hasCpuTemp()   const { return hasCpuTemp_; }
    double getCpuTempC()  const { return cpuTempC_; }
    const std::string& getCpuTempLabel() const { return cpuTempLabel_; }

    // GPU getters (NVIDIA via nvidia-smi; absent on other machines).
    bool   hasGpu()          const { return hasGpu_; }
    const std::string& getGpuName() const { return gpuName_; }
    double getGpuUtil()      const { return gpuUtil_; }
    long   getGpuMemUsedMB() const { return gpuMemUsedMB_; }
    long   getGpuMemTotalMB()const { return gpuMemTotalMB_; }
    double getGpuTempC()     const { return gpuTempC_; }
    const std::vector<NetInterface>& getNetInterfaces() const { return netInterfaces_; }
    double getNetRxKBps() const { return netRxKBps_; }
    double getNetTxKBps() const { return netTxKBps_; }
    double getDiskReadKBps()  const { return diskReadKBps_; }
    double getDiskWriteKBps() const { return diskWriteKBps_; }
    const std::vector<DiskMount>& getDiskMounts() const { return diskMounts_; }

    // Send a signal to a process (used by the "kill" feature).
    // Returns true on success. This demonstrates OS signals in action.
    static bool killProcess(int pid, int signal);

    // Change a process's scheduling priority via setpriority() (Phase 2).
    // `niceValue` is the standard -20 (highest) .. 19 (lowest) range.
    // Returns true on success (needs privileges to lower the nice value).
    static bool reniceProcess(int pid, int niceValue);

private:
    void readMemInfo();       // parse /proc/meminfo
    void readCpuStat();       // parse /proc/stat  -> overall CPU %
    void readProcesses();     // walk /proc/<pid>/ -> per-process info
    void readThermal();       // parse /sys/class/thermal -> CPU temperature
    void readNetDev();        // parse /proc/net/dev  -> per-interface throughput
    void readDiskStats();     // parse /proc/diskstats -> aggregate disk I/O rate
    void readDiskUsage();     // parse /proc/mounts + statvfs -> capacity per mount
    void readGpu();           // run nvidia-smi -> GPU load / memory / temperature

    // Elapsed wall-clock time between the last two update() calls. Rate metrics
    // (network, disk I/O) divide their byte/sector delta by this.
    std::chrono::steady_clock::time_point lastTime_;
    bool   haveLastTime_ = false;
    double dtSec_ = 0.0;

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

    // --- CPU temperature (Phase 2) ---
    bool        hasCpuTemp_ = false;
    double      cpuTempC_ = 0.0;
    std::string cpuTempLabel_;

    // --- GPU (NVIDIA via nvidia-smi) ---
    bool        gpuAvailable_ = true;   // becomes false if nvidia-smi is missing
    bool        hasGpu_ = false;
    std::string gpuName_;
    double      gpuUtil_ = 0.0;         // GPU load %
    long        gpuMemUsedMB_ = 0, gpuMemTotalMB_ = 0;
    double      gpuTempC_ = 0.0;

    // --- Network throughput (Phase 2) ---
    std::vector<NetInterface> netInterfaces_;
    double netRxKBps_ = 0.0, netTxKBps_ = 0.0;   // totals across interfaces
    // Previous cumulative rx/tx byte counters, keyed by interface name.
    std::map<std::string, std::pair<unsigned long long, unsigned long long>> prevNet_;

    // --- Disk I/O rate (Phase 2) ---
    double diskReadKBps_ = 0.0, diskWriteKBps_ = 0.0;
    unsigned long long prevReadSectors_ = 0, prevWriteSectors_ = 0;
    bool haveDiskStats_ = false;

    // --- Disk usage (Phase 2) ---
    std::vector<DiskMount> diskMounts_;
};

#endif // SYSTEM_MONITOR_H
