CXX ?= c++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -Wpedantic
THREAD_FLAGS ?= -pthread

.PHONY: all test
all: sop_solver

sop_solver: sop_solver.cpp
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) $< -o $@

test_bounds: test_bounds.cpp sop_solver.cpp
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) test_bounds.cpp -o $@

test_engines: test_engines.cpp sop_solver.cpp
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) test_engines.cpp -o $@

test: sop_solver test_bounds test_engines
	./test_bounds
	./test_engines
	python3 test_solver.py ./sop_solver
