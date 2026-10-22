# Reproducible Dual-Target Build Environment for bgl-engine
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV CONAN_HOME=/tmp/conan_home

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    python3 \
    python3-pip \
    libsdl2-dev \
    libpng-dev \
    libjpeg-dev \
    libwebp-dev \
    libgtest-dev \
    libglm-dev \
    libspdlog-dev \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Install Conan 2.x
RUN pip3 install --no-cache-dir --break-system-packages conan

WORKDIR /workspace

# Verify compiler support
RUN g++ --version && cmake --version && ninja --version

CMD ["/bin/bash"]

