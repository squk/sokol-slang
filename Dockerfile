FROM debian:trixie-slim AS linux
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates g++ meson ninja-build pkg-config python3 glslang-tools \
    libegl-dev libegl-mesa0 libgles-dev libgl-dev libgl1-mesa-dri \
    libvulkan-dev vulkan-tools mesa-vulkan-drivers vulkan-validationlayers \
    libx11-dev libxi-dev libxcursor-dev xvfb xauth \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
ENTRYPOINT ["sh", "/work/tests/docker-test.sh"]
CMD ["linux"]

FROM linux AS multiplatform
ARG EMSDK_VERSION=6.0.2
RUN apt-get update && apt-get install -y --no-install-recommends \
    git xz-utils chromium g++-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch ${EMSDK_VERSION} https://github.com/emscripten-core/emsdk.git /opt/emsdk \
    && /opt/emsdk/emsdk install ${EMSDK_VERSION} \
    && /opt/emsdk/emsdk activate ${EMSDK_VERSION} \
    && ln -s /opt/emsdk/node/*/bin/node /usr/local/bin/node
ENV PATH="/opt/emsdk:/opt/emsdk/upstream/emscripten:${PATH}"
# Chromium runs in the disposable container without its user-namespace sandbox.
RUN printf '#!/bin/sh\nexec /usr/bin/chromium --no-sandbox --use-angle=swiftshader --enable-features=Vulkan --disable-vulkan-surface "$@"\n' \
    >/usr/local/bin/sokol-chromium && chmod +x /usr/local/bin/sokol-chromium
CMD ["all"]
