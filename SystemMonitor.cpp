#include "SystemMonitor.h"

#include <fstream>     // std::ifstream  -> read /proc files like normal files
#include <sstream>     // std::istringstream -> tokenize a line
#include <string>
#include <vector>
#include <iterator>    // std::istreambuf_iterator -> slurp a whole file
#include <cctype>      // isdigit
#include <dirent.h>    // opendir/readdir -> list /proc directory entries
#include <unistd.h>    // sysconf, getpagesize
#include <csignal>     // kill(), SIGTERM, SIGKILL
#include <algorithm>   // std::sort
#include <sys/statvfs.h> // statvfs() -> free/total space of a mount
#include <sys/resource.h>// setpriority() -> renice a process
#include <cstdio>        // popen()/pclose() -> run nvidia-smi and read its output

// ===========================================================================
// Constructor: figure out how many CPU cores we have.
// sysconf(_SC_NPROCESSORS_ONLN) is a POSIX call returning the number of
// cores currently online. We need it to scale per-process CPU% like `top`
// (where 100% means "one full core").
// ===========================================================================
SystemMonitor::SystemMonitor() {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    numCores_ = (n > 0) ? static_cast<int>(n) : 1;
}

// ===========================================================================
// update(): one full refresh. Order matters — readCpuStat() computes the
// total-jiffies delta that readProcesses() reuses for per-process CPU%.
// ===========================================================================
void SystemMonitor::update() {
    // Measure elapsed wall-clock time since the last refresh so the rate
    // metrics (network, disk I/O) can convert a byte/sector delta into per-second
    // throughput. The refresh loop aims for ~1s but is never exactly 1s.
    auto now = std::chrono::steady_clock::now();
    dtSec_ = haveLastTime_
           ? std::chrono::duration<double>(now - lastTime_).count()
           : 0.0;
    lastTime_ = now;
    haveLastTime_ = true;

    readMemInfo();
    readCpuStat();
    readUptime();
    readLoadAvg();
    readProcesses();
    readThermal();
    readNetDev();
    readDiskStats();
    readDiskUsage();
    readGpu();
}

// ===========================================================================
// readMemInfo(): parse /proc/meminfo
// ---------------------------------------------------------------------------
// /proc/meminfo looks like:
//     MemTotal:       16321092 kB
//     MemAvailable:    9876543 kB
//     SwapTotal:       2097148 kB
//     SwapFree:        2097148 kB
// "MemAvailable" is the kernel's best estimate of memory usable by new apps
// WITHOUT swapping — a better "used" measure than MemFree, because Linux uses
// spare RAM for disk cache which is reclaimable.
// ===========================================================================
void SystemMonitor::readMemInfo() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return;

    long memTotal = 0, memAvailable = 0, swapTotal = 0, swapFree = 0;
    std::string key;
    long value;
    std::string unit;

    // Read "key value unit" triples until the file ends.
    while (file >> key >> value >> unit) {
        if      (key == "MemTotal:")     memTotal     = value;
        else if (key == "MemAvailable:") memAvailable = value;
        else if (key == "SwapTotal:")    swapTotal    = value;
        else if (key == "SwapFree:")     swapFree     = value;
    }

    totalMemKB_  = memTotal;
    usedMemKB_   = memTotal - memAvailable;   // what apps are actually using
    totalSwapKB_ = swapTotal;
    usedSwapKB_  = swapTotal - swapFree;
}

