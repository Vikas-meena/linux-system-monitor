# Roadmap — Linux System Monitor

A living plan for evolving this project from a basic monitor into a full
`htop`-class tool. Features are grouped into phases by difficulty. Each item
lists **what** it does, the **OS concept** it demonstrates, **where** to build
it, and a rough **effort**. Check items off as you finish them.

> Legend — Effort: 🟢 easy (~30 min) · 🟡 medium (1–2 hrs) · 🔴 hard (a day+)

---

## ✅ Phase 0 — Done (current state)

- [x] Read CPU usage from `/proc/stat` (two-sample delta method)
- [x] Read RAM / Swap from `/proc/meminfo`
- [x] Per-process info from `/proc/<pid>/{comm,stat,status}`
- [x] Per-core CPU bars
- [x] Color-coded bar graphs (CPU / RAM / Swap)
- [x] Scrolling CPU-history sparkline
- [x] Process table sorted by CPU
- [x] Kill a process via signals (`k` key)
- [x] Flicker-free, in-place redraw (ANSI cursor control)
- [x] Makefile + README + GitHub repo

---

## 🟢 Phase 1 — Quick wins (high value, low effort)

- [ ] **Sort toggle** — keys `c` / `m` / `p` to sort by CPU / memory / PID.
  - *OS concept:* scheduling priority, resource accounting.
  - *Where:* change the `std::sort` comparator in `SystemMonitor::readProcesses`
    (or store a "sort mode" and sort in `draw`). Handle the keys in `main`.
  - *Effort:* 🟢

- [ ] **Uptime & load average** — show system uptime and 1/5/15-min load.
  - *OS concept:* load average = avg number of runnable/uninterruptible tasks.
  - *Where:* read `/proc/uptime` (seconds) and `/proc/loadavg` (3 numbers).
    New methods in `SystemMonitor`, print them in `draw`.
  - *Effort:* 🟢

- [ ] **Process state summary** — e.g. `Tasks: 378 (2 running, 1 zombie, ...)`.
  - *OS concept:* process life-cycle / states (R, S, D, Z, T).
  - *Where:* while walking processes, count each `state`; expose counts; print.
  - *Effort:* 🟢

- [ ] **Total thread count** — sum of threads across processes.
  - *OS concept:* threads vs processes.
  - *Where:* count entries in `/proc/<pid>/task/`, or read `Threads:` from
    `/proc/<pid>/status`.
  - *Effort:* 🟢

- [ ] **Adjustable refresh rate** — `+` / `-` keys change the interval.
  - *OS concept:* sampling / polling trade-offs.
  - *Where:* make the `select()` timeout in `pollKey` a variable.
  - *Effort:* 🟢

---

## ✅ Phase 2 — Systems features (strong "low-level" signal) — Done

- [x] **Search / filter processes** — press `/`, type text, show only matches.
  - *OS concept:* n/a (UX), but shows solid string handling.
  - *Where:* filter string in `main`; case-insensitive match on name **and**
    cmdline in `draw` (`containsCI`). ESC-empty clears it.
  - *Effort:* 🟡

- [x] **CPU temperature** — read thermal sensors. *(Very Qualcomm-relevant.)*
  - *OS concept:* thermal management, `/sys` (sysfs) interface.
  - *Where:* `SystemMonitor::readThermal` scans
    `/sys/class/thermal/thermal_zone*/{type,temp}`, prefers a CPU zone, else
    reports the hottest. Shown on the CPU line, colored by the load scale.
  - *Effort:* 🟡

- [x] **Network throughput** — live upload / download KB/s per interface.
  - *OS concept:* same two-sample rate method; kernel network counters.
  - *Where:* `readNetDev` diffs rx/tx bytes from `/proc/net/dev` over the
    measured elapsed time; totals + top 3 interfaces (loopback excluded).
  - *Effort:* 🟡

- [x] **Disk usage** — show mounted filesystems with used/total bars.
  - *OS concept:* filesystems, mount points.
  - *Where:* `readDiskUsage` parses `/proc/mounts` (real `/dev/` devices only)
    and calls `statvfs()` per mount.
  - *Effort:* 🟡

- [x] **Disk I/O rate** — read/write throughput per device.
  - *OS concept:* block I/O.
  - *Where:* `readDiskStats` diffs sectors from `/proc/diskstats`, summing
    whole disks only (prefix test skips partitions/loop/ram to avoid double
    counting).
  - *Effort:* 🟡

- [x] **Per-process command line** — full command instead of short name.
  - *OS concept:* process arguments / `argv`.
  - *Where:* `readProcesses` slurps `/proc/<pid>/cmdline` (NUL→space); press
    `a` to toggle the NAME column between comm and full command line.
  - *Effort:* 🟢

