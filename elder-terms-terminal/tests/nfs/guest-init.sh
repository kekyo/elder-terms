#!/bin/sh
# Disposable test guest only: host exports and services are never touched.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mountpoint -q /proc || mount -t proc proc /proc
mountpoint -q /sys || mount -t sysfs sysfs /sys
mountpoint -q /dev || mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /run/sshd /run/rpcbind /run/lock /var/lib/nfs/rpc_pipefs
mount -t devpts devpts /dev/pts
mount -t tmpfs tmpfs /tmp
hostname elder-nfs-test
ip link set lo up
ip link set eth0 up
ip addr add 10.0.2.15/24 dev eth0
ip route add default via 10.0.2.2
modprobe nfsd
mount -t nfsd nfsd /proc/fs/nfsd
mount -t rpc_pipefs sunrpc /var/lib/nfs/rpc_pipefs
mkdir -p /srv/public/deep/inside /srv/other/public /srv/readonly /srv/restricted
chmod 777 /srv/public /srv/public/deep /srv/public/deep/inside /srv/other/public
chmod 700 /srv/restricted
printf 'plain\n' > /srv/public/plain.txt
printf 'UTF-8\n' > /srv/public/日本語表
printf 'CP932\n' > "/srv/public/$(printf '\223\372\226\173\214\352\225\134')"
printf 'EUC-JP\n' > "/srv/public/$(printf '\306\374\313\334\270\354\311\275')"
printf 'raw\n' > "/srv/public/$(printf '\377')"
printf 'punctuation\n' > '/srv/public/ ?#% '
printf 'NFC\n' > /srv/public/が
printf 'NFD\n' > /srv/public/が
printf 'nested\n' > /srv/public/deep/inside/payload.txt
ln -s .. /srv/public/deep/cycle
: > /srv/public/zero
cat > /etc/exports <<'EXPORTS'
/srv *(rw,sync,no_subtree_check,insecure,no_root_squash,fsid=0)
/srv/public *(rw,sync,no_subtree_check,insecure,no_root_squash)
/srv/other/public *(rw,sync,no_subtree_check,insecure,no_root_squash)
/srv/public/deep *(rw,sync,no_subtree_check,insecure,no_root_squash)
/srv/readonly *(ro,sync,no_subtree_check,insecure,no_root_squash)
/srv/restricted *(rw,sync,no_subtree_check,insecure,root_squash)
EXPORTS
rpcbind -w
rpc.mountd -p 20048
exportfs -ra
rpc.nfsd -V 2 -V 3 -V 4 -U 8
echo Y > /proc/fs/nfsd/v4_end_grace
/usr/sbin/sshd -o PermitRootLogin=prohibit-password -o PasswordAuthentication=no -o UsePAM=no
printf 'NFS_TEST_READY versions: '
cat /proc/fs/nfsd/versions
# PID 1 must remain alive and reap daemon children.
while :; do wait; sleep 3600; done