// ===========================================================================
// readCpuStat(): parse the aggregate CPU line of /proc/stat
// ---------------------------------------------------------------------------
// First line of /proc/stat:
//     cpu  user nice system idle iowait irq softirq steal guest guest_nice
// These are cumulative CPU "jiffies" (clock ticks) since boot.
//
// CPU usage is a RATE, so we take two samples (previous + now) and compare:
//     busy% = 100 * (1 - deltaIdle / deltaTotal)
// ===========================================================================
void SystemMonitor::readCpuStat() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return;

    // /proc/stat has an aggregate "cpu" line first, then one "cpu0", "cpu1"...
    // line per core, all in the same format. We read the file line by line and
    // apply the same two-sample delta formula to each.
    std::string line;
    int coreIndex = 0;                 // -1 would be aggregate; we track cores 0..N

    // Make sure our per-core history vectors are the right size.
    if ((int)prevCoreTotal_.size() < numCores_) {
        prevCoreTotal_.assign(numCores_, 0);
        prevCoreIdle_.assign(numCores_, 0);
        corePercents_.assign(numCores_, 0.0);
    }

    while (std::getline(file, line)) {
        // Only care about lines starting with "cpu".
        if (line.rfind("cpu", 0) != 0) break;   // once past cpu lines, stop

        std::istringstream ss(line);
        std::string label;
        ss >> label;                             // "cpu" or "cpu0", "cpu1"...

        long total = 0, idle = 0, val;
        int index = 0;
        while (ss >> val) {
            total += val;
            if (index == 3 || index == 4) idle += val;   // idle + iowait
            ++index;
        }

        if (label == "cpu") {
            // Aggregate line -> overall CPU%.
            long deltaTotal = total - prevTotalJiffies_;
            long deltaIdle  = idle  - prevIdleJiffies_;
            if (deltaTotal > 0)
                cpuPercent_ = 100.0 * (deltaTotal - deltaIdle) / deltaTotal;
            totalJiffiesDelta_ = deltaTotal;     // per-process CPU% reuses this
            prevTotalJiffies_ = total;
            prevIdleJiffies_  = idle;
        } else if (coreIndex < numCores_) {
            // Per-core line -> that core's CPU%.
            long deltaTotal = total - prevCoreTotal_[coreIndex];
            long deltaIdle  = idle  - prevCoreIdle_[coreIndex];
            if (deltaTotal > 0)
                corePercents_[coreIndex] =
                    100.0 * (deltaTotal - deltaIdle) / deltaTotal;
            prevCoreTotal_[coreIndex] = total;
            prevCoreIdle_[coreIndex]  = idle;
            ++coreIndex;
        }
    }
}

// ===========================================================================
// readUptime(): how long the system has been running (Phase 1).
// ---------------------------------------------------------------------------
// /proc/uptime has two numbers: seconds since boot, and seconds all cores have
// spent idle (summed). We only want the first.
// ===========================================================================
void SystemMonitor::readUptime() {
    std::ifstream file("/proc/uptime");
    if (!file.is_open()) return;
    double up = 0.0;
    if (file >> up) uptimeSec_ = up;
}

// ===========================================================================
// readLoadAvg(): system load average (Phase 1).
// ---------------------------------------------------------------------------
// /proc/loadavg looks like:  0.52 0.58 0.59 1/1234 5678
// The first three numbers are the 1-, 5- and 15-minute load averages — the
// average number of processes that were runnable or in uninterruptible sleep.
// ===========================================================================
void SystemMonitor::readLoadAvg() {
    std::ifstream file("/proc/loadavg");
    if (!file.is_open()) return;
    file >> load1_ >> load5_ >> load15_;
}

