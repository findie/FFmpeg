FROM ubuntu:bionic

RUN apt update && \
    apt install -y autoconf automake build-essential cmake git libass-dev libfreetype6-dev libsdl2-dev libtheora-dev libtool libva-dev libvdpau-dev libvorbis-dev libxcb1-dev libxcb-shm0-dev libxcb-xfixes0-dev mercurial pkg-config texinfo wget zlib1g-dev fontconfig libvpx-dev libx264-dev libx265-dev libmp3lame-dev libopus-dev yasm nasm libxvidcore-dev libfribidi-dev libtheora-dev libfdk-aac-dev && \
    mkdir -p /home/packages && cd /home/packages && git clone https://github.com/findie/FFmpeg.git ffmpeg && cd ffmpeg && \
    ./configure --enable-pthreads --enable-avresample --enable-gpl --enable-version3 --enable-nonfree --enable-libvpx --enable-libx264 --enable-libx265 --enable-libass --enable-libfdk-aac --enable-libfreetype --enable-libopus --enable-libxvid --enable-fontconfig --enable-libfontconfig --enable-libtheora --enable-libfribidi --enable-libmp3lame --enable-libtheora --enable-libvorbis --enable-swscale-alpha --enable-shared --enable-pic --extra-cflags=-fPIC && \
    make -j6 && make install  && ldconfig && cd / && rm -rf /home/packages \
    rm -rf /var/lib/apt/lists/* && apt clean;

ENTRYPOINT [ "ffmpeg" ]
CMD [ "-h" ]
