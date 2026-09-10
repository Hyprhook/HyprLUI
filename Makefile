CXXFLAGS = -fPIC --no-gnu-unique -Wall -g -DWLR_USE_UNSTABLE -std=c++2b -O2
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon lua5.5 hyprgraphics`
LIBS = `pkg-config --libs hyprgraphics`
SRC = $(shell find src -name '*.cpp')
OBJ = $(SRC:.cpp=.o)
TARGET = HyprLUI.so
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) -shared $(OBJ) $(LIBS) -o $(TARGET)

%.o: %.cpp
	@echo "Compiling $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

install:
	install -D -m 0755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJ)

withhyprpmheaders: export PKG_CONFIG_PATH = $(XDG_DATA_HOME)/hyprpm/headersRoot/share/pkgconfig
withhyprpmheaders: all