// Helper: is this /proc entry name a PID? (all-digit directory names)
static bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// ===========================================================================
// readProcesses(): walk /proc, read each /proc/<pid>/{comm,stat,status}
// ---------------------------------------------------------------------------
// For every numeric directory in /proc:
//   /proc/<pid>/comm   -> clean process name
//   /proc/<pid>/stat   -> state, utime, stime, nice (space-separated fields)
//   /proc/<pid>/status -> VmRSS (resident memory in KB)
//
// Per-process CPU% (top-style, 100% == one core):
//   cpu% = 100 * numCores * (procTimeDelta / totalJiffiesDelta)
// ===========================================================================
void SystemMonitor::readProcesses() {
    processes_.clear();
    taskCounts_ = TaskCounts{};             // reset the per-state tally each refresh

    DIR* dir = opendir("/proc");
    if (!dir) return;

    std::map<int, long> currentProcTime;   // rebuilt each refresh

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (!isNumber(name)) continue;      // skip non-PID entries
        int pid = std::stoi(name);

        Process p;
        p.pid = pid;

        // --- name: /proc/<pid>/comm (single clean line) ---
        {
            std::ifstream comm("/proc/" + name + "/comm");
            std::getline(comm, p.name);
        }

        // --- full command line: /proc/<pid>/cmdline ---
        // The arguments (argv) are stored NUL-separated with a trailing NUL, so
        // we read the whole thing and turn '\0' into spaces. It's empty for
        // kernel threads, in which case we fall back to the bracketed comm name.
        {
            std::ifstream cmd("/proc/" + name + "/cmdline", std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(cmd)),
                             std::istreambuf_iterator<char>());
            for (char& c : raw) if (c == '\0') c = ' ';
            while (!raw.empty() && raw.back() == ' ') raw.pop_back();
            p.cmdline = raw.empty() ? ("[" + p.name + "]") : raw;
        }

        // --- stat: /proc/<pid>/stat ---
        // Tricky part: field 2 (comm) is wrapped in parentheses and may itself
        // contain spaces or ')'. The safe trick is to split on the LAST ')':
        // everything after it is space-separated numeric-ish fields starting
        // with the state character (field 3).
        {
            std::ifstream stat("/proc/" + name + "/stat");
            std::string content;
            std::getline(stat, content);
            std::size_t close = content.rfind(')');
            if (close != std::string::npos && close + 2 < content.size()) {
                std::istringstream rest(content.substr(close + 2));
                std::vector<std::string> f;
                std::string tok;
                while (rest >> tok) f.push_back(tok);
                // After the ')', index 0 == field 3. So fieldN -> index N-3.
                //   state=field3 -> f[0]
                //   utime=field14 -> f[11],  stime=field15 -> f[12]
                //   nice =field19 -> f[16]
                if (f.size() > 0)  p.state = f[0].empty() ? '?' : f[0][0];
                if (f.size() > 12) { p.utime = std::stol(f[11]);
                                     p.stime = std::stol(f[12]); }
                if (f.size() > 16) p.nice  = std::stol(f[16]);
            }
        }

        // --- memory + threads: from /proc/<pid>/status ---
        // "VmRSS:" gives resident memory; "Threads:" gives this process's thread
        // count. We read both in one pass over the file.
        {
            std::ifstream status("/proc/" + name + "/status");
            std::string line;
            bool haveRss = false, haveThreads = false;
            while (std::getline(status, line)) {
                if (!haveRss && line.rfind("VmRSS:", 0) == 0) {
                    std::istringstream ss(line);
                    std::string k, unit; long kb;
                    ss >> k >> kb >> unit;
                    p.memKB = kb;
                    haveRss = true;
                } else if (!haveThreads && line.rfind("Threads:", 0) == 0) {
                    std::istringstream ss(line);
                    std::string k; long n;
                    ss >> k >> n;
                    p.threads = n;
                    haveThreads = true;
                }
                if (haveRss && haveThreads) break;
            }
        }

        // --- per-process CPU% via delta between refreshes ---
        long nowTime = p.totalTime();
        currentProcTime[pid] = nowTime;
        auto it = prevProcTime_.find(pid);
        if (it != prevProcTime_.end() && totalJiffiesDelta_ > 0) {
            long procDelta = nowTime - it->second;
            p.cpuPercent = 100.0 * numCores_ *
                           static_cast<double>(procDelta) / totalJiffiesDelta_;
        }

        // --- tally this process into the task-state summary ---
        taskCounts_.total++;
        taskCounts_.threads += static_cast<int>(p.threads);
        switch (p.state) {
            case 'R': taskCounts_.running++;  break;
            case 'Z': taskCounts_.zombie++;   break;
            case 'T':
            case 't': taskCounts_.stopped++;  break;
            default:  taskCounts_.sleeping++; break;   // S, D, I, ...
        }

        processes_.push_back(std::move(p));
    }
    closedir(dir);

    prevProcTime_ = std::move(currentProcTime);   // remember for next refresh

    // Sort by whichever column the user chose (Phase 1 sort toggle). CPU and
    // memory are "highest first"; PID is ascending, the natural spawn order.
    std::sort(processes_.begin(), processes_.end(),
              [this](const Process& a, const Process& b) {
                  switch (sortMode_) {
                      case SortMode::MEM: return a.memKB > b.memKB;
                      case SortMode::PID: return a.pid < b.pid;
                      case SortMode::CPU:
                      default:            return a.cpuPercent > b.cpuPercent;
                  }
              });
}

