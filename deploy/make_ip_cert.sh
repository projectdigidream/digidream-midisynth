#!/usr/bin/env bash
# Self-signed 10-year certificate for a bare-IP origin (IP in the SAN --
# without it, browsers reject the cert even after clicking through).
#   ./make_ip_cert.sh 10.0.0.5
set -euo pipefail
IP=${1:?usage: make_ip_cert.sh <server-ip>}
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout /etc/nginx/digidreamsynth-ip.key \
  -out /etc/nginx/digidreamsynth-ip.crt \
  -subj "/CN=DigidreamSynth" \
  -addext "subjectAltName=IP:$IP"
chmod 600 /etc/nginx/digidreamsynth-ip.key
echo "cert ready: https://$IP:8443/"
