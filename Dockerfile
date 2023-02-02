#---------------------------------------------------------------------
# LAPS-Relay Docker image
#---------------------------------------------------------------------

# Build layer
FROM alpine:latest as builder

RUN apk add --no-cache cmake alpine-sdk openssl-dev
RUN apk add --no-cache tcsh bash
RUN apk add --no-cache \
        ca-certificates \
        clang lld curl

WORKDIR /ws

COPY ./Makefile ./
COPY ./CMakeLists.txt ./
COPY ./version_config.h.in ./
COPY ./dependencies ./dependencies
COPY ./src ./src

RUN  make all

RUN cp  build/src/lapsRelay/lapsRelay  /usr/local/bin/. \
    && cp  build/src/lapsTest/lapsTest  /usr/local/bin/.


# Run layer
FROM alpine:latest
RUN apk add --no-cache libstdc++
RUN apk add --no-cache bash tcsh

COPY --from=builder /usr/local/bin/lapsRelay /usr/local/bin/.
COPY --from=builder /usr/local/bin/lapsTest /usr/local/bin/.

RUN addgroup -S laps
RUN adduser -D -S -S -G laps laps
USER laps
WORKDIR /home/laps

EXPOSE 33434/udp
EXPOSE 33434/tcp

CMD /usr/local/bin/lapsRelay

