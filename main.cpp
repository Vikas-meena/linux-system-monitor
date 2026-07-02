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

// Draw one refresh IN PLACE.
// Instead of clearing the whole screen (which flickers and scrolls), we move
// the cursor to the top-left with HOME and overwrite the old text line by line.
// Every line ends with EOL ("\033[K\n") to erase leftovers from the last frame,
// and we finish with CLR_BELOW to wipe any lines the new frame didn't cover.
// `history` holds recent overall-CPU% values so we can draw a trend graph.
static void draw(const SystemMonitor& mon, const std::deque<double>& history) {
    std::ostringstream out;                 // build the whole frame, print once
    out << HOME;
    out << BOLD << CYAN
        << "=== Linux System Monitor (C++) ===" << RESET << EOL << EOL;

    out << std::fixed << std::setprecision(1);

    // --- CPU with a bar graph ---
    double cpu = mon.getCpuPercent();
    out << BOLD << "CPU  " << RESET << makeBar(cpu, 30) << "  " << cpu << "%" << EOL;

    // --- RAM with a bar graph ---
    double memPct = mon.getTotalMemKB()
                  ? 100.0 * mon.getUsedMemKB() / mon.getTotalMemKB() : 0.0;
    out << BOLD << "RAM  " << RESET << makeBar(memPct, 30) << "  "
        << humanKB(mon.getUsedMemKB()) << " / "
        << humanKB(mon.getTotalMemKB()) << EOL;

    // --- Swap with a bar graph (only if swap exists) ---
    if (mon.getTotalSwapKB() > 0) {
        double swapPct = 100.0 * mon.getUsedSwapKB() / mon.getTotalSwapKB();
        out << BOLD << "Swap " << RESET << makeBar(swapPct, 30) << "  "
            << humanKB(mon.getUsedSwapKB()) << " / "
            << humanKB(mon.getTotalSwapKB()) << EOL;
    }

    // --- CPU history as a scrolling sparkline graph ---
    out << EOL << BOLD << "CPU history " << RESET << "(last "
        << history.size() << "s):" << EOL << "  ";
    for (double v : history) out << loadColor(v) << spark(v) << RESET;
    out << EOL;

    // --- Per-core CPU bars, packed several per row to stay compact ---
    out << EOL << BOLD << "Per-core:" << RESET << EOL;
    const auto& cores = mon.getCorePercents();
    const int PER_ROW = 4;                  // how many core bars on one line
    for (size_t i = 0; i < cores.size(); ++i) {
        std::ostringstream cell;
        cell << "C" << std::left << std::setw(2) << i;      // e.g. "C0 "
        out << "  " << cell.str() << makeBar(cores[i], 8)
            << std::right << std::setw(6) << cores[i] << "% ";
        if ((int)((i + 1) % PER_ROW) == 0) out << EOL;      // end of a row
    }
    if (cores.size() % PER_ROW != 0) out << EOL;            // finish last row

    // --- process table header ---
    out << EOL << BOLD << GREEN << std::left
        << std::setw(8)  << "PID"
        << std::setw(18) << "NAME"
        << std::setw(7)  << "STATE"
        << std::setw(8)  << "CPU%"
        << std::setw(6)  << "NICE"
        << std::setw(10) << "MEM"
        << RESET << EOL;

    // --- top processes by CPU ---
    const auto& procs = mon.getProcesses();
    int shown = 0;
    for (const auto& p : procs) {
        if (shown++ >= 10) break;           // top 10 keeps the screen compact

        std::ostringstream cpuStr;
        cpuStr << std::fixed << std::setprecision(1) << p.cpuPercent;

        out << std::left
            << std::setw(8)  << p.pid
            << std::setw(18) << p.name.substr(0, 17)
            << std::setw(7)  << p.state
            << std::setw(8)  << cpuStr.str()
            << std::setw(6)  << p.nice
            << std::setw(10) << humanKB(p.memKB)
            << EOL;
    }

    out << EOL << "[q] quit   [k] kill process" << EOL;
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

    bool running = true;
    while (running) {
        mon.update();

        // Record the latest CPU% and keep only the most recent HISTORY_LEN
        // samples (a sliding window) for the trend graph.
        cpuHistory.push_back(mon.getCpuPercent());
        if (cpuHistory.size() > HISTORY_LEN) cpuHistory.pop_front();

        draw(mon, cpuHistory);          // draws in place + its own footer

        char key = pollKey();                         // waits up to 1s
        if (key == 'q') {
            running = false;
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
