CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude
LDFLAGS  := -lssh
TARGET   := madbackuper

SRC_MAIN := src/main.cpp
SRC_MODS := $(wildcard src/modules/*.cpp)

OBJ := $(SRC_MAIN:.cpp=.o) $(SRC_MODS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
