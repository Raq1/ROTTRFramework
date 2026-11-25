import struct, re, sys
def cstr(b,o): return b[o:b.index(b'\0',o)].decode('latin1')
def rdef(blob):
    n=struct.unpack_from('<I',blob,28)[0]
    out={}
    for i in range(n):
        off=struct.unpack_from('<I',blob,32+4*i)[0]; tag=blob[off:off+4]; size=struct.unpack_from('<I',blob,off+4)[0]
        out[tag]=blob[off+8:off+8+size]
    return out
def parse_rdef(r):
    ncb,cboff,nres,resoff,ver,flags=struct.unpack_from('<IIIIHH',r,0)
    vmaj=ver>>8 & 0xff
    res=[]
    for i in range(nres):
        o=resoff+i*32
        nameoff,typ,ret,dim,samples,bind,count,fl=struct.unpack_from('<8I',r,o)
        res.append((cstr(r,nameoff),typ,dim,bind,count))
    cbs=[]
    for i in range(ncb):
        o=cboff+i*24
        nameoff,nvar,varoff,size,fl,typ=struct.unpack_from('<6I',r,o)
        vars=[]
        vstride=40 if vmaj>=5 else 24
        for v in range(nvar):
            vo=varoff+v*vstride
            vn,start,vsize,vfl,vtype,defv=struct.unpack_from('<6I',r,vo)
            vars.append((cstr(r,vn),start,vsize,vfl))
        cbs.append((cstr(r,nameoff),size,vars))
    return res,cbs
def shaders(data):
    return [m.start() for m in re.finditer(b'DXBC',data)]
def sig(chunk):
    n,_=struct.unpack_from('<II',chunk,0); out=[]
    for i in range(n):
        o=8+i*24; nameoff,idx,sv,ct,reg,mask=struct.unpack_from('<IIIIIB',chunk,o)
        out.append((cstr(chunk,nameoff),idx,reg,mask))
    return out
if __name__=='__main__':
    d=open(sys.argv[1],'rb').read()
    print('size',len(d),'head',d[:64].hex())
    for o in shaders(d):
        size=struct.unpack_from('<I',d,o+24)[0]; blob=d[o:o+size]; ch=rdef(blob)
        res,cbs=parse_rdef(ch[b'RDEF'])
        shex=ch.get(b'SHEX') or ch.get(b'SHDR'); prog=struct.unpack_from('<I',shex,0)[0]
        print('== shader at',o,'size',size,'type', 'PS' if (prog>>16)==0 else 'VS' if (prog>>16)==1 else prog>>16, 'pre-header', d[o-16:o].hex())
        print('  in ',sig(ch[b'ISGN'])); print('  out',sig(ch[b'OSGN']))
        for r in res: print('  res',r)
        for name,sz,vars in cbs:
            print('  cb',name,sz, [(v[0],v[1],v[2]) for v in vars if v[3]&2] if len(sys.argv)<3 else vars)
