#!/bin/sh
# usage: fn.sh hexaddr...  -> print function(s) from out/app.lst
for a in "$@"; do awk -v t=";---------------- sub_$a " 'index($0,t)==1{p=1;print;next} /^;----------------/{p=0} p' re/tools/out/app.lst; done
