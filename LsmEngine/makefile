CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra

ENGINE = lsm-engine
TEST = lsm-layout-test
BIN_DIR = bin

build: build_exe build_lib

build_lib:
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -c $(ENGINE).cpp -o $(BIN_DIR)/$(ENGINE).o

build_exe:
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(ENGINE).cpp $(TEST).cpp -o $(BIN_DIR)/$(ENGINE)

run: build_exe
	cd $(BIN_DIR) && ./$(ENGINE)

clean:
	rm -rf $(BIN_DIR)

.PHONY: build build_lib build_exe run clean
