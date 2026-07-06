#ifndef PROCESS_H
#define PROCESS_H

#include <string>

// ---------------------------------------------------------------------------
// Process
// ---------------------------------------------------------------------------
// Represents ONE running process on the system.
//
// In OS terms, the kernel keeps a "Process Control Block" (PCB) for every
// process. On Linux the kernel exposes that information to user programs
// through the virtual /proc filesystem: every process gets a directory
// /proc/<pid>/ with files describing it. This class is basically a small,
// user-space snapshot of the fields we care about from that PCB.
// ---------------------------------------------------------------------------
class Process {
public:
    int pid = 0;              // Process ID
    std::string name;         // Executable name (from /proc/<pid>/comm)
    std::string cmdline;      // Full command line / argv (from /proc/<pid>/cmdline)
    char state = '?';         // R=Running, S=Sleeping, D=Uninterruptible,
                              // Z=Zombie, T=Stopped   (process life-cycle)
    long nice = 0;            // Nice value (scheduling priority hint)
    long threads = 1;         // Number of threads (from /proc/<pid>/status)

    // Raw CPU time this process has used, measured in "clock ticks" (jiffies).
    // utime = time spent in user mode, stime = time spent in kernel mode.
    // We store the previous sample so we can compute a delta between refreshes.
    long utime = 0;
    long stime = 0;
    long prevTotalTime = 0;   // utime+stime from the PREVIOUS refresh

    long memKB = 0;           // Resident memory in KB (VmRSS from status)
    double cpuPercent = 0.0;  // CPU usage %, computed between two samples

    long totalTime() const { return utime + stime; }
};

#endif // PROCESS_H