// ===========================================================================
// killProcess(): send a signal to a PID.
// ---------------------------------------------------------------------------
// This is a direct use of the OS signal mechanism. kill(pid, sig) is a
// system call. SIGTERM (15) politely asks a process to exit; SIGKILL (9)
// forces it. Returns true if the syscall succeeded.
// ===========================================================================
bool SystemMonitor::killProcess(int pid, int signal) {
    return kill(pid, signal) == 0;
}

// ===========================================================================
// reniceProcess(): change a process's scheduling priority (Phase 2).
// ---------------------------------------------------------------------------
// setpriority(PRIO_PROCESS, pid, nice) sets the "nice" value, the scheduler's
// hint about how greedy a process may be for CPU time: -20 = highest priority,
// 19 = lowest. Raising priority (a more negative value) needs privileges.
// ===========================================================================
bool SystemMonitor::reniceProcess(int pid, int niceValue) {
    if (niceValue < -20) niceValue = -20;
    if (niceValue >  19) niceValue =  19;
    return setpriority(PRIO_PROCESS, pid, niceValue) == 0;
}

// ===========================================================================
// readThermal(): CPU temperature from sysfs (Phase 2).
// ---------------------------------------------------------------------------
// The kernel exposes thermal sensors under /sys/class/thermal/thermal_zone*/:
//   type -> a label like "x86_pkg_temp", "cpu-thermal", "acpitz"
//   temp -> temperature in milli-degrees Celsius (e.g. 47000 == 47.0 °C)
// A machine can have several zones. We prefer a CPU-package zone; failing that
// we report the hottest zone as a reasonable "system temperature".
// ===========================================================================
void SystemMonitor::readThermal() {
    hasCpuTemp_ = false;
    double best = -1.0;                 // hottest zone seen (fallback)
    std::string bestLabel;

    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string zone = entry->d_name;
        if (zone.rfind("thermal_zone", 0) != 0) continue;   // only zones

        std::string base = "/sys/class/thermal/" + zone;

        long milli = 0;
        {
            std::ifstream tf(base + "/temp");
            if (!(tf >> milli)) continue;                   // unreadable zone
        }
        double celsius = milli / 1000.0;

        std::string type;
        {
            std::ifstream tyf(base + "/type");
            std::getline(tyf, type);
        }

        // Prefer a zone that clearly names the CPU. Take it immediately.
        if (type.find("x86_pkg") != std::string::npos ||
            type.find("cpu")     != std::string::npos ||
            type.find("coretemp")!= std::string::npos ||
            type.find("k10temp") != std::string::npos) {
            cpuTempC_     = celsius;
            cpuTempLabel_ = type;
            hasCpuTemp_   = true;
            closedir(dir);
            return;
        }

        if (celsius > best) { best = celsius; bestLabel = type; }
    }
    closedir(dir);

    if (best >= 0.0) {                  // no CPU-specific zone; use the hottest
        cpuTempC_     = best;
        cpuTempLabel_ = bestLabel.empty() ? "thermal" : bestLabel;
        hasCpuTemp_   = true;
    }
}

