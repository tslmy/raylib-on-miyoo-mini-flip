FROM ubuntu:22.04

RUN apt-get update && \
  DEBIAN_FRONTEND=noninteractive apt-get install -y \
  gcc-arm-linux-gnueabihf \
  g++-arm-linux-gnueabihf \
  binutils-arm-linux-gnueabihf \
  make \
  ccache \
  && rm -rf /var/lib/apt/lists/*

ENV CC=arm-linux-gnueabihf-gcc \
  CXX=arm-linux-gnueabihf-g++ \
  AR=arm-linux-gnueabihf-ar \
  RANLIB=arm-linux-gnueabihf-ranlib \
  STRIP=arm-linux-gnueabihf-strip

WORKDIR /build

CMD ["/build/src/docker/build.sh"]
