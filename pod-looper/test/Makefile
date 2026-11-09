# Host-side tests (no Daisy needed): make -C test
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -fsanitize=address,undefined

run: engine_test synth_test
	./engine_test
	./synth_test

engine_test: engine_test.cpp ../src/looper.cpp ../src/looper.h
	$(CXX) $(CXXFLAGS) -o $@ engine_test.cpp ../src/looper.cpp

synth_test: synth_test.cpp ../src/synth.cpp ../src/synth.h
	$(CXX) $(CXXFLAGS) -o $@ synth_test.cpp ../src/synth.cpp

clean:
	rm -f engine_test synth_test

.PHONY: run clean
