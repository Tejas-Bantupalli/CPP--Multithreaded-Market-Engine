CXX = g++
CXXFLAGS = -O2 -std=c++17 -pthread -Iinclude
CPPFLAGS = -MMD -MP
TARGET = a.out
SRC = $(wildcard src/*.cpp)
OBJ = $(SRC:.cpp=.o)
DEPS = $(OBJ:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $(TARGET)

src/%.o: src/%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJ) $(DEPS)
