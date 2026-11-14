#!/usr/bin/env bash
# Copies the ingest secret into the iMac's Docker volume `lle-secrets` without
# printing it: readable only by the container user (uid 10001).
#   ssh imac-ts 'bash -s' < tools/imac-secret.sh   (with the secret at ~/1tb-drive/lle/secrets/ingest)
set -euo pipefail
export PATH=$PATH:/usr/local/bin:/opt/homebrew/bin
export DOCKER_HOST="unix:///Volumes/1tb drive/homelab-runtime/colima/default/docker.sock"
docker volume create lle-secrets >/dev/null
docker run --rm -i --user 0 --entrypoint sh -v lle-secrets:/s lle-live:arm64 \
  -c 'cat > /s/ingest && chown 10001 /s/ingest && chmod 400 /s/ingest && wc -c < /s/ingest' < ~/1tb-drive/lle/secrets/ingest
