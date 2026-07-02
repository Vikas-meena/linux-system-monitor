// ===========================================================================
// Linux System Monitor  (C++)
// ---------------------------------------------------------------------------
// A terminal program that shows live CPU, memory and per-process usage by
// reading the /proc filesystem — a hands-on tour of OS process/memory concepts.
//
// Controls:
//     q        quit
//     k        kill a process (prompts for PID + signal)
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
#include <csignal>       // SIGTERM, SIGKILL
#include <termios.h>     // terminal mode control (raw / cooked)
#include <unistd.h>      // read, STDIN_FILENO
#include <sys/select.h>  // select() -> check for a keypress without blocking

// --- ANSI escape codes: control the terminal cursor/colors as plain text ---
static const char* CLEAR_SCREEN = "\033[2J\033[H"; // clear + cursor to top-left
static const char* BOLD         = "\033[1m";
static const char* RESET        = "\033[0m";
static const char* CYAN         = "\033[36m";
static const char* GREEN        = "\033[32m";
static const char* YELLOW       = "\033[33m";
static const char* RED          = "\033[31m";

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

// Return a pressed key if one is waiting, else 0. Uses select() with a 1-second
// timeout, which doubles as our refresh interval: it sleeps up to 1s but wakes
// instantly if the user presses a key.
static char pollKey() {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    struct timeval timeout{1, 0};                     // 1 second, 0 microsec
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

// Draw one refresh of the whole screen.
// `history` holds recent overall-CPU% values so we can draw a trend graph.
static void draw(const SystemMonitor& mon, const std::deque<double>& history) {
    std::cout << CLEAR_SCREEN;
    std::cout << BOLD << CYAN
              << "=== Linux System Monitor (C++) ===" << RESET << "\n\n";

    std::cout << std::fixed << std::setprecision(1);

    // --- CPU with a bar graph ---
    double cpu = mon.getCpuPercent();
    std::cout << BOLD << "CPU  " << RESET
              << makeBar(cpu, 30) << "  " << cpu << "%\n";

    // --- RAM with a bar graph ---
    double memPct = mon.getTotalMemKB()
                  ? 100.0 * mon.getUsedMemKB() / mon.getTotalMemKB() : 0.0;
    std::cout << BOLD << "RAM  " << RESET
              << makeBar(memPct, 30) << "  "
              << humanKB(mon.getUsedMemKB()) << " / "
              << humanKB(mon.getTotalMemKB()) << "\n";

    // --- Swap with a bar graph (only if swap exists) ---
    if (mon.getTotalSwapKB() > 0) {
        double swapPct = 100.0 * mon.getUsedSwapKB() / mon.getTotalSwapKB();
        std::cout << BOLD << "Swap " << RESET
                  << makeBar(swapPct, 30) << "  "
                  << humanKB(mon.getUsedSwapKB()) << " / "
                  << humanKB(mon.getTotalSwapKB()) << "\n";
    }

    // --- CPU history as a scrolling sparkline graph ---
    std::cout << "\n" << BOLD << "CPU history " << RESET << "(last "
              << history.size() << "s):\n  ";
    for (double v : history) std::cout << loadColor(v) << spark(v) << RESET;
    std::cout << "\n";

    // --- Per-core CPU bars ---
    std::cout << "\n" << BOLD << "Per-core:" << RESET << "\n";
    const auto& cores = mon.getCorePercents();
    for (size_t i = 0; i < cores.size(); ++i) {
        std::ostringstream label;
        label << "Core" << i;
        std::cout << "  " << std::left << std::setw(7) << label.str()
                  << makeBar(cores[i], 20) << "  " << cores[i] << "%\n";
    }

    std::cout << "\n" << BOLD << GREEN
              << std::left
              << std::setw(8)  << "PID"
              << std::setw(22) << "NAME"
              << std::setw(7)  << "STATE"
              << std::setw(8)  << "CPU%"
              << std::setw(6)  << "NICE"
              << std::setw(10) << "MEM"
              << RESET << "\n";

    // --- top processes by CPU ---
    const auto& procs = mon.getProcesses();
    int shown = 0;
    for (const auto& p : procs) {
        if (shown++ >= 15) break;                     // top 15 fit on a screen

        // Format cpu% into a small string so column alignment stays clean.
        std::ostringstream cpu;
        cpu << std::fixed << std::setprecision(1) << p.cpuPercent;

        std::cout << std::left
                  << std::setw(8)  << p.pid
                  << std::setw(22) << p.name.substr(0, 21)
                  << std::setw(7)  << p.state
                  << std::setw(8)  << cpu.str()
                  << std::setw(6)  << p.nice
                  << std::setw(10) << humanKB(p.memKB)
                  << "\n";
    }
}

int main() {
    SystemMonitor mon;
    enableRawMode();

    // First update establishes the baseline sample; CPU% needs two samples,
    // so we do one silent update, then start the loop.
    mon.update();

    std::deque<double> cpuHistory;      // rolling window of overall CPU%
    const size_t HISTORY_LEN = 60;      // keep ~60 samples (~60 seconds)

    bool running = true;
    while (running) {
        mon.update();

        // Record the latest CPU% and keep only the most recent HISTORY_LEN
        // samples (a sliding window) for the trend graph.
        cpuHistory.push_back(mon.getCpuPercent());
        if (cpuHistory.size() > HISTORY_LEN) cpuHistory.pop_front();

        draw(mon, cpuHistory);

        std::cout << "\n" << "[q] quit   [k] kill process\n";
        std::cout.flush();

        char key = pollKey();                         // waits up to 1s
        if (key == 'q') {
            running = false;
        } else if (key == 'k') {
            // Temporarily go back to normal (cooked) input so the user can
            // type a full PID and press Enter comfortably.
            disableRawMode();
            std::cout << "\nEnter PID to kill: ";
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
        }
    }

    disableRawMode();
    std::cout << CLEAR_SCREEN << "Goodbye!\n";
    return 0;
}
