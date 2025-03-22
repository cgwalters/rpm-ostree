#!/bin/bash
set -xeuo pipefail

# First: a cross-arch rechunking
podman pull --arch=ppc64le quay.io/centos/centos-bootc:stream9
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v $(pwd):/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --from quay.io/centos/centos-bootc:stream9 --output containers-storage:localhost/chunked-ppc64le
podman rmi quay.io/centos/centos-bootc:stream9
skopeo inspect --raw --config containers-storage:localhost/chunked-ppc64le > config.json
podman rmi localhost/chunked-ppc64le
test $(jq -r .architecture < config.json) = "ppc64le"
echo "ok cross arch rechunking"

# Build a custom image, then rechunk it
podman build -t localhost/base -f Containerfile.test
tar -xzvf ../../install.tar
podman build -v $(pwd)/usr/bin:/ci -t localhost/builder -f Containerfile.builder
orig_created=$(sudo skopeo inspect --raw --config containers-storage:localhost/base | jq -r .created)
podman run --rm --privileged --security-opt=label=disable \
  -v /var/lib/containers:/var/lib/containers \
  -v /var/tmp:/var/tmp \
  -v $(pwd):/output \
  localhost/builder rpm-ostree compose build-chunked-oci --bootc --format-version=1 --max-layers 99 --from localhost/base --output containers-storage:localhost/chunked
skopeo inspect containers-storage:localhost/chunked

skopeo inspect --raw --config containers-storage:localhost/chunked > new-config.json
# Verify we propagated the creation date
new_created=$(jq -r .created < new-config.json)
# ostree only stores seconds, so canonialize the rfc3339 data to seconds
test "$(date --date="${orig_created}" --rfc-3339=seconds)" = "$(date --date="${new_created}" --rfc-3339=seconds)"
# Verify we propagated labels
test $(jq -r .Labels.testlabel < new-config.json) = "1"
echo "ok rechunking with labels"
