CXX      ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDLIBS   := -lssh

TARGET   := madbackuper
SOURCES  := $(wildcard src/*.cpp) $(wildcard src/modules/*.cpp)
OBJECTS  := $(patsubst %.cpp,build/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build $(TARGET)

debug: CXXFLAGS := -std=c++17 -O1 -g3 -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LDLIBS += -fsanitize=address,undefined
debug: clean all

-include $(DEPS)

.PHONY: all clean debug
