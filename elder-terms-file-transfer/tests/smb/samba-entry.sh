#!/bin/sh
set -eu
useradd --no-create-home elder
mkdir -p /share /outside
mkdir -p /empty /unicode
chown elder:elder /share
chown elder:elder /empty /unicode
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
cat >>/etc/samba/smb.conf <<'EOF'
[empty]
path = /empty
read only = no
valid users = elder
[日本語 space]
path = /unicode
read only = no
valid users = elder
[denied]
path = /share
valid users = root
[hidden$]
path = /share
read only = yes
valid users = elder
EOF
count=${ELDER_SMB_SHARE_COUNT:-0}
while [ "$count" -gt 0 ]; do
  printf '\n[bulk-%s]\npath = /empty\nread only = yes\nvalid users = elder\n' "$count" >>/etc/samba/smb.conf
  count=$((count - 1))
done
if [ "${ELDER_SMB_DENY_ENUMERATION:-0}" = 1 ]; then
  printf '\n[IPC$]\npath = /tmp\nvalid users = root\n' >>/etc/samba/smb.conf
fi
exec smbd --foreground --no-process-group --debug-stdout
