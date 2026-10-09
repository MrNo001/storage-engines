CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -I$(CURDIR)/$(INC_DIR)

ENGINE = lsm-engine
TEST = lsm-layout-test
SRC_DIR = src
INC_DIR = h
BIN_DIR = bin
SRCS = $(addprefix $(SRC_DIR)/,$(ENGINE).cpp wal.cpp manifest.cpp utils.cpp)

build: build_exe build_lib

build_lib:
	mkdir -p $(BIN_DIR)
	cd $(BIN_DIR) && $(CXX) $(CXXFLAGS) -c $(addprefix ../,$(SRCS))

build_exe:
	mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(SRCS) $(SRC_DIR)/$(TEST).cpp -o $(BIN_DIR)/$(ENGINE)

run: build_exe
	cd $(BIN_DIR) && ./$(ENGINE)

clean:
	rm -rf $(BIN_DIR)

.PHONY: build build_lib build_exe run clean
