# Harmonix

Run Linux ELF binary (aarch64 and x86_64) on HarmonyOS PC, inspired by [Termony](https://github.com/TermonyHQ/Termony), based on [qemu-ohos](https://github.com/harmoninux/qemu).

![Run in Harmonix](./docs/images/screen_202508231607.jpg)

# Build

To build and install to your device, see [Build Termony on Linux](https://github.com/TermonyHQ/Termony?tab=readme-ov-file#usage-if-you-are-a-linux-user)

# Usage

- Build and sign hap of Harmonix
  - or download unsigned hap from [Release page](https://github.com/hackeris/Harmonix/releases) and sign the hap by your self
- Install Harmonix to your HarmonyOS PC
- Open Harmonix or HiShell, and run `harmonix install` to install Alpine Linux
- After Alpine Linux installed, run `harmonix run` to start Alpine Linux
- Run `cd /` cd to `/` and run `ls`, you can see the root changed!
- Run `uname -a` to display system info
- Run `apk` to install packages 

## x86_64

You can also run Linux program of x86_64 on HarmonyOS PC by qemu-harmonix-x86_64 in Harmonix.

- Download alpine minimal root filesystem from [https://alpinelinux.org/downloads/](https://alpinelinux.org/downloads/), choose "Mini root filesystem" -> "x86_64"  
- Extract downloaded rootfs tar.gz file to data directory, for example `/data/storage/el2/base/files/alpine_x64`
- Run `qemu-harmonix-x86_64` to load busybox shell with rootfs and environment variables
```shell
cd /data/storage/el2/base/files/alpine_x64
qemu-harmonix-x86_64 -E PATH=/bin:/usr/bin:/sbin -E HOME=/root -L ./ ./bin/busybox sh -c 'cd ~ && sh'
```
- Run `uname -a` to display system info.

# Features

- In Alpine Linux, you can use `apk` to install packages.
- Install `gcc` and `musl-dev` by `apk add gcc musl-dev`, then you can compile C code and run.
- Install `python` by `apk add python3`, then you can run python script.
- Install `openjdk17-jdk` by `apk add openjdk17-jdk`, then you can compile Java code by `javac`.
- More external programs from packages works.

![GCC compiles Hello World](docs/images/screen_202507192010.png)

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=hackeris/harmonix&type=Date)](https://www.star-history.com/#hackeris/harmonix&Date)
