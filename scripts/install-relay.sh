#!/bin/bash
#
# Install or update laps relay
#
# Files that should be in the user home directory:
#    setup.sh    -- This script
#    lapsRelay   -- Latest laps relay binary
#
# ARGS:
#    No args
#

# ----------------------------------------------------------------------------------
# Constants
#
INSTALL_DIR=/usr/local/laps
LAPS_UNIT_SERVICE_FILE=/etc/systemd/system/laps.service
LAPS_MDNS_SERVICE_FILE=/etc/avahi/services/moqt.service
USER=$(id -u)

# ----------------------------------------------------------------------------------
# Systemd laps service unit file contents
#
LAPS_UNIT_SERVICE=$(cat << END_LAPS_UNIT_SERVICE
#   LAPS Service

[Unit]
Description=Latency Aware Publish Subscriber (LAPS) relay
Documentation=https://github.com/quicr/laps
Wants=network-online.target
After=network.target
Requires=network-online.target


[Service]
Type=simple
ExecStart=${INSTALL_DIR}/lapsRelay
Nice=15
Restart=always
RestartSec=1s
KillMode=process
WorkingDirectory=/tmp
Environment="LAPS_DISABLE_SPLITHZ=1"
Environment="LAPS_TLS_CERT_FILENAME=${INSTALL_DIR}/server-cert.pem"
Environment="LAPS_TLS_KEY_FILENAME=${INSTALL_DIR}/server-key.pem"
Environment="LAPS_PEER_RELIABLE=1"
Environment="LAPS_CWIN_MIN_KB=4"
Environment="LAPS_DEBUG=1"
Environment="LAPS_PEERS=relay.us-west-2.quicr.ctgpoc.com"
Environment="LAPS_PEER_PORT=33439"
Environment="LAPS_PEER_ID=$(hostname)-$(hostid)"

#User=pi

[Install]
WantedBy=multi-user.target

END_LAPS_UNIT_SERVICE
)


MDNS_UNIT_SERVICE=$(cat << END_MDNS_UNIT_SERVICE
<?xml version="1.0" standalone='no'?><!--*-nxml-*-->
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">

<!-- Man 5 avahi.service for doc on this file -->

<service-group>

  <name replace-wildcards="yes">%h</name>

  <service>
    <type>_moqt._udp</type>
    <port>33437</port>
  </service>

  <service>
    <type>_moqt._quic</type>
    <port>33437</port>
  </service>

</service-group>
END_MDNS_UNIT_SERVICE
)

# ----------------------------------------------------------------------------------
# Functions
#
update_systemd_unit_service() {

  if [[ -f $LAPS_UNIT_SERVICE_FILE ]]; then
    echo "Shutting down lapsRelay"
    sudo systemctl stop laps.service
  fi

  echo "Updating systemd unit file $LAPS_UNIT_SERVICE_FILE"

  sudo rm -f /tmp/laps-service.tmp
  echo "$LAPS_UNIT_SERVICE" > /tmp/laps-service.tmp
  sudo mv -f /tmp/laps-service.tmp $LAPS_UNIT_SERVICE_FILE

  sudo systemctl enable laps.service

  sudo systemctl daemon-reload
}

upgrade_laps() {
  if [[ -f ~/lapsRelay ]]; then
    echo "Copying ~/lapsRelay to $INSTALL_DIR/lapsRelay"

    sudo cp ~/lapsRelay $INSTALL_DIR/lapsRelay
  else
    echo "Missing ~/lapsRelay, no upgrade/install of laps relay"
  fi
}

create_cert() {
  cd $INSTALL_DIR

  if [[ ! -f server-key.pem || ! -f server-cert.pem ]]; then
    echo "Copying ~/lapsRelay to $INSTALL_DIR/lapsRelay"
    sudo cp ~/lapsRelay $INSTALL_DIR/lapsRelay
  fi
}

update_mdns() {
  sudo rm -f /tmp/laps-mdns-service.tmp
  echo "$LAPS_MDNS_UNIT_SERVICE" > /tmp/laps-mdns-service.tmp
  sudo mv -f /tmp/laps-mdns-service.tmp  $LAPS_MDNS_SERVICE_FILE

  sudo systemctl enable avahi.service
  
}

## Main function
main() {

  if [[ ! -d $INSTALL_DIR ]]; then
    echo "Making $INSTALL_DIR"
    sudo mkdir $INSTALL_DIR
  fi

  sudo chown -R $USER $INSTALL_DIR

  update_systemd_unit_service # Must be first so that it shutdown existing service

  upgrade_laps
  create_cert
  setup_mdns

  sudo chown -R $USER $INSTALL_DIR

  sudo systemctl start laps.service

  sudo systemctl status laps.service

  sudo sytemctl restart avahi.service 

  echo ""
  echo "RUN 'systemctl status laps.service' to get status of service"
  echo "RUN 'journalctl -u laps.service -f' to tail laps relay log"
}


main
