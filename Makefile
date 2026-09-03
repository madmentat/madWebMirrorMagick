CXX      ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
LDLIBS   := -lssh -pthread

TARGET   := madbackuper
HELPER   := madweb-helper
SOURCES  := $(wildcard src/*.cpp) $(wildcard src/modules/*.cpp)
OBJECTS  := $(patsubst %.cpp,build/%.o,$(SOURCES))
DEPS     := $(OBJECTS:.o=.d)

all: $(TARGET) $(HELPER)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(HELPER): tools/madweb-helper.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build $(TARGET) $(HELPER)

debug: CXXFLAGS := -std=c++17 -O1 -g3 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer
debug: LDLIBS += -fsanitize=address,undefined
debug: clean all

-include $(DEPS)

.PHONY: all clean debug
