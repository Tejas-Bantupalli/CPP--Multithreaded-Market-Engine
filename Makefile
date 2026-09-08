CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -pthread -Wall -Wextra -Iinclude
CPPFLAGS = -MMD -MP
BUILD_DIR = build

CORE_SRC = src/order_book.cpp src/engine.cpp src/logger.cpp src/report.cpp src/strategies.cpp src/plugin_loader.cpp
CORE_OBJ = $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(CORE_SRC))
MAIN_OBJ = $(BUILD_DIR)/main.o
TEST_OBJ = $(BUILD_DIR)/tests_engine_tests.o
REPLAY_OBJ = $(BUILD_DIR)/tools_replay.o
DEPS = $(CORE_OBJ:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(REPLAY_OBJ:.o=.d)

TARGET = $(BUILD_DIR)/market_engine
TEST_TARGET = $(BUILD_DIR)/engine_tests
REPLAY_TARGET = $(BUILD_DIR)/replay
PLUGIN_DIR = $(BUILD_DIR)/plugins
EXAMPLE_PLUGIN = $(PLUGIN_DIR)/example_breakout.so

.PHONY: all clean test run plugin

all: $(TARGET) $(REPLAY_TARGET) $(EXAMPLE_PLUGIN)

$(BUILD_DIR) $(PLUGIN_DIR):
	mkdir -p $@

# Strategy plugins: make plugin SRC=plugins/foo.cpp  ->  build/plugins/foo.so
$(PLUGIN_DIR)/%.so: plugins/%.cpp $(wildcard include/*.h) | $(PLUGIN_DIR)
	$(CXX) $(CXXFLAGS) -shared -fPIC $< -o $@

plugin: | $(PLUGIN_DIR)
	@test -n "$(SRC)" || (echo "usage: make plugin SRC=path/to/strategy.cpp" && exit 2)
	$(CXX) $(CXXFLAGS) -shared -fPIC $(SRC) -o $(PLUGIN_DIR)/$(basename $(notdir $(SRC))).so
	@echo built $(PLUGIN_DIR)/$(basename $(notdir $(SRC))).so

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

test: $(TEST_TARGET) $(TARGET) $(EXAMPLE_PLUGIN)
	python3 tests/run_tests.py $(TEST_TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR)
