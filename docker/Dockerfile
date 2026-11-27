# Reproducible Dual-Target Build Environment for bgl-engine
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV CONAN_HOME=/tmp/conan_home

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    git \
    python3 \
    python3-pip \
    clang \
    clang-format \
    clang-tidy \
    doxygen \
    graphviz \
    libsdl2-dev \
    libcurl4-openssl-dev \
    libgl1-mesa-dev \
    libegl1-mesa-dev \
    libvulkan-dev \
    vulkan-tools \
    libpng-dev \
    libjpeg-dev \
    libwebp-dev \
    libgtest-dev \
    libglm-dev \
    libspdlog-dev \
    pkg-config \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install pinned Conan 2.x release and switch to non-root builder user
RUN pip3 install --no-cache-dir --break-system-packages "conan>=2.0,<3.0" \
    && useradd -m -s /bin/bash builder \
    && mkdir -p /workspace && chown -R builder:builder /workspace

USER builder
ENV CONAN_HOME=/home/builder/.conan2

WORKDIR /workspace

# Verify compiler support
RUN g++ --version && cmake --version && ninja --version && conan --version

CMD ["/bin/bash"]

