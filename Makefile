CXXFLAGS=-shared -fPIC --no-gnu-unique -Wall -g -DWLR_USE_UNSTABLE -std=c++2b -O2
INCLUDES = `pkg-config --cflags pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server xkbcommon lua5.5 hyprgraphics`
# hyprgraphics (Hyprgraphics::CImage, used by gfx.cpp's makeImageTexture())
# is a genuinely separate shared library Hyprland links against, unlike
# every other internal API this project reaches into so far (those are
# plain method calls through Hyprland's own already-loaded singletons -
# g_pHyprRenderer etc - resolved against the host process's own symbol
# table at dlopen time, no explicit link needed). CImage's constructor/
# destructor/etc. are actually implemented in libhyprgraphics.so itself,
# so this links against it explicitly rather than assuming those symbols
# happen to already be visible in Hyprland's process.
LIBS = `pkg-config --libs hyprgraphics`
SRC = $(shell find src -name '*.cpp')
TARGET = HyprLUI.so
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib

all:
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(SRC) $(LIBS) -o $(TARGET)

install:
	install -D -m 0755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)

clean:
	rm ./$(TARGET)

withhyprpmheaders: export PKG_CONFIG_PATH = $(XDG_DATA_HOME)/hyprpm/headersRoot/share/pkgconfig
withhyprpmheaders: all
