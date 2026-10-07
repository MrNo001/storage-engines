CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

build: build_exe build_lib

build_lib:
	mkdir -p bin
	$(CXX) $(CXXFLAGS) -c hash_engine.cpp -o bin/hash-engine.o

build_exe:
	mkdir -p bin
	$(CXX) $(CXXFLAGS) hash_engine.cpp hash_engine_test.cpp -o bin/hash_engine

run: build_exe
	cd bin && ./hash_engine

clean:
	rm -f bin/hash-engine.o bin/hash_engine
