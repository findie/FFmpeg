# Building 

## Dependencies

### macOS (for issues refer [here](https://trac.ffmpeg.org/wiki/CompilationGuide/macOS))

```bash
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```

```bash
brew install \
    automake git lame libass libtool libvorbis libvpx \
    opus sdl shtool texi2html theora wget x264 x265 xvid nasm \
    fontconfig fribidi libvidstab theora fdk-aac
```

### Ubuntu, Debian, Mint (for issues refer [here](https://trac.ffmpeg.org/wiki/CompilationGuide/Ubuntu))

#### VidStab Prerequisite
```bash
sudo apt install yasm nasm \
                build-essential cmake automake autoconf \
                libtool pkg-config libcurl4-openssl-dev \
                intltool libxml2-dev libgtk2.0-dev \
                libnotify-dev libglib2.0-dev libevent-dev \
                checkinstall libav-tools

git clone https://github.com/georgmartius/vid.stab.git
cd vid.stab/
cmake .
make
sudo make install
```

```bash
sudo su;

apt update && \
apt -y install \
    autoconf automake build-essential cmake \
    git libass-dev libfreetype6-dev libsdl2-dev \
    libtheora-dev libtool libva-dev libvdpau-dev \
    libvorbis-dev libxcb1-dev libxcb-shm0-dev \
    libxcb-xfixes0-dev mercurial pkg-config \
    texinfo wget zlib1g-dev fontconfig \
    libvpx-dev libx264-dev libx265-dev \
    libmp3lame-dev libopus-dev \
    yasm nasm libxvidcore-dev libfribidi-dev libtheora-dev libfdk-aac-dev
    
```

_For CentOS, RHEL, Fedora please follow guide [here](https://trac.ffmpeg.org/wiki/CompilationGuide/Centos)_<br/>
_For Windows please follow guide [here](https://trac.ffmpeg.org/wiki/CompilationGuide#Windows)_

## Compiling

### MacOS
```bash
git clone https://github.com/findie/FFmpeg.git ffmpeg && cd ffmpeg && \
\
./configure --enable-pthreads --enable-avresample --enable-openssl \
    --enable-gpl --enable-version3 --enable-nonfree \
    --enable-libvpx --enable-libx264 --enable-libx265 \
    --enable-libass --enable-libfdk-aac --enable-libfreetype \
    --enable-libopus --enable-libxvid --enable-fontconfig --enable-libfontconfig --enable-libtheora --enable-libfribidi \
    --enable-libmp3lame --enable-libtheora --enable-libvorbis --enable-libvidstab \
    --enable-swscale-alpha --enable-shared && \
\
make build -j `(nproc || echo 8)` && \
make install && \
./ffmpeg -h 2>&1 | grep -i findie || echo "FFMPEG not on findie branch"
```

### Linux
```bash
git clone https://github.com/findie/FFmpeg.git ffmpeg && cd ffmpeg && \
\
./configure --enable-pthreads --enable-avresample --enable-openssl \
    --enable-gpl --enable-version3 --enable-nonfree \
    --enable-libvpx --enable-libx264 --enable-libx265 \
    --enable-libass --enable-libfdk-aac --enable-libfreetype \
    --enable-libopus --enable-libxvid --enable-fontconfig --enable-libfontconfig --enable-libtheora --enable-libfribidi \
    --enable-libmp3lame --enable-libtheora --enable-libvorbis --enable-libvidstab \
    --enable-swscale-alpha --enable-shared --enable-pic --extra-cflags="-fPIC" && \
\
make build -j `(nproc || echo 4)` && \
sudo make install && \
sudo ldconfig && \
ffmpeg -h 2>&1 | grep -i findie || echo "FFMPEG not on findie branch"
```
___

### CUDA support

- Install Nvidia headers from [here](http://git.videolan.org/?p=ffmpeg/nv-codec-headers.git)
- Add to config
```
--enable-cuda --enable-cuvid --enable-nvenc --enable-cuda-sdk --enable-libnpp \
--extra-cflags='-fPIC -I/usr/local/cuda/include' \
--extra-ldflags=-L/usr/local/cuda/lib64
```

___

## Troubleshooting 

### Linux

#### libvidstab 

In case you get errors like: 
```bash
ffmpeg: error while loading shared libraries: libvidstab.so.1.1: cannot open shared object file: No such file or directory
```

You can install libvidstab from:
 - amd64 [page](https://debian.pkgs.org/9/multimedia-main-amd64/libvidstab1.0_0.98b-dmo1+deb8u1_amd64.deb.html) and [.deb file](http://www.deb-multimedia.org/pool/main/v/vid.stab/libvidstab1.0_0.98b-dmo1+deb8u1_amd64.deb)
 - i386 [page](https://debian.pkgs.org/9/multimedia-main-i386/libvidstab1.0_0.98b-dmo1+deb8u1_i386.deb.html) and [.deb file](http://www.deb-multimedia.org/pool/main/v/vid.stab/libvidstab1.0_0.98b-dmo1+deb8u1_i386.deb)

### Debugging
#### *db (lldb/gdb)
Add ` --disable-stripping --disable-optimizations --enable-debug=1 --enable-shared --disable-static --disable-optimizations --disable-mmx --disable-stripping --enable-debug` to configure step
