#!/bin/bash
# Prepares the VPS to run the online server's core (server.py --role core) - see
# vps_cutover.md and docs/ONLINE_VPS_PLAN.md. Nothing goes live: the systemd unit
# is installed DISABLED and the nginx location is written but not included anywhere.
#
# Run as root from the port/server folder of a checkout of the release you want
# (copied to the VPS, e.g. made on the PC with
#   git archive --format=tar.gz -o svr2011-server.tar.gz <tag> port/server
# then on the VPS: tar xzf svr2011-server.tar.gz && cd port/server):
#   sudo bash vps_install.sh --dry-run      (prints what it would do, changes nothing)
#   sudo bash vps_install.sh
# Running it again updates the server files and the venv and keeps the keys.
# Undo: sudo bash vps_uninstall.sh
#
# It never touches WireGuard (no wg / wg-quick), firewalld (no ports, no --reload,
# the masquerade stays) or the running relay edge (svr-relay-edge).
set -euo pipefail

dry=0
src="$(cd "$(dirname "$0")" && pwd)"
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) dry=1 ;;
        --src) src="$(cd "$2" && pwd)"; shift ;;
        -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
        *) echo "unknown option $1 (--dry-run, --src DIR)"; exit 1 ;;
    esac
    shift
done

app=/opt/svr2011/app
venv=/opt/svr2011/venv
data=/var/lib/svr2011
logs=/var/log/svr2011
etc=/etc/svr2011
unit=/etc/systemd/system/svr2011.service
ngx_dir=/etc/nginx/svr2011
ngx_file=$ngx_dir/svr-location.conf
files="server.py admin_auth.py gamespy.py leaderboards.py match_names.py media.py relay.py relay_link.py thumbs.py lb_reset.py
       signup.py namefilter.py discovery.py reports.py dashboard.html leaderboards.json requirements.txt"
pw_tool=/usr/local/sbin/svr2011-admin-password

# run CMD...: does it (or prints it with --dry-run)
run() {
    if [ $dry = 1 ]; then echo "  + $*"; else "$@"; fi
}
# put PATH MODE OWNER: stdin into PATH (atomically; printed with --dry-run)
put() {
    if [ $dry = 1 ]; then
        echo "  + write $1 (mode $2, owner $3):"; sed 's/^/      | /'
        return
    fi
    local tmp; tmp=$(mktemp "$1.XXXXXX")
    cat > "$tmp"
    chmod "$2" "$tmp"; chown "$3" "$tmp"
    mv -f "$tmp" "$1"
}

[ $dry = 1 ] && echo "DRY RUN - nothing is changed"
if [ $dry = 0 ] && [ "$(id -u)" != 0 ]; then echo "run it as root (sudo)"; exit 1; fi
for f in $files; do
    [ -f "$src/$f" ] || { echo "missing $src/$f - run it from port/server (or --src DIR)"; exit 1; }
done
python3 -c 'import sys; sys.exit(sys.version_info < (3, 9))' || { echo "python3 3.9 or newer is needed"; exit 1; }
python3 -c 'import venv' 2>/dev/null || { echo "python3's venv module is missing (dnf install python3)"; exit 1; }

echo "== user: svr2011"
if id svr2011 >/dev/null 2>&1; then
    echo "  (there)"
else
    run useradd --system --home-dir "$data" --no-create-home --shell /sbin/nologin svr2011
fi

echo "== folders"
run install -d -m 755 -o root -g root /opt/svr2011 "$app"
run install -d -m 750 -o svr2011 -g svr2011 "$data" "$logs"
run install -d -m 750 -o root -g svr2011 "$etc"

echo "== server files: $app (from $src)"
for f in $files; do
    run install -m 644 -o root -g root "$src/$f" "$app/$f"
done

echo "== venv: $venv"
if [ -x "$venv/bin/python" ]; then
    echo "  (there)"
else
    run python3 -m venv "$venv"
fi
run "$venv/bin/pip" install --quiet --disable-pip-version-check -r "$app/requirements.txt"

