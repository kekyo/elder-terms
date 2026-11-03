#!/bin/sh
set -eu
useradd --no-create-home elder
mkdir -p /share /empty /unicode
chown elder:elder /share /empty /unicode
printf 'before\n' >/unicode/before.txt
printf 'supplementary name\n' >/unicode/絵文字😀.txt
printf 'after\n' >/unicode/after.txt
printf '%s\n%s\n' "$ELDER_SMB_PASSWORD" "$ELDER_SMB_PASSWORD" | smbpasswd -s -a elder >/dev/null
unset ELDER_SMB_PASSWORD
cat >/etc/samba/smb.conf <<'EOF'
[global]
server role = standalone server
netbios name = ELDERTEST
server min protocol = NT1
server max protocol = NT1
server signing = disabled
map to guest = Bad User
load printers = no
disable spoolss = yes
log level = 3
logging = stdout
[probe]
path = /share
read only = no
valid users = elder
follow symlinks = no
[empty]
path = /empty
read only = no
valid users = elder
[日本語 space]
path = /unicode
read only = no
valid users = elder
[names]
path = /unicode
read only = yes
valid users = elder
[denied]
path = /share
valid users = root
EOF
if [ -n "${ELDER_SMB_PORT:-}" ]; then
  printf '\n[global]\nsmb ports = %s\n' "$ELDER_SMB_PORT" >>/etc/samba/smb.conf
fi
if [ "${ELDER_SMB_DENY_ENUMERATION:-0}" = 1 ]; then
  printf '\n[IPC$]\npath = /tmp\nvalid users = root\n' >>/etc/samba/smb.conf
fi
count=${ELDER_SMB_SHARE_COUNT:-0}
while [ "$count" -gt 0 ]; do
  printf '\n[bulk-%s]\npath = /empty\nread only = yes\nvalid users = elder\n' "$count" >>/etc/samba/smb.conf
  count=$((count - 1))
done
exec smbd --foreground --no-process-group --debug-stdout