// ===========================================================================
// readNetDev(): per-interface network throughput (Phase 2).
// ---------------------------------------------------------------------------
// /proc/net/dev has two header lines, then one line per interface:
//   iface: rxBytes rxPackets ... (8 rx fields) txBytes txPackets ... (8 tx)
// rxBytes/txBytes are cumulative since boot, so we diff them against the last
// sample and divide by elapsed seconds — the same rate method as CPU%.
// ===========================================================================
void SystemMonitor::readNetDev() {
    netInterfaces_.clear();
    netRxKBps_ = netTxKBps_ = 0.0;

    std::ifstream file("/proc/net/dev");
    if (!file.is_open()) return;

    std::string line;
    std::getline(file, line);           // header line 1
    std::getline(file, line);           // header line 2

    std::map<std::string, std::pair<unsigned long long, unsigned long long>> current;

    while (std::getline(file, line)) {
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string name = line.substr(0, colon);
        // Trim leading whitespace from the interface name.
        std::size_t s = name.find_first_not_of(" \t");
        if (s != std::string::npos) name = name.substr(s);

        std::istringstream ss(line.substr(colon + 1));
        std::vector<unsigned long long> v;
        unsigned long long x;
        while (ss >> x) v.push_back(x);
        if (v.size() < 16) continue;    // need at least 8 rx + 8 tx fields

        unsigned long long rx = v[0];   // rx bytes
        unsigned long long tx = v[8];   // tx bytes (9th field)
        current[name] = {rx, tx};

        NetInterface ni;
        ni.name = name;
        auto it = prevNet_.find(name);
        if (it != prevNet_.end() && dtSec_ > 0.0) {
            double drx = static_cast<double>(rx - it->second.first);
            double dtx = static_cast<double>(tx - it->second.second);
            if (drx < 0) drx = 0;       // guard counter resets
            if (dtx < 0) dtx = 0;
            ni.rxKBps = drx / dtSec_ / 1024.0;
            ni.txKBps = dtx / dtSec_ / 1024.0;
        }

        // Skip the loopback interface in the totals and the display list — it's
        // internal traffic, not real network I/O.
        if (name != "lo") {
            netRxKBps_ += ni.rxKBps;
            netTxKBps_ += ni.txKBps;
            netInterfaces_.push_back(ni);
        }
    }

    prevNet_ = std::move(current);

    // Show the busiest interfaces first.
    std::sort(netInterfaces_.begin(), netInterfaces_.end(),
              [](const NetInterface& a, const NetInterface& b) {
                  return (a.rxKBps + a.txKBps) > (b.rxKBps + b.txKBps);
              });
}

// ===========================================================================
// readDiskStats(): aggregate disk read/write throughput (Phase 2).
// ---------------------------------------------------------------------------
// /proc/diskstats has one line per block device:
//   major minor name reads rdMerged sectorsRead ... writes wrMerged sectorsWritten ...
// Sectors are 512 bytes. We sum sectors across WHOLE disks only (a device is a
// partition if another device name is a prefix of it, e.g. sda -> sda1,
// nvme0n1 -> nvme0n1p1), skipping loop/ram devices, so we don't double-count.
// Then we diff the totals against the previous sample -> KB/s.
// ===========================================================================
void SystemMonitor::readDiskStats() {
    diskReadKBps_ = diskWriteKBps_ = 0.0;

    std::ifstream file("/proc/diskstats");
    if (!file.is_open()) return;

    struct Dev { std::string name; unsigned long long rd, wr; };
    std::vector<Dev> devs;
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::string major, minor, name;
        unsigned long long reads, rdMerged, sectorsRead, msRead,
                           writes, wrMerged, sectorsWritten;
        if (!(ss >> major >> minor >> name
                 >> reads >> rdMerged >> sectorsRead >> msRead
                 >> writes >> wrMerged >> sectorsWritten))
            continue;
        if (name.rfind("loop", 0) == 0 || name.rfind("ram", 0) == 0) continue;
        devs.push_back({name, sectorsRead, sectorsWritten});
    }

    // Sum only whole disks: skip any device whose name has another device's
    // name as a proper prefix (that makes it a partition of that disk).
    unsigned long long totalRead = 0, totalWrite = 0;
    for (const auto& d : devs) {
        bool isPartition = false;
        for (const auto& other : devs) {
            if (other.name.size() < d.name.size() &&
                d.name.compare(0, other.name.size(), other.name) == 0) {
                isPartition = true;
                break;
            }
        }
        if (isPartition) continue;
        totalRead  += d.rd;
        totalWrite += d.wr;
    }

    if (haveDiskStats_ && dtSec_ > 0.0) {
        double drd = static_cast<double>(totalRead  - prevReadSectors_);
        double dwr = static_cast<double>(totalWrite - prevWriteSectors_);
        if (drd < 0) drd = 0;
        if (dwr < 0) dwr = 0;
        // sectors * 512 bytes / 1024 == sectors / 2  KB.
        diskReadKBps_  = (drd * 512.0 / 1024.0) / dtSec_;
        diskWriteKBps_ = (dwr * 512.0 / 1024.0) / dtSec_;
    }
    prevReadSectors_  = totalRead;
    prevWriteSectors_ = totalWrite;
    haveDiskStats_    = true;
}

