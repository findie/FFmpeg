# Building 

## macOS

### Dependencies

Brew: 
```bash
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
```

FFmpeg dependencies: 
```bash
brew install \
    automake fdk-aac git lame libass libtool libvorbis libvpx \
    opus sdl shtool texi2html theora wget x264 x265 xvid nasm \
    fontconfig
```

### Compiling

FFmpeg:
```bash

git clone git@github.com:findie/FFmpeg.git ffmpeg && cd ffmpeg && \

./configure \
    --enable-gpl  --enable-version3  --enable-nonfree \
    --enable-libvpx --enable-libx264 --enable-libx265 \
    --enable-libass --enable-libfdk-aac --enable-libfreetype \
    --enable-libopus --enable-libxvid --enable-libfontconfig \
    --enable-libmp3lame --enable-libtheora --enable-libvorbis && \

make build && ./ffmpeg -h 2>&1 | head -n3
```
___

## Linux
TBD

___

## Windows
TBD