PLUGIN_NAME = omarchy-fx

SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:.cpp=.o)

CXXFLAGS += -shared -fPIC --no-gnu-unique -std=c++2b -Wall -Wno-unused-parameter -O2 -g
CXXFLAGS += $(shell pkg-config --cflags pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon)

all: $(PLUGIN_NAME).so

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PLUGIN_NAME).so: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS)

clean:
	rm -f $(OBJECTS) $(PLUGIN_NAME).so

.PHONY: all clean
