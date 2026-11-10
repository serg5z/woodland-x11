# XTS (X Test Suite) with Xvfb as the reference server, for conformance runs of the host server.
# Build:  docker build -t woodland-xts experiments/x11-esp32/tools/xts
# Run:    see run_xts.sh (usage at its top) and README.md
FROM debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential autoconf automake libtool pkg-config xutils-dev perl \
    libx11-dev libxau-dev libxext-dev libxi-dev libxtst-dev libxt-dev libxmu-dev libxaw7-dev libxtrans-dev \
    xvfb x11-utils x11-xserver-utils xfonts-utils xfonts-base xauth python3 gdb git ca-certificates \
    && rm -rf /var/lib/apt/lists/*
# XTS master as of the 2026-11-09 campaign (6cf94400a09abecd6b86e4eb6441741acecd51f6)
RUN git clone https://gitlab.freedesktop.org/xorg/test/xts.git /opt/xts-src \
    && cd /opt/xts-src && git checkout 6cf94400a09abecd6b86e4eb6441741acecd51f6 \
    && ./autogen.sh --prefix=/opt/xts >/tmp/xts-build.log 2>&1 && make -j8 >>/tmp/xts-build.log 2>&1
COPY run_xts.sh /opt/run_xts.sh
