// ===========================================================================
// Linux System Monitor  (C++)
// ---------------------------------------------------------------------------
// A terminal program that shows live CPU, memory and per-process usage by
// reading the /proc filesystem — a hands-on tour of OS process/memory concepts.
//
// Controls:
//     q          quit
//     k          kill a process (prompts for PID + signal)
//     r          renice a process (change scheduling priority)
//     / a d      find / toggle command line / toggle detailed view
//     c m p      sort the process list by CPU / memory / PID
//     + -        slow down / speed up the refresh interval
//
// Display refreshes about once per second using ANSI escape codes (no external
// libraries needed).
// ===========================================================================
#include "SystemMonitor.h"

#include <iostream>
#include <iomanip>       // std::setw for aligned columns
#include <sstream>       // std::ostringstream for formatting
#include <string>
#include <vector>
#include <deque>         // rolling CPU history buffer
#include <algorithm>     // std::search for case-insensitive process filter
#include <cctype>        // std::tolower
#include <csignal>       // SIGTERM, SIGKILL
#include <termios.h>     // terminal mode control (raw / cooked)
#include <unistd.h>      // read, STDIN_FILENO
#include <sys/select.h>  // select() -> check for a keypress without blocking

// --- ANSI escape codes: control the terminal cursor/colors as plain text ---
static const char* CLEAR_SCREEN = "\033[2J\033[H"; // clear + cursor to top-left
static const char* HOME         = "\033[H";        // move cursor to top-left ONLY
static const char* CLR_EOL      = "\033[K";        // erase from cursor to line end
static const char* CLR_BELOW    = "\033[J";        // erase everything below cursor
static const char* HIDE_CURSOR  = "\033[?25l";     // hide the blinking cursor
static const char* SHOW_CURSOR  = "\033[?25h";     // show it again
static const char* BOLD         = "\033[1m";
static const char* RESET        = "\033[0m";
static const char* CYAN         = "\033[36m";
static const char* GREEN        = "\033[32m";
static const char* YELLOW       = "\033[33m";
static const char* RED          = "\033[31m";

// We end every printed line with this instead of a plain "\n". CLR_EOL wipes
// any leftover characters from the PREVIOUS frame that were longer than the
// current line — this is what lets us overwrite in place without clearing the
// whole screen (which causes the flicker / scrolling you saw).
static const char* EOL = "\033[K\n";

// Pick a color by load level: green < 50%, yellow < 80%, red otherwise.
// Returns an ANSI color code so bars visually signal how busy something is.
static const char* loadColor(double pct) {
    if (pct < 50.0) return GREEN;
    if (pct < 80.0) return YELLOW;
    return RED;
}

// Build a horizontal bar like  [||||||||........]  for a 0..100 percentage.
// Uses '|' for the filled part and '.' for the empty part; colored by load.
static std::string makeBar(double pct, int width) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int filled = static_cast<int>(pct / 100.0 * width + 0.5);
    std::ostringstream os;
    os << loadColor(pct) << "[";
    for (int i = 0; i < width; ++i) os << (i < filled ? '|' : '.');
    os << "]" << RESET;
    return os.str();
}

// Turn a value 0..100 into one of eight Unicode "block" characters of
// increasing height. Used to draw the scrolling CPU history as a sparkline.
static std::string spark(double pct) {
    static const char* blocks[8] = {
        "▁","▂","▃","▄","▅","▆","▇","█"
    };
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    int idx = static_cast<int>(pct / 100.0 * 7 + 0.5);   // 0..7
    return blocks[idx];
}

// ---------------------------------------------------------------------------
// Terminal raw mode.
// By default the terminal is in "cooked" mode: it buffers a whole line and
// only hands it to us after Enter, and it echoes typed characters. For a live
// UI we want each keypress immediately and no echo. termios lets us toggle
// that. We always restore the original settings before exiting.
// ---------------------------------------------------------------------------
static struct termios g_originalTermios;

static void enableRawMode() {
    tcgetattr(STDIN_FILENO, &g_originalTermios);      // save current settings
    struct termios raw = g_originalTermios;
    raw.c_lflag &= ~(ICANON | ECHO);                  // no line-buffer, no echo
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_originalTermios);
}

