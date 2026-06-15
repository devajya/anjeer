#!/usr/bin/env bash
set -euo pipefail

apt-get update
apt-get install -y --no-install-recommends \
    libpq5 libcurl4 libssl3 ca-certificates \
    nginx certbot python3-certbot-nginx

useradd --system --no-create-home --shell /usr/sbin/nologin anjeer || true

mkdir -p /opt/anjeer/config /opt/anjeer/db/migrations
chown -R anjeer:anjeer /opt/anjeer

cp "$(dirname "$0")/anjeer.service" /etc/systemd/system/anjeer.service
systemctl daemon-reload
systemctl enable anjeer

mkdir -p /etc/anjeer
if [ ! -f /etc/anjeer/env ]; then
    cat > /etc/anjeer/env <<'EOF'
ANJEER_DB_CONN=
ANJEER_JWT_SECRET=
ANJEER_GITHUB_CLIENT_ID=
ANJEER_GITHUB_CLIENT_SECRET=
ANJEER_GITHUB_REDIRECT_URI=
ANJEER_GOOGLE_CLIENT_ID=
ANJEER_GOOGLE_CLIENT_SECRET=
ANJEER_GOOGLE_REDIRECT_URI=
ANJEER_CORS_ORIGIN=
EOF
    chmod 600 /etc/anjeer/env
    echo "Created /etc/anjeer/env — fill in real values before starting the service."
fi
