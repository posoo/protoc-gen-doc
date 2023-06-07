#!/bin/bash
set -euo pipefail

# this is required because of the wildcard expansion. Passing /protos/*.proto in CMD gets escaped -_-. So instead leaving
# off the [FILES] will put /protos/*.proto in from here which will expand correctly.
# P.S. `entrypoint.sh` is called from WORKDIR=/protos 
args=("$@")
if [ "${#args[@]}" -lt 2 ]; then args+=(*.proto); fi

exec protoc --doc_out=/out "${args[@]}"