// ===========================================================================
// readDiskUsage(): capacity of each real mounted filesystem (Phase 2).
// ---------------------------------------------------------------------------
// /proc/mounts lists every mount as "device mountpoint fstype options ...".
// We keep only mounts backed by a real block device (device starts with
// "/dev/"), which filters out pseudo filesystems (proc, sysfs, tmpfs, cgroup).
// statvfs() then reports block counts we turn into used/total KB.
// ===========================================================================
void SystemMonitor::readDiskUsage() {
    diskMounts_.clear();

    std::ifstream file("/proc/mounts");
    if (!file.is_open()) return;

    std::string device, mountPoint, fsType, rest;
    while (file >> device >> mountPoint >> fsType) {
        std::getline(file, rest);       // consume options + rest of the line

        if (device.rfind("/dev/", 0) != 0) continue;   // real devices only

        // Skip a mount point we already recorded (bind mounts show up twice).
        bool dup = false;
        for (const auto& m : diskMounts_)
            if (m.mountPoint == mountPoint) { dup = true; break; }
        if (dup) continue;

        struct statvfs vfs;
        if (statvfs(mountPoint.c_str(), &vfs) != 0) continue;

        // f_frsize is the fundamental block size in bytes.
        unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        unsigned long long used  = total - (unsigned long long)vfs.f_bfree * vfs.f_frsize;
        if (total == 0) continue;       // skip empty pseudo mounts

        DiskMount dm;
        dm.device     = device;
        dm.mountPoint = mountPoint;
        dm.fsType     = fsType;
        dm.totalKB    = static_cast<long>(total / 1024);
        dm.usedKB     = static_cast<long>(used  / 1024);
        dm.usedPct    = 100.0 * used / total;
        (void)avail;                    // available space is implied by total-used
        diskMounts_.push_back(dm);
    }
}

// ===========================================================================
// readGpu(): NVIDIA GPU stats via nvidia-smi (Phase 4).
// ---------------------------------------------------------------------------
// Unlike CPU/memory there is no universal /proc file for the GPU, so we ask the
// vendor tool. `nvidia-smi --query-gpu=...` prints exactly the fields we want as
// one CSV line, which popen() lets us read like a file. If the tool is missing
// (no NVIDIA GPU / driver), we flip gpuAvailable_ off and stop trying, so the
// program runs fine on any machine and simply shows no GPU section.
// ===========================================================================
void SystemMonitor::readGpu() {
    if (!gpuAvailable_) return;

    FILE* pipe = popen(
        "nvidia-smi --query-gpu=name,utilization.gpu,memory.used,"
        "memory.total,temperature.gpu --format=csv,noheader,nounits 2>/dev/null",
        "r");
    if (!pipe) { gpuAvailable_ = false; return; }

    char buf[512] = {0};
    std::string line;
    if (std::fgets(buf, sizeof(buf), pipe)) line = buf;
    int rc = pclose(pipe);

    // Non-zero exit or no output means nvidia-smi isn't usable here.
    if (rc != 0 || line.empty()) { gpuAvailable_ = false; hasGpu_ = false; return; }

    // Split the CSV line and trim whitespace from each field.
    std::vector<std::string> f;
    std::istringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        std::size_t a = tok.find_first_not_of(" \t\r\n");
        std::size_t b = tok.find_last_not_of(" \t\r\n");
        f.push_back(a == std::string::npos ? "" : tok.substr(a, b - a + 1));
    }
    if (f.size() < 5) { hasGpu_ = false; return; }

    // Some fields can read "[N/A]" on certain GPUs — guard the conversions.
    try {
        gpuName_       = f[0];
        gpuUtil_       = std::stod(f[1]);
        gpuMemUsedMB_  = std::stol(f[2]);
        gpuMemTotalMB_ = std::stol(f[3]);
        gpuTempC_      = std::stod(f[4]);
        hasGpu_        = true;
    } catch (...) {
        hasGpu_ = false;                // leave gpuAvailable_ true; try again next tick
    }
}
