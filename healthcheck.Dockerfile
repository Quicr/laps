#---------------------------------------------------------------------
# LAPS Relay health-check HTTP image
#---------------------------------------------------------------------

FROM alpine:3.20.3 AS builder

RUN apk add --no-cache \
    alpine-sdk \
    bash \
    ca-certificates \
    clang \
    cmake \
    curl \
    linux-headers \
    lld \
    openssl-dev \
    python3 \
    tcsh

WORKDIR /ws

COPY ./CMakeLists.txt ./
COPY ./version_config.h.in ./
COPY ./dependencies ./dependencies
COPY ./src ./src

ENV CFLAGS="-Wno-error=stringop-overflow"
ENV CXXFLAGS="-Wno-error=stringop-overflow -fpermissive -Wno-error=pedantic"

RUN cmake -S . -B build \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_TESTING=OFF \
    -DLAPS_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release
RUN cmake --build build --target relay_health_check -j "$(nproc)"
RUN cp build/src/relay_health_check /usr/local/bin/.

FROM alpine:3.20.3

RUN apk add --no-cache libstdc++ python3

COPY --from=builder /usr/local/bin/relay_health_check /usr/local/bin/.
COPY ./scripts/relay_health_http.py /usr/local/bin/relay_health_http.py

RUN addgroup -S laps
RUN adduser -D -S -S -G laps laps

USER laps
WORKDIR /home/laps

EXPOSE 8080/tcp

ENV RELAY_HEALTH_HTTP_HOST=0.0.0.0
ENV RELAY_HEALTH_HTTP_PORT=8080

CMD ["python3", "/usr/local/bin/relay_health_http.py"]
