# Simple Makefile for the Linux System Monitor
# Usage:
#   make        -> build the ./monitor executable
#   make run    -> build and run
#   make clean  -> remove build artifacts

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
TARGET   = monitor
OBJS     = main.o SystemMonitor.o

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJS)

main.o: main.cpp SystemMonitor.h Process.h
	$(CXX) $(CXXFLAGS) -c main.cpp

SystemMonitor.o: SystemMonitor.cpp SystemMonitor.h Process.h
	$(CXX) $(CXXFLAGS) -c SystemMonitor.cpp

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: run clean
