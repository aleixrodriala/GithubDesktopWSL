CC = gcc
CC_ARM64 = aarch64-linux-gnu-gcc
CFLAGS = -O2 -Wall -Wextra -pedantic
LDFLAGS = -lpthread

all: wsl-git-daemon

# Cross-compiled for ARM64 WSL distros (Snapdragon / Windows on ARM hosts).
# Needs gcc-aarch64-linux-gnu. On a native aarch64 machine, plain `make`
# already produces an arm64 binary — copy it to the suffixed name instead.
arm64: wsl-git-daemon-arm64

wsl-git-daemon: daemon.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

wsl-git-daemon-arm64: daemon.c
	$(CC_ARM64) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: wsl-git-daemon
	install -m 755 wsl-git-daemon /usr/local/bin/

clean:
	rm -f wsl-git-daemon wsl-git-daemon-arm64

.PHONY: all arm64 install clean