// Return a pressed key if one is waiting, else 0. Uses select() with a timeout,
// which doubles as our refresh interval: it sleeps up to `intervalSec` seconds
// but wakes instantly if the user presses a key. The interval is adjustable at
// runtime with the +/- keys (Phase 1).
static char pollKey(double intervalSec) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval timeout;
    timeout.tv_sec  = static_cast<long>(intervalSec);
    timeout.tv_usec = static_cast<long>((intervalSec - timeout.tv_sec) * 1e6);
    if (select(STDIN_FILENO + 1, &set, nullptr, nullptr, &timeout) > 0) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) return c;
    }
    return 0;
}

// Turn a KB amount into a human-friendly string (KB / MB / GB).
static std::string humanKB(long kb) {
    double v = static_cast<double>(kb);
    const char* unit = "KB";
    if (v >= 1024) { v /= 1024; unit = "MB"; }
    if (v >= 1024) { v /= 1024; unit = "GB"; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v << unit;
    return os.str();
}

// Turn a KB/s rate into a human-friendly string (KB/s or MB/s).
static std::string humanRate(double kbps) {
    double v = kbps;
    const char* unit = "KB/s";
    if (v >= 1024) { v /= 1024; unit = "MB/s"; }
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << v << unit;
    return os.str();
}

// Turn a number of seconds into a compact "3d 04:12:07" / "12:34" uptime string.
static std::string formatUptime(double seconds) {
    long s = static_cast<long>(seconds);
    long days  = s / 86400; s %= 86400;
    long hours = s / 3600;  s %= 3600;
    long mins  = s / 60;    long secs = s % 60;
    std::ostringstream os;
    if (days > 0) os << days << "d ";
    os << std::setfill('0');
    if (days > 0 || hours > 0)
        os << std::setw(2) << hours << ":";
    os << std::setw(2) << mins << ":" << std::setw(2) << secs;
    return os.str();
}

// Human-readable name for the current sort column, shown in the footer.
static const char* sortName(SortMode m) {
    switch (m) {
        case SortMode::MEM: return "memory";
        case SortMode::PID: return "PID";
        case SortMode::CPU:
        default:            return "CPU";
    }
}

// Translate a kernel process-state letter into a plain-English word, so the
// detailed view doesn't make people memorize R/S/D/Z/T.
static std::string stateWord(char s) {
    switch (s) {
        case 'R': return "Running";
        case 'S': return "Sleeping";
        case 'D': return "Waiting";   // uninterruptible (usually disk I/O)
        case 'Z': return "Zombie";
        case 'T': return "Stopped";
        case 'I': return "Idle";
        default:  return "-";
    }
}

// Case-insensitive "does haystack contain needle?" — used by process search.
static bool containsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) { return std::tolower((unsigned char)a) ==
                                    std::tolower((unsigned char)b); });
    return it != haystack.end();
}

