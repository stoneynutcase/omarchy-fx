PLUGIN_NAME = omarchy-fx

SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:.cpp=.o)
DEPS    = $(OBJECTS:.o=.d)

# -MMD -MP: without header prerequisites, editing a .hpp rebuilds nothing, and
# a half-rebuilt tree links objects that disagree on class layout. That is not
# a link error — it is a crash on first use of a member past the mismatch.
CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++2b -Wall -Wno-unused-parameter -O2 -g -MMD -MP
CXXFLAGS += $(shell pkg-config --cflags pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon)

all: $(PLUGIN_NAME).so

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PLUGIN_NAME).so: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(DEPS) $(PLUGIN_NAME).so

.PHONY: all clean

-include $(DEPS)
