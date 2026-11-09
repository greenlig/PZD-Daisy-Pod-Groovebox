# Host-side engine tests (no Daisy needed): make -C test
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined

run: engine_test
	./engine_test

engine_test: engine_test.cpp ../src/looper.cpp ../src/looper.h
	$(CXX) $(CXXFLAGS) -o $@ engine_test.cpp ../src/looper.cpp

clean:
	rm -f engine_test

.PHONY: run clean