// Draw one refresh IN PLACE.
// Instead of clearing the whole screen (which flickers and scrolls), we move
// the cursor to the top-left with HOME and overwrite the old text line by line.
// Every line ends with EOL ("\033[K\n") to erase leftovers from the last frame,
// and we finish with CLR_BELOW to wipe any lines the new frame didn't cover.
// `history` holds recent overall-CPU% values so we can draw a trend graph.
// The screen has two modes:
//   SIMPLE (default)  — a clean, plain-language overview for everyday users.
//   DETAILED ('d')    — adds swap, CPU history, per-core bars, per-interface
//                       network, disk I/O and the process state/priority columns
//                       for people who want the technical picture.
static void draw(const SystemMonitor& mon, const std::deque<double>& history,
                 const std::string& filter, bool showCmdline, bool detailed,
                 double intervalSec) {
    std::ostringstream out;                 // build the whole frame, print once
    out << HOME;
    out << BOLD << CYAN << "=== System Monitor ===" << RESET
        << (detailed ? "  (detailed view)" : "") << EOL;

    // --- Uptime + load average (Phase 1) ---
    // Load average = avg number of runnable/uninterruptible tasks over 1/5/15
    // min. As a rule of thumb, a value near the core count means fully loaded.
    out << BOLD << "Up        " << RESET << formatUptime(mon.getUptimeSec())
        << "    " << BOLD << "Load " << RESET
        << std::fixed << std::setprecision(2)
        << mon.getLoad1()  << " " << mon.getLoad5() << " " << mon.getLoad15()
        << "  (1/5/15 min)" << EOL << EOL;

    // --- CPU: a bar + a whole-number percent (plus temperature if available) ---
    double cpu = mon.getCpuPercent();
    out << BOLD << "CPU       " << RESET << makeBar(cpu, 30)
        << "  " << std::setw(3) << (int)(cpu + 0.5) << "%";
    if (mon.hasCpuTemp()) {
        double t = mon.getCpuTempC();
        out << "   " << loadColor(t) << (int)(t + 0.5) << "°C" << RESET;
    }
    out << EOL;

    // --- Memory: bar + how much is in use out of the total ---
    double memPct = mon.getTotalMemKB()
                  ? 100.0 * mon.getUsedMemKB() / mon.getTotalMemKB() : 0.0;
    out << BOLD << "Memory    " << RESET << makeBar(memPct, 30)
        << "  " << humanKB(mon.getUsedMemKB()) << " used of "
        << humanKB(mon.getTotalMemKB()) << EOL;

    // --- GPU: load bar + temperature + video memory (only if a GPU is found) ---
    if (mon.hasGpu()) {
        double g = mon.getGpuUtil();
        out << BOLD << "GPU       " << RESET << makeBar(g, 30)
            << "  " << std::setw(3) << (int)(g + 0.5) << "%"
            << "   " << loadColor(mon.getGpuTempC())
            << (int)(mon.getGpuTempC() + 0.5) << "°C" << RESET
            << "   " << humanKB(mon.getGpuMemUsedMB() * 1024) << " of "
            << humanKB(mon.getGpuMemTotalMB() * 1024) << EOL;
    }

    // --- Internet speed: plain Download / Upload, no per-interface clutter ---
    out << BOLD << "Internet  " << RESET
        << GREEN << "Download " << humanRate(mon.getNetRxKBps()) << RESET << "   "
        << CYAN  << "Upload "   << humanRate(mon.getNetTxKBps()) << RESET << EOL;

    // --- Storage: one line per real disk, used out of total ---
    const auto& mounts = mon.getDiskMounts();
    for (const auto& m : mounts) {
        out << BOLD << "Storage   " << RESET
            << std::left << std::setw(8) << m.mountPoint.substr(0, 8)
            << makeBar(m.usedPct, 18) << "  "
            << (int)(m.usedPct + 0.5) << "% full ("
            << humanKB(m.usedKB) << " of " << humanKB(m.totalKB) << ")" << EOL;
    }

    // =====================================================================
    // DETAILED-ONLY sections: swap, CPU trend, per-core, extra network/disk.
    // =====================================================================
    if (detailed) {
        if (mon.hasGpu()) {
            double gmemPct = mon.getGpuMemTotalMB()
                ? 100.0 * mon.getGpuMemUsedMB() / mon.getGpuMemTotalMB() : 0.0;
            out << BOLD << "GPU model " << RESET << mon.getGpuName() << EOL;
            out << BOLD << "GPU memory" << RESET << makeBar(gmemPct, 30) << "  "
                << humanKB(mon.getGpuMemUsedMB() * 1024) << " of "
                << humanKB(mon.getGpuMemTotalMB() * 1024) << EOL;
        }
        if (mon.getTotalSwapKB() > 0) {
            double swapPct = 100.0 * mon.getUsedSwapKB() / mon.getTotalSwapKB();
            out << BOLD << "Swap      " << RESET << makeBar(swapPct, 30) << "  "
                << humanKB(mon.getUsedSwapKB()) << " of "
                << humanKB(mon.getTotalSwapKB()) << EOL;
        }

        out << EOL << BOLD << "CPU history " << RESET << "(last "
            << history.size() << "s):" << EOL << "  ";
        for (double v : history) out << loadColor(v) << spark(v) << RESET;
        out << EOL;

        out << EOL << BOLD << "Per-core:" << RESET << EOL;
        const auto& cores = mon.getCorePercents();
        const int PER_ROW = 4;
        for (size_t i = 0; i < cores.size(); ++i) {
            std::ostringstream cell;
            cell << "C" << std::left << std::setw(2) << i;
            out << "  " << cell.str() << makeBar(cores[i], 8)
                << std::right << std::setw(5) << (int)(cores[i] + 0.5) << "% ";
            if ((int)((i + 1) % PER_ROW) == 0) out << EOL;
        }
        if (cores.size() % PER_ROW != 0) out << EOL;

        const auto& ifaces = mon.getNetInterfaces();
        if (!ifaces.empty()) {
            out << EOL << BOLD << "Network by connection:" << RESET << EOL;
            int ifShown = 0;
            for (const auto& ni : ifaces) {
                if (ifShown++ >= 3) break;
                out << "  " << std::left << std::setw(10) << ni.name.substr(0, 10)
                    << GREEN << "down " << std::right << std::setw(9)
                    << humanRate(ni.rxKBps) << RESET << "  "
                    << CYAN  << "up "   << std::setw(9)
                    << humanRate(ni.txKBps) << RESET << EOL;
            }
        }

        out << EOL << BOLD << "Disk activity  " << RESET
            << YELLOW << "reading " << humanRate(mon.getDiskReadKBps())  << RESET
            << "   " << YELLOW << "writing " << humanRate(mon.getDiskWriteKBps())
            << RESET << EOL;
    }

    // =====================================================================
    // Program list (always shown).
    // =====================================================================
    out << EOL;

    // --- Task/thread summary (Phase 1): counts by process state ---
    const auto& tc = mon.getTaskCounts();
    out << BOLD << "Tasks     " << RESET << tc.total << " total"
        << ", " << GREEN << tc.running << " running" << RESET
        << ", " << tc.sleeping << " sleeping";
    if (tc.stopped > 0) out << ", " << tc.stopped << " stopped";
    if (tc.zombie  > 0) out << ", " << RED << tc.zombie << " zombie" << RESET;
    out << "   " << tc.threads << " threads" << EOL;

    if (!filter.empty()) {
        out << BOLD << YELLOW << "Showing only: \"" << filter << "\"" << RESET
            << "  (press / to change)" << EOL;
    }
    out << "Sorted by " << BOLD << sortName(mon.getSortMode()) << RESET
        << "  (c/m/p to change)" << EOL;

    const char* nameCol = showCmdline ? "COMMAND" : "PROGRAM";
    out << BOLD << GREEN << std::left
        << std::setw(8)  << "PID"
        << std::setw(8)  << "CPU%"
        << std::setw(11) << "MEMORY";
    if (detailed) out << std::setw(10) << "STATUS" << std::setw(10) << "PRIORITY";
    out << nameCol << RESET << EOL;

    const auto& procs = mon.getProcesses();
    int shown = 0, matched = 0;
    const int LIMIT = detailed ? 15 : 8;    // shorter list in simple mode
    for (const auto& p : procs) {
        if (!filter.empty() &&
            !containsCI(p.name, filter) && !containsCI(p.cmdline, filter))
            continue;
        ++matched;
        if (shown++ >= LIMIT) continue;

        std::ostringstream cpuStr;
        cpuStr << (int)(p.cpuPercent + 0.5) << "%";

        std::string label = showCmdline ? p.cmdline : p.name;
        if (label.size() > 55) label = label.substr(0, 52) + "...";

        out << std::left
            << std::setw(8)  << p.pid
            << std::setw(8)  << cpuStr.str()
            << std::setw(11) << humanKB(p.memKB);
        if (detailed)
            out << std::setw(10) << stateWord(p.state) << std::setw(10) << p.nice;
        out << label << EOL;
    }
    if (matched > shown)
        out << "  ... and " << (matched - shown) << " more" << EOL;

    // --- footer: a short menu in simple mode, the full one in detailed ---
    out << EOL;
    std::ostringstream rate;
    rate << std::fixed << std::setprecision(1) << intervalSec << "s";
    if (detailed) {
        out << "[q] quit  [k] stop  [r] priority  [/] find  [c/m/p] sort  [a] "
            << (showCmdline ? "short names" : "full command")
            << "  [+/-] rate " << rate.str() << "  [d] simple view" << EOL;
    } else {
        out << "[q] quit  [k] stop  [/] find  [c/m/p] sort  [+/-] rate "
            << rate.str() << "  [d] more detail" << EOL;
    }
    out << CLR_BELOW;                        // erase any old lines below the frame

    std::cout << out.str();                  // one write = no flicker
    std::cout.flush();
}

