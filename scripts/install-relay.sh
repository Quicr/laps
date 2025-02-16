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
AVAHI_SERVICE_FILE=/etc/avahi/services/laps.service
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
ExecStart=${INSTALL_DIR}/lapsRelay -d -e $(hostname)-$(hostid) -p 33435 -c ${INSTALL_DIR}/server-cert.pem -k ${INSTALL_DIR}/server-key.pem --peer_port 33436 --peer relay.m10x.org:33436
Nice=15
Restart=always
RestartSec=1s
KillMode=process
WorkingDirectory=/tmp

#User=pi

[Install]
WantedBy=multi-user.target

END_LAPS_UNIT_SERVICE
)

# ----------------------------------------------------------------------------------
# Functions
#
update_mdns() {
  echo "Updating MDNS Config"
  cat << END > /tmp/avahi_laps.service
<?xml version="1.0" standalone='no'?><!--*-nxml-*-->
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">%h</name>
  <service>
    <type>_laps._udp</type>
    <port>33435</port>
  </service>
 </service-group>
END

  sudo mv -f /tmp/avahi_laps.service $AVAHI_SERVICE_FILE
}

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

    update_mdns
  else
    echo "Missing ~/lapsRelay, no upgrade/install of laps relay"
  fi
}

create_cert() {
  cd $INSTALL_DIR

  if [[ ! -f server-key.pem || ! -f server-cert.pem ]]; then
    echo "Creating relay self-signed certificate"

    openssl req -nodes -x509 -newkey rsa:2048 -days 365 \
        -subj "/C=US/ST=CA/L=San Jose/O=Cisco/CN=relay.quicr.ctgpoc.com" \
        -keyout server-key.pem -out server-cert.pem
  fi
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

  sudo chown -R $USER $INSTALL_DIR

  sudo systemctl start laps.service

  sudo systemctl status laps.service

  echo ""
  echo "RUN 'systemctl status laps.service' to get status of service"
  echo "RUN 'journalctl -u laps.service -f' to tail laps relay log"
}


main