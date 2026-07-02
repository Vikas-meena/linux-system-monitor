# Linux System Monitor (C++)

A terminal-based system monitor written in C++ that displays live CPU usage,
memory usage, and a sorted list of running processes — similar to `top`/`htop`.
All data is read directly from the Linux **`/proc`** virtual filesystem.

### Graphical display (in the terminal)
The UI is drawn with ANSI escape codes (colors + cursor control), so it is
graphical without any GUI framework or extra installs:
- **Color-coded bar graphs** for CPU, RAM and Swap (green < 50%, yellow < 80%,
  red otherwise).
- A **scrolling CPU-history graph** (sparkline) using Unicode block characters
  `▁▂▃▄▅▆▇█`, showing the last ~60 seconds of load.
- **Per-core CPU bars**, one per logical CPU.
- A live, CPU-sorted **process table**.

```
CPU  [||||||||......................]  25.0%
RAM  [||||||||||||||||||||||........]  5.6GB / 7.5GB
CPU history (last 60s):
  ▁▂▃▅▇█▇▅▃▂▁▂▄▆█
Per-core:
  Core0  [|...................]  6.1%
  ...
```

This project is a practical demonstration of core Operating System concepts:
processes, process states, scheduling, memory management, and signals.

## Build & Run

```bash
make        # compiles -> ./monitor
./monitor   # run it
```

Controls:
- `q` — quit
- `k` — kill a process (prompts for PID and signal)

## How it works (the OS concepts)

### The `/proc` filesystem
`/proc` is a **virtual filesystem**: the files don't exist on disk. When you
read them, the kernel generates the contents on the fly from its internal data
structures. It is the standard, portable way for user programs to inspect the
kernel's view of the system — no special privileges needed for reading.

### Processes and the PCB
The kernel tracks every process with a **Process Control Block (PCB)**. Linux
exposes each process as a directory `/proc/<pid>/`. Files we use:

| File | What we read | OS concept |
|------|--------------|------------|
| `/proc/<pid>/comm`   | process name | process identity |
| `/proc/<pid>/stat`   | state, utime, stime, nice | process state, CPU time, scheduling |
| `/proc/<pid>/status` | VmRSS (resident memory) | memory management |

### Process states
The `state` field is a single character reflecting the process life-cycle:
- `R` — Running / runnable
- `S` — Interruptible sleep (waiting for an event)
- `D` — Uninterruptible sleep (usually waiting on I/O)
- `Z` — Zombie (finished, but parent hasn't `wait()`-ed for it)
- `T` — Stopped

### CPU usage is a *rate* (two-sample method)
`/proc/stat`'s first line holds cumulative CPU **jiffies** (clock ticks) since
boot: `user nice system idle iowait ...`. A single reading tells you nothing
about *current* load. So we take **two samples** one second apart and compute:

```
busy% = 100 * (deltaTotal - deltaIdle) / deltaTotal
```

Per-process CPU% uses the same idea with each process's `utime + stime`,
scaled by the number of cores so 100% means "one full core" (like `top`):

```
cpu% = 100 * numCores * (procTimeDelta / totalJiffiesDelta)
```

### Memory management
`/proc/meminfo` gives `MemTotal` and `MemAvailable`. We report
`used = MemTotal - MemAvailable`. `MemAvailable` is the kernel's estimate of
memory usable without swapping — more meaningful than `MemFree`, because Linux
uses otherwise-idle RAM as reclaimable disk cache. Per-process memory uses
**VmRSS** (Resident Set Size = physical RAM currently occupied).

### Signals (the `k` feature)
Killing a process uses the `kill(pid, signal)` **system call**:
- `SIGTERM (15)` — polite request to terminate (process can clean up)
- `SIGKILL (9)`  — forceful, cannot be caught or ignored

### Terminal handling
The live UI uses **ANSI escape codes** to clear the screen and color text, and
switches the terminal into **raw mode** (via `termios`) so keypresses arrive
instantly without waiting for Enter. `select()` on stdin gives us a 1-second
refresh that still responds immediately to a keypress.

## C++ design
- `Process` — a plain data class: one process's snapshot.
- `SystemMonitor` — reads `/proc`, computes CPU/memory/process stats, keeps the
  previous sample as state (needed for rate calculations).
- `main.cpp` — the terminal UI / input loop.

Concepts used: classes/encapsulation, `std::vector`, `std::map`, RAII file
streams (`std::ifstream`), lambdas (`std::sort`), move semantics (`std::move`).

## Files
```
Process.h          # Process data class
SystemMonitor.h    # Monitor interface
SystemMonitor.cpp  # /proc parsing + calculations (core logic)
main.cpp           # display loop + keyboard input
Makefile           # build
```

## Roadmap
Planned features and improvements — grouped by difficulty, each with the OS
concept it demonstrates and where to implement it — are tracked in
[ROADMAP.md](ROADMAP.md). Highlights: sort toggle, uptime/load average, process
state summary, CPU temperature, network throughput, disk usage, and (stretch) a
loadable kernel module exposing a custom `/proc` entry.
