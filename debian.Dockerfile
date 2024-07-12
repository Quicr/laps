#---------------------------------------------------------------------
# LAPS-Relay Docker image
#---------------------------------------------------------------------

# Build layer
FROM debian:12-slim as builder

RUN apt-get update && apt-get install -y make openssl golang python3-venv wget git cmake ca-certificates

WORKDIR /ws

COPY ./Makefile ./
COPY ./CMakeLists.txt ./
COPY ./version_config.h.in ./
COPY ./dependencies ./dependencies
COPY ./src ./src

ENV CFLAGS="-Wno-error=stringop-overflow"
ENV CXXFLAGS="-Wno-error=stringop-overflow"

RUN make all

RUN cp  build/src/lapsRelay/lapsRelay  /usr/local/bin/. \
    && cp  build/src/lapsTest/lapsTest  /usr/local/bin/.

WORKDIR /usr/local/cert
RUN openssl ecparam -name prime256v1 -genkey -noout -out server-key-ec.pem
RUN openssl req -nodes -x509 -key server-key-ec.pem -days 365 \
            -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=relay.m10x.org" \
            -keyout server-key.pem -out server-cert.pem

# Run layer
FROM debian:12-slim
RUN apt-get install -y libstdc++ bash

COPY --from=builder /usr/local/bin/lapsRelay /usr/local/bin/.
COPY --from=builder /usr/local/bin/lapsTest /usr/local/bin/.

RUN addgroup laps
RUN adduser --quiet --disabled-password --gecos "Laps User" --ingroup laps laps
USER laps
WORKDIR /home/laps

COPY --chown=laps:laps --from=builder /usr/local/cert/server-cert.pem /usr/local/cert/
COPY --chown=laps:laps --from=builder /usr/local/cert/server-key.pem /usr/local/cert/


EXPOSE 33434/udp
EXPOSE 33435/udp
EXPOSE 33436/udp
EXPOSE 33437/udp
EXPOSE 33438/udp

ENV LAPS_TLS_CERT_FILENAME=/usr/local/cert/server-cert.pem
ENV LAPS_TLS_KEY_FILENAME=/usr/local/cert/server-key.pem

CMD /usr/local/bin/lapsRelay

