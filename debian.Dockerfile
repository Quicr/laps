#---------------------------------------------------------------------
# LAPS-Relay Docker image
#---------------------------------------------------------------------

# Build layer
FROM debian:12-slim as builder

RUN apt-get update && apt-get install -y make openssl golang perl wget git cmake ca-certificates

WORKDIR /tmp

#RUN [[ "$(uname -m)" == "x86_64" ]] \
#    && export CMAKE_FILENAME=cmake-3.29.2-linux-x86_64.sh \
#    || export CMAKE_FILENAME=cmake-3.29.2-linux-aarch64.sh; \
#    wget https://cmake.org/files/v3.29/$CMAKE_FILENAME; \
#    /bin/sh ./$CMAKE_FILENAME --skip-license --prefix=/usr/local

WORKDIR /ws

COPY ./Makefile ./
COPY ./CMakeLists.txt ./
COPY ./version_config.h.in ./
COPY ./dependencies ./dependencies
COPY ./src ./src

ENV CFLAGS="-Wno-error=stringop-overflow"
ENV CXXFLAGS="-Wno-error=stringop-overflow"

RUN make all

RUN cp  build/src/lapsRelay  /usr/local/bin/.

WORKDIR /usr/local/cert
RUN openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
    -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=relay.quicr.ctgpoc.com" \
    -keyout server-key.pem -out server-cert.pem

# Run layer
FROM debian:11-slim
RUN apt-get install -y libstdc++ bash

COPY --from=builder /usr/local/bin/lapsRelay /usr/local/bin/.

RUN addgroup laps
RUN adduser --quiet --disabled-password --gecos "Laps User" --ingroup laps laps
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