- [x] **Renice a process** — change a process's priority interactively.
  - *OS concept:* scheduling priority, `nice`/`setpriority()`.
  - *Where:* `r` key prompts for PID + nice value →
    `SystemMonitor::reniceProcess` calls `setpriority(PRIO_PROCESS, ...)`.
  - *Effort:* 🟡

---

## 🔴 Phase 3 — Polish & engineering maturity

- [ ] **ncurses UI** — real windows, scrollable process list, mouse support.
  - *Where:* replace the ANSI `draw` with ncurses windows. Needs `libncurses-dev`.
  - *Effort:* 🔴

- [ ] **Command-line arguments** — e.g. `./monitor --sort mem --refresh 2`.
  - *Where:* parse `argc`/`argv` in `main` (or use `getopt`).
  - *Effort:* 🟡

- [ ] **Config file** — remember preferences between runs (`~/.monitorrc`).
  - *Where:* read/write a small key=value file at startup/exit.
  - *Effort:* 🟡

- [ ] **Unit tests** — feed sample `/proc` text to the parsers, assert results.
  - *Why:* testing discipline stands out in a student project.
  - *Where:* refactor parsing to take a string/stream; add a `tests/` target.
  - *Effort:* 🟡

- [ ] **Logging / CSV export** — dump metrics over time to a file for graphing.
  - *Where:* append a row per refresh to a `.csv`.
  - *Effort:* 🟢

- [ ] **Scrollable process list** — page through all processes, not just top N.
  - *OS concept:* n/a (UX).
  - *Where:* track a scroll offset; arrow keys move it.
  - *Effort:* 🟡

---

## 🌟 Phase 4 — Stretch / advanced (portfolio standouts)

- [ ] **Loadable Kernel Module (LKM)** exposing a custom `/proc` entry, then
      read it here.
  - *OS concept:* kernel space vs user space, kernel modules, `/proc` from the
    kernel side. *This is the single most Qualcomm-relevant addition.*
  - *Effort:* 🔴 (learn kernel module basics first)

- [ ] **eBPF-based metrics** — trace syscalls or scheduling events.
  - *OS concept:* modern kernel observability.
  - *Effort:* 🔴

- [ ] **Run on an ARM board** (e.g. Raspberry Pi) and note any differences.
  - *Why:* Qualcomm is ARM-heavy; cross-platform awareness is a plus.
  - *Effort:* 🟡 (mostly setup)

- [x] **GPU usage** (if NVIDIA: parse `nvidia-smi`; else vendor sysfs).
  - *Where:* `SystemMonitor::readGpu` runs `nvidia-smi --query-gpu=...` via
    `popen` and parses the CSV (load %, memory, temperature). Degrades
    gracefully (no GPU section) when nvidia-smi is absent. Shown on a "GPU"
    line in simple view; model + memory bar in detailed view.
  - *Effort:* 🟡

---

## Suggested order

Phase 2 is now complete. The remaining quick wins in **Phase 1** (sort toggle,
uptime/load average, task-state summary, thread count, adjustable refresh rate)
are all 🟢 and still worth doing for polish. After that, move to **Phase 3**
(ncurses UI, CLI args, config file, unit tests) to turn this into a serious
portfolio piece.

## How to work on a feature

1. Pick an unchecked item and read its *Where* hint.
2. Add the data-reading logic in `SystemMonitor` (a new `readX()` + getters).
3. Display it in `draw()` in `main.cpp`.
4. Build and test: `make && ./monitor`.
5. Commit: `git add -A && git commit -m "Add <feature>" && git push`.
6. Check the box in this file so you track progress.

## Useful `/proc` and `/sys` references

| Source | Gives you |
|--------|-----------|
| `/proc/stat` | CPU jiffies (overall + per core), context switches, boot time |
| `/proc/meminfo` | memory & swap totals |
| `/proc/uptime` | system uptime |
| `/proc/loadavg` | load averages, running/total tasks |
| `/proc/<pid>/stat` | state, CPU time, priority, nice |
| `/proc/<pid>/status` | memory (VmRSS), threads, UID |
| `/proc/<pid>/cmdline` | full command line |
| `/proc/<pid>/task/` | threads of the process |
| `/proc/net/dev` | network rx/tx counters |
| `/proc/diskstats` | disk I/O counters |
| `/proc/mounts` | mounted filesystems |
| `/sys/class/thermal/thermal_zone*/temp` | temperatures |

`man proc` documents every field in detail — the best reference while building.
