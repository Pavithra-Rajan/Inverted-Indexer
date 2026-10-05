CXX := c++
CXXFLAGS := -O3 -std=c++17 -Iinclude
BUILD_DIR := build

all: $(BUILD_DIR)/build_runs $(BUILD_DIR)/merge $(BUILD_DIR)/dump_list $(BUILD_DIR)/query

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/build_runs: src/build_runs.cpp include/tokenizer.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/build_runs.cpp

$(BUILD_DIR)/merge: src/merge.cpp include/varbyte.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/merge.cpp

$(BUILD_DIR)/dump_list: src/dump_list.cpp include/varbyte.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/dump_list.cpp

$(BUILD_DIR)/query: src/query.cpp include/tokenizer.hpp include/varbyte.hpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -o $@ src/query.cpp

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
