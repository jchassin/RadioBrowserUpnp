# RadioBrowserUpnp - Makefile
# Builds the vendored ohNet (submodule) then the media server.
# Targets: Linux amd64/aarch64 (Ubuntu/Debian), Synology DSM/Container.

CXX ?= g++
CXXFLAGS ?= -O2 -std=c++17 -pthread
OHNET_DIR := thirdparty/ohNet
OHNET_BUILD := $(OHNET_DIR)/Build
OHNET_LIBS := $(OHNET_BUILD)/Obj/Posix/Release

# ohNet needs rsync or falls back to cp -u; rsync is often absent in
# minimal/synology environments so force the cp fallback.
# The 'devices' target builds only the native libs we link against
# (ohNetCore/proxies/devices); ohNet's default 'all' also runs
# bundle_binaries.py, which fails on plain aarch64 Linux (it only knows
# aarch64 with the kirkstone/scarthgap distros) and is useless here.
OHNET_MAKE = $(MAKE) -C $(OHNET_DIR) native_only=yes rsync=no devices proxies

# Matches the flags ohNet itself is compiled with (little endian on both
# amd64 and aarch64 Linux).
OHNET_DEFINES := -DDEFINE_LITTLE_ENDIAN -DDEFINE_TRACE -DPOSIX_STACK_TRACE -DSET_PTHREAD_NAMES -D_GNU_SOURCE -D_REENTRANT

INCLUDES := -Isrc -Isrc/generated -I$(OHNET_BUILD)/Include
LIBS := -pthread

SRCS := src/main.cpp \
         src/RadioBrowserClient.cpp \
         src/RadioBrowserMediaServer.cpp \
         src/WebConfig.cpp \
         src/generated/DvUpnpOrgContentDirectory1.cpp \
         src/generated/DvUpnpOrgConnectionManager1.cpp

OBJS := $(SRCS:.cpp=.o)
BIN := Build/bin/radiobrowserupnp

.PHONY: all clean ohnet install

all: $(BIN)

$(BIN): $(OBJS) | ohnet
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LIBS) \
	    $(OHNET_LIBS)/libohNetCore.a $(OHNET_LIBS)/libohNetDevices.a $(OHNET_LIBS)/libohNetProxies.a

%.o: %.cpp ohnet
	$(CXX) $(CXXFLAGS) $(OHNET_DEFINES) $(INCLUDES) -c -o $@ $<

ohnet: $(OHNET_LIBS)/libohNetCore.a

$(OHNET_LIBS)/libohNetCore.a:
	$(OHNET_MAKE)

PREFIX ?= /usr/local

install: $(BIN)
	install -D -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/radiobrowserupnp

clean:
	rm -f $(OBJS) $(BIN)
	$(MAKE) -C $(OHNET_DIR) clean 2>/dev/null || true
