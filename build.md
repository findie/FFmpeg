# Building 

## macOS (for issues refer [here](https://trac.ffmpeg.org/wiki/CompilationGuide/macOS))

### Dependencies

Brew: 
```bash
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```

FFmpeg: 
```bash
brew install \
    automake fdk-aac git lame libass libtool libvorbis libvpx \
    opus sdl shtool texi2html theora wget x264 x265 xvid nasm \
    fontconfig fribidi
```

### Compiling

FFmpeg:
```bash
git clone https://github.com/findie/FFmpeg.git ffmpeg && cd ffmpeg && \
\
./configure \
    --enable-gpl  --enable-version3  --enable-nonfree \
    --enable-libvpx --enable-libx264 --enable-libx265 \
    --enable-libass --enable-libfdk-aac --enable-libfreetype \
    --enable-libopus --enable-libxvid --enable-libfontconfig --enable-libfribidi \
    --enable-libmp3lame --enable-libtheora --enable-libvorbis && \
\
make build -j4 && ./ffmpeg -h 2>&1 | head -n3
```
___

## Linux

### CentOS, RHEL, Fedora guide [here](https://trac.ffmpeg.org/wiki/CompilationGuide/Centos)

___

### Ubuntu, Debian, Mint (for issues refer [here](https://trac.ffmpeg.org/wiki/CompilationGuide/Ubuntu))

#### Dependencies

```bash
sudo su;

apt-get update && 
apt-get -y install \
    autoconf automake build-essential cmake \
    git libass-dev libfreetype6-dev libsdl2-dev \
    libtheora-dev libtool libva-dev libvdpau-dev \
    libvorbis-dev libxcb1-dev libxcb-shm0-dev \
    libxcb-xfixes0-dev mercurial pkg-config \
    texinfo wget zlib1g-dev fontconfig \
    libvpx-dev libx264-dev libx265-dev \
    libfdk-aac-dev libmp3lame-dev libopus-dev \
    yasm nasm libxvidcore-dev libfribidi-dev
    
```
#### Build
```bash
git clone https://github.com/findie/FFmpeg.git ffmpeg && cd ffmpeg && \
\
./configure \
    --enable-gpl  --enable-version3  --enable-nonfree \
    --enable-libvpx --enable-libx264 --enable-libx265 \
    --enable-libass --enable-libfdk-aac --enable-libfreetype \
    --enable-libopus --enable-libxvid --enable-libfontconfig --enable-libfribidi \
    --enable-libmp3lame --enable-libtheora --enable-libvorbis && \
\
make build -j4 && ./ffmpeg -h 2>&1 | head -n3
```
___

## Windows
https://trac.ffmpeg.org/wiki/CompilationGuide#Windows