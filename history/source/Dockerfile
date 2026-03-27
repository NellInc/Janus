FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    wget unzip nasm make xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Download and install Open Watcom V2
RUN wget -q https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz \
    -O /tmp/ow.tar.xz && \
    mkdir -p /opt/watcom && \
    tar -xf /tmp/ow.tar.xz -C /opt/watcom && \
    rm /tmp/ow.tar.xz

# Set up Open Watcom environment
ENV WATCOM=/opt/watcom
ENV PATH="${WATCOM}/binl64:${WATCOM}/binl:${PATH}"
ENV INCLUDE="${WATCOM}/h"
ENV EDPATH="${WATCOM}/eddat"

WORKDIR /src