int main() {
    SystemMonitor mon;
    enableRawMode();

    // Clear the screen ONCE up front and hide the cursor. From here on every
    // frame is drawn in place (cursor homed), so the display updates instead of
    // scrolling. HIDE_CURSOR stops the cursor from flickering across the screen.
    std::cout << CLEAR_SCREEN << HIDE_CURSOR;
    std::cout.flush();

    // First update establishes the baseline sample; CPU% needs two samples,
    // so we do one silent update, then start the loop.
    mon.update();

    std::deque<double> cpuHistory;      // rolling window of overall CPU%
    const size_t HISTORY_LEN = 60;      // keep ~60 samples (~60 seconds)

    std::string filter;                 // process search filter ('/' key)
    bool showCmdline = false;           // show full command line vs short name ('a')
    bool detailed = false;              // simple overview vs technical view ('d')
    double interval = 1.0;              // refresh interval in seconds ('+' / '-')
    const double MIN_INTERVAL = 0.2, MAX_INTERVAL = 10.0;

    bool running = true;
    while (running) {
        mon.update();

        // Record the latest CPU% and keep only the most recent HISTORY_LEN
        // samples (a sliding window) for the trend graph.
        cpuHistory.push_back(mon.getCpuPercent());
        if (cpuHistory.size() > HISTORY_LEN) cpuHistory.pop_front();

        draw(mon, cpuHistory, filter, showCmdline, detailed, interval);

        char key = pollKey(interval);                 // waits up to `interval`s
        if (key == 'q') {
            running = false;
        } else if (key == 'd') {
            detailed = !detailed;                     // toggle simple / detailed view
        } else if (key == 'a') {
            showCmdline = !showCmdline;               // toggle name / command line
        } else if (key == 'c') {
            mon.setSortMode(SortMode::CPU);           // sort process list by CPU
        } else if (key == 'm') {
            mon.setSortMode(SortMode::MEM);           // sort by memory
        } else if (key == 'p') {
            mon.setSortMode(SortMode::PID);           // sort by PID
        } else if (key == '+' || key == '=') {
            // Slower refresh (longer interval). '=' is the unshifted '+' key.
            interval = std::min(MAX_INTERVAL, interval + 0.2);
        } else if (key == '-' || key == '_') {
            // Faster refresh (shorter interval).
            interval = std::max(MIN_INTERVAL, interval - 0.2);
        } else if (key == '/') {
            // Prompt for a search filter. Drop to cooked mode so the user can
            // type and edit a full line comfortably; an empty line clears it.
            disableRawMode();
            std::cout << CLEAR_SCREEN << SHOW_CURSOR;
            std::cout << "Filter processes (empty = clear): ";
            std::cout.flush();
            std::getline(std::cin, filter);
            enableRawMode();
            std::cout << CLEAR_SCREEN << HIDE_CURSOR;
            std::cout.flush();
        } else if (key == 'r') {
            // Renice: change a process's scheduling priority interactively.
            disableRawMode();
            std::cout << CLEAR_SCREEN << SHOW_CURSOR;
            std::cout << "Enter PID to renice: ";
            std::cout.flush();
            int pid = 0;
            std::cin >> pid;
            std::cout << "New nice value (-20 highest .. 19 lowest): ";
            int nv = 0;
            std::cin >> nv;
            bool ok = SystemMonitor::reniceProcess(pid, nv);
            std::cout << (ok ? "Priority changed.\n"
                             : "Failed (need privileges to raise priority, "
                               "or no such PID).\n");
            std::cout << "Press Enter to continue...";
            std::cin.ignore();
            std::cin.get();
            enableRawMode();
            std::cout << CLEAR_SCREEN << HIDE_CURSOR;
            std::cout.flush();
        } else if (key == 'k') {
            // Temporarily go back to normal (cooked) input so the user can
            // type a full PID and press Enter comfortably.
            disableRawMode();
            std::cout << CLEAR_SCREEN << SHOW_CURSOR;
            std::cout << "Enter PID to kill: ";
            std::cout.flush();
            int pid = 0;
            std::cin >> pid;
            std::cout << "Signal (15=SIGTERM polite, 9=SIGKILL force): ";
            int sig = 15;
            std::cin >> sig;
            bool ok = SystemMonitor::killProcess(pid, sig);
            std::cout << (ok ? "Signal sent.\n" : "Failed (permission or no such PID).\n");
            std::cout << "Press Enter to continue...";
            std::cin.ignore();
            std::cin.get();
            enableRawMode();
            std::cout << CLEAR_SCREEN << HIDE_CURSOR;  // back to live view
            std::cout.flush();
        }
    }

    disableRawMode();
    std::cout << SHOW_CURSOR << CLEAR_SCREEN << "Goodbye!\n";  // restore cursor
    return 0;
}
