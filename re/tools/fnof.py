import sys,re,bisect
fl=sorted(int(m,16) for m in re.findall(r'^;-+ sub_([0-9a-f]+)',open('re/tools/out/app.lst').read(),re.M))
for a in sys.argv[1:]:
    a=int(a,16); i=bisect.bisect_right(fl,a)-1; print(hex(a),'in sub_%x'%fl[i])