echo "== keys: $etc"
if [ -s "$etc/storage_key" ]; then
    echo "  storage_key: kept"
else
    # (the PC's storage node gets the same value: pc_storage_setup.ps1)
    if [ $dry = 1 ]; then echo "  + generate $etc/storage_key (64 hex digits, mode 600)"; else
        python3 -c 'import secrets; print(secrets.token_hex(32))' | put "$etc/storage_key" 600 root:root; fi
fi
if [ -s "$etc/relay_secret.txt" ]; then
    echo "  relay_secret.txt: kept"
elif [ -s /opt/svr-relay/secret.txt ]; then
    # the relay edge's secret (the core signs the edge's tickets with it)
    run install -m 600 -o root -g root /opt/svr-relay/secret.txt "$etc/relay_secret.txt"
else
    echo "  WARNING: no /opt/svr-relay/secret.txt - put the edge's secret in $etc/relay_secret.txt (mode 600)"
fi

echo "== systemd: svr2011.service (installed, NOT enabled or started)"
# The relay edge's link stays where it is (10.8.0.1:36501 - the VPS's own tunnel
# address, so the edge needs no change). Secrets come in as credentials (%d).
# The dashboard, two ways: https://<host>/svr-admin/ with its own sign-in (admin_auth.py:
# the password set with svr2011-admin-password; only a valid admin session is an admin
# there), and --local-admin: the old routes (/, /api/admin/...) for direct loopback
# connections only (an SSH tunnel, ssh -L 8101:127.0.0.1:8101) - everything through nginx
# carries X-Forwarded-For (svr-location.conf sets it), which is never an admin by itself,
# and the old routes are outside /svr/ and /svr-admin/ so nginx doesn't pass them at all.
put "$unit" 644 root:root <<EOF
[Unit]
Description=SvR2011 online server (core: accounts, Community Creations, relay)
After=network-online.target wg-quick@wg0.service svr-relay-edge.service
Wants=network-online.target

[Service]
User=svr2011
Group=svr2011
WorkingDirectory=$app
LoadCredential=relay_secret:$etc/relay_secret.txt
LoadCredential=storage_key:$etc/storage_key
ExecStart=$venv/bin/python $app/server.py --supervise --role core --host 127.0.0.1 --port 8101 --base /svr \\
    --data $data --log $logs/server.log --max-connections 100 --max-file-mb 8 --quota-mb 256 \\
    --relay-udp-port 36500 --relay-edge 10.8.0.1:36501 --relay-secret %d/relay_secret \\
    --storage-url http://10.8.0.2:8102 --storage-key %d/storage_key --local-admin
Restart=always
RestartSec=3
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=$data $logs

[Install]
WantedBy=multi-user.target
EOF
run systemctl daemon-reload
if [ $dry = 0 ]; then
    echo "  state: $(systemctl is-enabled svr2011 2>/dev/null || true), $(systemctl is-active svr2011 2>/dev/null || true)"
fi

echo "== dashboard password: $pw_tool (run it once: sudo $pw_tool)"
put "$pw_tool" 755 root:root <<EOF
#!/bin/bash
# Sets the online dashboard's password (https://<host>/svr-admin/): asked twice, stored
# as a PBKDF2 hash in $data/admin_password (mode 600, the service's user). The running
# server reads it at each sign-in: no restart needed.
set -euo pipefail
[ "\$(id -u)" = 0 ] || { echo "run it as root (sudo)"; exit 1; }
exec runuser -u svr2011 -- $venv/bin/python $app/server.py --set-admin-password --data $data
EOF
if [ $dry = 0 ]; then
    if [ -s "$data/admin_password" ]; then echo "  password: set"; else echo "  password: NOT set yet"; fi
fi

echo "== nginx: $ngx_file (written, NOT included - vps_cutover.md switches it on)"
run install -d -m 755 -o root -g root "$ngx_dir"
put "$ngx_file" 644 root:root <<'EOF'
# SvR2011 online server (core on 127.0.0.1:8101). Include this inside the
# sho-ti.me server { } block, BEFORE the portal's catch-all location:
#     include /etc/nginx/svr2011/svr-location.conf;
# then: nginx -t && systemctl reload nginx

