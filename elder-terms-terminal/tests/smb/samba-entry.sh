#!/bin/sh
set -eu
useradd --no-create-home elder
mkdir -p /share /outside
chown elder:elder /share
printf 'outside sentinel\n' >/outside/sentinel.txt
ln -s /outside /share/outside-link
printf '%s\n%s\n' "$ELDER_SMB_PASSWORD" "$ELDER_SMB_PASSWORD" | smbpasswd -s -a elder >/dev/null
unset ELDER_SMB_PASSWORD
cat >/etc/samba/smb.conf <<'EOF'
[global]
server role = standalone server
server min protocol = SMB2_02
server signing = mandatory
map to guest = never
load printers = no
disable spoolss = yes
log level = 5
logging = stdout
[probe]
path = /share
read only = no
valid users = elder
follow symlinks = no
[encrypted]
path = /share
read only = no
valid users = elder
smb encrypt = required
follow symlinks = no
[readonly]
path = /share
read only = yes
valid users = elder
EOF
exec smbd --foreground --no-process-group --debug-stdout
