CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude
CPPFLAGS = -MMD -MP
BUILD_DIR = build

CORE_SRC = src/order_book.cpp src/engine.cpp src/logger.cpp src/report.cpp src/strategies.cpp
CORE_OBJ = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(CORE_SRC))
MAIN_OBJ = $(BUILD_DIR)/main.o
TEST_OBJ = $(BUILD_DIR)/tests_engine_tests.o
REPLAY_OBJ = $(BUILD_DIR)/tools_replay.o
DEPS = $(CORE_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(REPLAY_OBJ:.o=.d)

TARGET = $(BUILD_DIR)/market_engine
TEST_TARGET = $(BUILD_DIR)/engine_tests
REPLAY_TARGET = $(BUILD_DIR)/replay

.PHONY: all clean test run

all: $(TARGET) $(REPLAY_TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(CORE_OBJ) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_TARGET): $(CORE_OBJ) $(TEST_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(REPLAY_TARGET): $(BUILD_DIR)/order_book.o $(REPLAY_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tests_%.o: tests/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/tools_%.o: tools/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

test: $(TEST_TARGET)
	python3 tests/run_tests.py $(TEST_TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)
