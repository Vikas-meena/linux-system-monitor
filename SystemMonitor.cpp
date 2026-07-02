#include "SystemMonitor.h"

#include <fstream>     // std::ifstream  -> read /proc files like normal files
#include <sstream>     // std::istringstream -> tokenize a line
#include <string>
#include <vector>
#include <cctype>      // isdigit
#include <dirent.h>    // opendir/readdir -> list /proc directory entries
#include <unistd.h>    // sysconf, getpagesize
#include <csignal>     // kill(), SIGTERM, SIGKILL
#include <algorithm>   // std::sort

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
    readMemInfo();
    readCpuStat();
    readProcesses();
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

        // --- memory: VmRSS from /proc/<pid>/status ---
        {
            std::ifstream status("/proc/" + name + "/status");
            std::string line;
            while (std::getline(status, line)) {
                if (line.rfind("VmRSS:", 0) == 0) {   // line starts with VmRSS:
                    std::istringstream ss(line);
                    std::string k, unit; long kb;
                    ss >> k >> kb >> unit;
                    p.memKB = kb;
                    break;
                }
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

        processes_.push_back(std::move(p));
    }
    closedir(dir);

    prevProcTime_ = std::move(currentProcTime);   // remember for next refresh

    // Sort by CPU usage (highest first), like `top`.
    std::sort(processes_.begin(), processes_.end(),
              [](const Process& a, const Process& b) {
                  return a.cpuPercent > b.cpuPercent;
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
