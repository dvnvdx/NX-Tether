# PC-side tests. Needs only a C++20 host compiler.
CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined

.PHONY: all check clean

all: check

test_core: test_core.cpp $(wildcard ../sysmodule/source/*.hpp)
	$(CXX) $(CXXFLAGS) -I../sysmodule/source $< -o $@

check: test_core
	./test_core

clean:
	rm -f test_core
