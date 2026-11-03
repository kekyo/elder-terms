#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../../.. && pwd)
vm="$root/.build/nfs-vm"
mkdir -p "$vm"
if ! podman image exists localhost/elder-plan17-nfs-vm; then
  podman build -t localhost/elder-plan17-nfs-vm -f Containerfile .
fi
if [[ ! -f "$vm/base.ext4" || guest-init.sh -nt "$vm/base.ext4" ]]; then
  container=$(podman create localhost/elder-plan17-nfs-vm)
  trap 'podman rm "$container" >/dev/null' EXIT
  podman export "$container" > "$vm/rootfs.tar"
  mkdir -p "$vm/rootfs"
  podman unshare tar -xf "$vm/rootfs.tar" -C "$vm/rootfs" --exclude=dev
  mkdir -p "$vm/rootfs/dev"
  cp guest-init.sh "$vm/rootfs/nfs-test-init"
  chmod +x "$vm/rootfs/nfs-test-init"
  [[ -f "$vm/id_ed25519" ]] || ssh-keygen -q -t ed25519 -N '' -f "$vm/id_ed25519"
  podman unshare bash -c 'mkdir -p "$1/rootfs/root/.ssh"; cp "$1/id_ed25519.pub" "$1/rootfs/root/.ssh/authorized_keys"; chmod 700 "$1/rootfs/root/.ssh"; chmod 600 "$1/rootfs/root/.ssh/authorized_keys"' bash "$vm"
  cp "$vm"/rootfs/boot/vmlinuz-* "$vm/kernel"
  cp "$vm"/rootfs/boot/initrd.img-* "$vm/initrd"
  rg '^CONFIG_NFSD_V2=y$' "$vm"/rootfs/boot/config-*
  # The guest's older kernel predates the orphan_file and metadata_csum_seed defaults.
  podman unshare mke2fs -q -t ext4 -O ^orphan_file,^metadata_csum_seed -d "$vm/rootfs" "$vm/base.ext4.tmp" 2G
  mv "$vm/base.ext4.tmp" "$vm/base.ext4"
  rm "$vm/rootfs.tar"
fi
printf '%s\n' "$vm"
