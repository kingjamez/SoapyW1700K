# SPDX-License-Identifier: MIT
# SoapySDR module for the W1700K: build/libw1700kSupport.so. Needs a C++17 compiler, SoapySDR
# (libsoapysdr-dev) and nlohmann-json (nlohmann-json3-dev). CMakeLists.txt builds the same module
# for people who prefer CMake.
#
#   make                 build
#   sudo make install    into SoapySDR's module directory: every SoapySDR program finds it
#   make install-user    into ~/.local/lib/SoapySDR/modules<ABI>; SoapySDR does NOT search that by
#                        default, so see README.md ("SOAPY_SDR_PLUGIN_PATH")
#
# Variables: W1700K_CMD, the `w1700k` command the module runs by default: a path, or a name looked up
# in PATH (`w1700k` unless set; the module falls back to `w1700k` in PATH if a path is missing at run
# time); DESTDIR, for packaging; WARNINGS=-Werror, for CI.
CXX ?= c++
CXXFLAGS ?= -O2
WARNINGS ?= -Wall -Wextra
VERSION ?= 0.1.0
W1700K_CMD ?= w1700k

# SoapySDR's own module directory (the first of the search paths it reports), and its name, which
# carries the ABI version ("modules0.8"). Empty if SoapySDRUtil is missing.
SEARCH_PATH := $(shell SoapySDRUtil --info 2>/dev/null | sed -n 's/^Search path: *\([^ ]*\).*/\1/p' | head -1)
MODDIR ?= $(SEARCH_PATH)
MODNAME := $(notdir $(SEARCH_PATH))

override CXXFLAGS += -std=c++17 -fPIC -Iinclude -DW1700K_CMD='"$(W1700K_CMD)"' \
	-DW1700K_MODULE_VERSION='"$(VERSION)"' $(WARNINGS)
LDLIBS += -lSoapySDR -lrt -pthread

SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))

.PHONY: all clean install install-user
all: build/libw1700kSupport.so

build/libw1700kSupport.so: $(OBJECTS)
	$(CXX) $(CXXFLAGS) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp $(wildcard src/*.hpp) include/w1700k_stream.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p $@

install: all
	@test -n "$(MODDIR)" || { echo "cannot find SoapySDR's module directory (is SoapySDRUtil installed?): set MODDIR="; exit 1; }
	install -D -m 755 build/libw1700kSupport.so $(DESTDIR)$(MODDIR)/libw1700kSupport.so

install-user: all
	@test -n "$(MODNAME)" || { echo "cannot find SoapySDR's module directory name (is SoapySDRUtil installed?)"; exit 1; }
	install -D -m 755 build/libw1700kSupport.so $(HOME)/.local/lib/SoapySDR/$(MODNAME)/libw1700kSupport.so

clean:
	rm -rf build
