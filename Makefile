CXX ?= g++
CPPFLAGS += -Iinclude -Isrc -D_XOPEN_SOURCE_EXTENDED
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDLIBS += -lncursesw
SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))

.PHONY: all test clean
all: build/relay-chat
build/relay-chat: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@
build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@
-include $(OBJECTS:.o=.d)
test: all
	python3 tests/integration.py
	python3 tests/tui_smoke.py
clean:
	rm -f build/*.o build/*.d build/relay-chat
