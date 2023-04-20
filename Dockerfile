#---------------------------------------------------------------------
# LAPS-Relay Docker image
#---------------------------------------------------------------------

# Build layer
FROM alpine:latest as builder

RUN apk update
RUN apk add --no-cache cmake alpine-sdk openssl-dev go perl
RUN apk add --no-cache tcsh bash linux-headers
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

WORKDIR /usr/local/cert
RUN openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
    -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=relay.quicr.ctgpoc.com" \
    -keyout server-key.pem -out server-cert.pem

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

COPY --chown=laps:laps --from=builder /usr/local/cert/server-cert.pem /usr/local/cert/
COPY --chown=laps:laps --from=builder /usr/local/cert/server-key.pem /usr/local/cert/


EXPOSE 33434/udp
EXPOSE 33435/udp
EXPOSE 33434/tcp

ENV LAPS_TLS_CERT_FILENAME=/usr/local/cert/server-cert.pem
ENV LAPS_TLS_KEY_FILENAME=/usr/local/cert/server-key.pem

CMD /usr/local/bin/lapsRelay

