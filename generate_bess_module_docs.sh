#!/bin/bash
# This script assumes you have protoc-gen-doc installed in your homedir
# git clone https://github.com/estan/protoc-gen-doc.git
# and follow instructions from there.

PATH=$PATH:~/protoc-gen-doc
protoc --doc_out=bess,Modules.md:. module_msg.proto
cat Modules.md | sed 's/Command/\./g' | sed 's/Arg/()/g' >Modules.md.out
mv Modules.md.out Modules.md