location /svr/ {
    proxy_pass http://127.0.0.1:8101;     # (no URI: /svr/... is passed as it is - the server's --base /svr)
    proxy_http_version 1.1;
    # the client's address, set (not appended) so it can't be forged
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    # the portal's admin headers: never from a client
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    # websockets and the relay's long streams (/svr/api/relay/...)
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $http_connection;
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 14400s;
    proxy_send_timeout 14400s;
    # uploads: Community Creations files (--max-file-mb 8)
    client_max_body_size 8m;
}

# The relay's upload (POST /svr/api/relay/<id>) is one stream for as long as
# the game is online: nginx counts a chunked body against client_max_body_size
# too, so /svr/'s 8m cut a long relayed session off at 8 MB. No limit here.
location /svr/api/relay/ {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection $http_connection;
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 14400s;
    proxy_send_timeout 14400s;
    client_max_body_size 0;
}

# The dashboard with its own sign-in (admin_auth.py): /svr-admin/... is passed as it is.
# There only a valid admin session is an admin - no header is (X-Forwarded-For is set
# here, the portal's headers emptied, as above). Five wrong passwords lock an address out.
location /svr-admin/ {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    client_max_body_size 1m;
    add_header X-Robots-Tag "noindex, nofollow" always;
}
location = /svr-admin {
    return 301 /svr-admin/;
}

# Discovery uploads: a .svrmod up to 250 MB and its four pictures (discovery.py)
location = /svr/api/mods {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 3600s;
    proxy_send_timeout 3600s;
    client_max_body_size 260m;
}
# ...and an owner's update of one (PUT /svr/api/mods/{id}/file: the same limit; GET downloads pass too)
location ~ ^/svr/api/mods/[0-9]+/file$ {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 3600s;
    proxy_send_timeout 3600s;
    client_max_body_size 260m;
}

# problem reports: a 25 MB zip, up to five 8 MB pictures, the match folder (5 MB) and the mods the check asked
# for (300 MB a report) - streamed to the core, which writes the mods to disk as they come (reports.py)
location = /svr/api/reports {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 1800s;
    proxy_send_timeout 1800s;
    client_max_body_size 400m;
}

# a player's answer to a report's question (POST /svr/api/reports/reply): text and up to three 8 MB pictures
location = /svr/api/reports/reply {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    proxy_request_buffering off;
    client_max_body_size 30m;
}

# entrance songs and movies are larger (--max-music-mb 512, --max-movie-mb 1024)
location = /svr/api/media {
    proxy_pass http://127.0.0.1:8101;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Forwarded-Proto $scheme;
    proxy_set_header X-Portal-Role "";
    proxy_set_header X-Portal-Public "";
    proxy_set_header X-Portal-User "";
    proxy_buffering off;
    proxy_request_buffering off;
    proxy_read_timeout 3600s;
    proxy_send_timeout 3600s;
    client_max_body_size 1100m;
}
EOF

echo "== SELinux"
if command -v getsebool >/dev/null 2>&1 && selinuxenabled 2>/dev/null; then
    if getsebool httpd_can_network_connect | grep -q -- '--> on'; then
        echo "  httpd_can_network_connect: on already"
    else
        run setsebool -P httpd_can_network_connect 1
    fi
    run restorecon -R /opt/svr2011 "$data" "$logs" "$etc" "$ngx_dir"
else
    echo "  (SELinux off or not installed)"
fi

echo "== ffmpeg (entrance songs and movies are re-encoded with it; optional)"
if command -v ffmpeg >/dev/null 2>&1; then echo "  $(command -v ffmpeg)"; else
    echo "  not installed - uploads are kept as sent. To add it: dnf install ffmpeg-free"; fi

echo
if [ $dry = 1 ]; then echo "dry run done - nothing was changed"; else
    echo "done. Nothing runs yet: the storage key for the PC is in $etc/storage_key"
    echo "(sudo cat it once, for pc_storage_setup.ps1), then follow vps_cutover.md"
fi
