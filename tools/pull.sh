#!/bin/sh
# pull devkitarm image layers, keep only /opt/devkitpro
set -e
R=devkitpro/devkitarm
TOK=$(curl -sSf "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$R:pull" | python3 -c "import json,sys;print(json.load(sys.stdin)['token'])")
H="Authorization: Bearer $TOK"
M=$(curl -sSf -H "$H" -H "Accept: application/vnd.oci.image.manifest.v1+json" https://registry-1.docker.io/v2/$R/manifests/sha256:15b79ce75822c289538d8153da5fa7aafe5e6adc32ad8a575a197beca0f0761b)
mkdir -p layers root
for d in $(echo "$M" | python3 -c "import json,sys;[print(l['digest'],l['size']) for l in json.load(sys.stdin)['layers']]" | awk '{print $1}'); do
  f=layers/${d#sha256:}.tgz
  [ -f $f ] || curl -sSfL -H "$H" -o $f https://registry-1.docker.io/v2/$R/blobs/$d
  echo got $d $(stat -c%s $f)
  tar -xzf $f -C root --wildcards 'opt/devkitpro/*' 2>/dev/null || true
done
