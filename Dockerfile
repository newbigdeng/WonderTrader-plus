FROM ubuntu:26.04 AS toolchain
ARG UBUNTU_MIRROR=
ENV DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai
RUN if [ -n "$UBUNTU_MIRROR" ]; then sed -i "s|http://archive.ubuntu.com/ubuntu|$UBUNTU_MIRROR|g; s|http://security.ubuntu.com/ubuntu|$UBUNTU_MIRROR|g" /etc/apt/sources.list.d/ubuntu.sources; fi \
    && apt-get -o Acquire::Languages=none update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake ninja-build pkg-config ca-certificates \
       python3 python3-dev python3-venv \
       libboost-filesystem-dev libboost-thread-dev libboost-system-dev \
       libboost-date-time-dev libboost-serialization-dev libboost-atomic-dev \
       libnng-dev libasio-dev libspdlog-dev libfmt-dev \
       zlib1g-dev libssl-dev libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*
ENV LD_LIBRARY_PATH=/opt/wt/native:/opt/wt/native/parsers:/opt/wt/native/traders

FROM toolchain AS build
WORKDIR /opt/wt-src
COPY src/ ./src/
COPY third_party/ ./third_party/
COPY docker/stage_native.py ./docker/stage_native.py
RUN cmake -S src -B /opt/wt-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DWT_PLUS_RUNTIME_ONLY=ON \
      -DMyDeps=/opt/wt-src/third_party/compat \
    && cmake --build /opt/wt-build --parallel 2 \
    && python3 docker/stage_native.py /opt/wt-build /opt/wt/native --source-root /opt/wt-src

FROM toolchain AS native
COPY --from=build /opt/wt/native/ /opt/wt/native/
COPY LICENSE /opt/wt/LICENSE
COPY src/ /opt/wt/src/
COPY third_party/compat/ /opt/wt/compat/
COPY tests/ /opt/wt/tests/
RUN mkdir -p /opt/wt/third_party && ln -s /opt/wt/compat /opt/wt/third_party/compat \
    && chmod -R a+rX /opt/wt/src /opt/wt/compat /opt/wt/tests /opt/wt/native
WORKDIR /workspace
CMD ["bash"]
