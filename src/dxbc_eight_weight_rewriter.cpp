#include "dxbc_eight_weight_rewriter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace DxbcEightWeight {
namespace {
constexpr std::uint32_t OP_FTOU = 28;
constexpr std::uint32_t OP_IADD = 30;
constexpr std::uint32_t OP_INEG = 40;
constexpr std::uint32_t OP_ISHL = 41;
constexpr std::uint32_t OP_MAD = 50;
constexpr std::uint32_t OP_MUL = 56;
constexpr std::uint32_t OP_ROUND_NE = 64;
constexpr std::uint32_t OP_DCL_INPUT = 95;
constexpr std::uint32_t OP_DCL_INPUT_LAST = 100;
constexpr std::uint32_t OP_DCL_TEMPS = 104;
constexpr std::uint32_t OP_DCL_INDEXABLE_TEMP = 105;
constexpr std::uint32_t OP_DCL_GLOBAL_FLAGS = 106;
constexpr std::uint32_t OP_CUSTOMDATA = 53;

std::uint32_t ReadU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
void WriteU32(std::vector<std::uint8_t>& out, std::size_t off, std::uint32_t v) {
    if (off + 4 > out.size()) throw std::runtime_error("write past buffer");
    std::memcpy(out.data() + off, &v, 4);
}
void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const auto old = out.size(); out.resize(old + 4); std::memcpy(out.data() + old, &v, 4);
}
void AppendBytes(std::vector<std::uint8_t>& out, const void* p, std::size_t n) {
    const auto old = out.size(); out.resize(old + n); if (n) std::memcpy(out.data() + old, p, n);
}

struct Operand;
struct IndexEntry {
    std::uint32_t repr = 0;
    std::uint64_t immediate = 0;
    std::unique_ptr<Operand> relative;
    IndexEntry() = default;
    IndexEntry(const IndexEntry& o);
    IndexEntry& operator=(const IndexEntry& o);
    IndexEntry(IndexEntry&&) noexcept = default;
    IndexEntry& operator=(IndexEntry&&) noexcept = default;
};
struct Operand {
    std::uint32_t token0 = 0;
    std::uint32_t type = 0;
    std::uint32_t numComponents = 0;
    std::uint32_t selectMode = 0;
    std::array<std::uint32_t,4> components{};
    std::uint32_t componentCount = 0;
    std::vector<std::uint32_t> extTokens;
    std::vector<IndexEntry> indices;
    std::vector<std::uint64_t> immediateValues;
    Operand() = default;
    Operand(const Operand& o) : token0(o.token0), type(o.type), numComponents(o.numComponents),
        selectMode(o.selectMode), components(o.components), componentCount(o.componentCount),
        extTokens(o.extTokens), indices(o.indices), immediateValues(o.immediateValues) {}
    Operand& operator=(const Operand& o) {
        if (this != &o) { token0=o.token0; type=o.type; numComponents=o.numComponents;
            selectMode=o.selectMode; components=o.components; componentCount=o.componentCount;
            extTokens=o.extTokens; indices=o.indices; immediateValues=o.immediateValues; }
        return *this;
    }
    int RegIndex() const {
        if (!indices.empty() && indices[0].repr == 0 && indices[0].immediate <= std::numeric_limits<std::uint32_t>::max())
            return static_cast<int>(indices[0].immediate);
        return -1;
    }
};
IndexEntry::IndexEntry(const IndexEntry& o) : repr(o.repr), immediate(o.immediate),
    relative(o.relative ? std::make_unique<Operand>(*o.relative) : nullptr) {}
IndexEntry& IndexEntry::operator=(const IndexEntry& o) {
    if (this != &o) { repr=o.repr; immediate=o.immediate;
        relative=o.relative ? std::make_unique<Operand>(*o.relative) : nullptr; }
    return *this;
}

Operand ParseOperand(const std::vector<std::uint32_t>& dw, std::size_t& p, std::size_t limit) {
    if (p >= limit) throw std::runtime_error("operand eof");
    Operand o; o.token0 = dw[p++];
    o.numComponents = o.token0 & 3u;
    if (o.numComponents == 2) {
        o.selectMode = (o.token0 >> 2) & 3u;
        if (o.selectMode == 0) {
            const auto mask = (o.token0 >> 4) & 0xFu;
            for (std::uint32_t i=0;i<4;++i) if (mask & (1u<<i)) o.components[o.componentCount++] = i;
        } else if (o.selectMode == 1) {
            const auto sw = (o.token0 >> 4) & 0xFFu;
            for (std::uint32_t i=0;i<4;++i) o.components[o.componentCount++] = (sw >> (2*i)) & 3u;
        } else if (o.selectMode == 2) {
            o.components[o.componentCount++] = (o.token0 >> 4) & 3u;
        }
    } else if (o.numComponents == 1) {
        o.components[o.componentCount++] = 0;
    }
    o.type = (o.token0 >> 12) & 0xFFu;
    const auto dim = (o.token0 >> 20) & 3u;
    std::array<std::uint32_t,3> reps{};
    for (std::uint32_t i=0;i<dim;++i) reps[i] = (o.token0 >> (22 + 3*i)) & 7u;
    if (o.token0 >> 31) {
        while (true) {
            if (p >= limit) throw std::runtime_error("operand ext eof");
            const auto e = dw[p++]; o.extTokens.push_back(e); if (!(e >> 31)) break;
        }
    }
    for (std::uint32_t i=0;i<dim;++i) {
        IndexEntry e; e.repr = reps[i];
        if (e.repr == 0) { if (p>=limit) throw std::runtime_error("idx eof"); e.immediate=dw[p++]; }
        else if (e.repr == 1) { if (p+1>=limit) throw std::runtime_error("idx64 eof"); e.immediate=dw[p] | (std::uint64_t(dw[p+1])<<32); p+=2; }
        else if (e.repr == 2) { e.relative=std::make_unique<Operand>(ParseOperand(dw,p,limit)); }
        else if (e.repr == 3) { if (p>=limit) throw std::runtime_error("idxrel eof"); e.immediate=dw[p++]; e.relative=std::make_unique<Operand>(ParseOperand(dw,p,limit)); }
        else if (e.repr == 4) { if (p+1>=limit) throw std::runtime_error("idx64rel eof"); e.immediate=dw[p] | (std::uint64_t(dw[p+1])<<32); p+=2; e.relative=std::make_unique<Operand>(ParseOperand(dw,p,limit)); }
        else throw std::runtime_error("unsupported index representation");
        o.indices.push_back(std::move(e));
    }
    if (o.type == 4) {
        const std::size_t count = o.numComponents==1 ? 1 : (o.numComponents==2 ? 4 : 0);
        if (p+count>limit) throw std::runtime_error("imm32 eof");
        for (std::size_t i=0;i<count;++i) o.immediateValues.push_back(dw[p++]);
    } else if (o.type == 5) {
        const std::size_t count = o.numComponents==1 ? 1 : (o.numComponents==2 ? 4 : 0);
        if (p+count*2>limit) throw std::runtime_error("imm64 eof");
        for (std::size_t i=0;i<count;++i) { o.immediateValues.push_back(dw[p] | (std::uint64_t(dw[p+1])<<32)); p+=2; }
    }
    if (p > limit) throw std::runtime_error("operand overflow");
    return o;
}

void SerializeOperand(const Operand& o, std::vector<std::uint32_t>& out) {
    out.push_back(o.token0);
    out.insert(out.end(), o.extTokens.begin(), o.extTokens.end());
    for (const auto& e : o.indices) {
        if (e.repr == 0) out.push_back(static_cast<std::uint32_t>(e.immediate));
        else if (e.repr == 1) { out.push_back(static_cast<std::uint32_t>(e.immediate)); out.push_back(static_cast<std::uint32_t>(e.immediate>>32)); }
        else if (e.repr == 2) { if (!e.relative) throw std::runtime_error("missing rel"); SerializeOperand(*e.relative,out); }
        else if (e.repr == 3) { out.push_back(static_cast<std::uint32_t>(e.immediate)); if (!e.relative) throw std::runtime_error("missing rel"); SerializeOperand(*e.relative,out); }
        else if (e.repr == 4) { out.push_back(static_cast<std::uint32_t>(e.immediate)); out.push_back(static_cast<std::uint32_t>(e.immediate>>32)); if (!e.relative) throw std::runtime_error("missing rel"); SerializeOperand(*e.relative,out); }
    }
    if (o.type == 4) for (auto v:o.immediateValues) out.push_back(static_cast<std::uint32_t>(v));
    else if (o.type == 5) for (auto v:o.immediateValues) { out.push_back(static_cast<std::uint32_t>(v)); out.push_back(static_cast<std::uint32_t>(v>>32)); }
}
void SetReg(Operand& o, std::uint32_t reg) {
    if (o.indices.empty() || o.indices[0].repr != 0) throw std::runtime_error("non-direct register");
    o.indices[0].immediate = reg;
}
void ReplaceRelativeTemp(Operand& o, std::uint32_t oldReg, std::uint32_t newReg) {
    if (o.type == 0 && !o.indices.empty() && o.indices[0].repr==0 && o.indices[0].immediate==oldReg)
        o.indices[0].immediate=newReg;
    for (auto& e:o.indices) if (e.relative) ReplaceRelativeTemp(*e.relative,oldReg,newReg);
}
int FindCbRelativeTemp(const Operand& o) {
    if (o.type != 8 || o.indices.size()<2) return -1;
    const auto& e=o.indices[1];
    if ((e.repr==2 || e.repr==3 || e.repr==4) && e.relative && e.relative->type==0 &&
        !e.relative->indices.empty() && e.relative->indices[0].repr==0)
        return static_cast<int>(e.relative->indices[0].immediate);
    return -1;
}
Operand MakeTemp(std::uint32_t reg, bool dest) {
    Operand o;
    if (dest) o.token0 = 2u | 0xF0u | (1u<<20);
    else { const std::uint32_t sw=0u|(1u<<2)|(2u<<4)|(3u<<6); o.token0=2u|(1u<<2)|(sw<<4)|(1u<<20); }
    o.type=0; o.numComponents=2; o.selectMode=dest?0:1; o.componentCount=4; o.components={0,1,2,3};
    IndexEntry e; e.repr=0; e.immediate=reg; o.indices.push_back(std::move(e)); return o;
}
std::vector<std::uint32_t> MakeInstr(std::uint32_t opcode, const std::vector<Operand>& ops) {
    std::vector<std::uint32_t> body; for (const auto& o:ops) SerializeOperand(o,body);
    std::vector<std::uint32_t> out; out.reserve(body.size()+1); out.push_back((static_cast<std::uint32_t>(body.size()+1)<<24)|opcode); out.insert(out.end(),body.begin(),body.end()); return out;
}

struct Instruction { std::size_t off=0; std::uint32_t opcode=0; std::vector<std::uint32_t> tokens; std::vector<Operand> operands; };
std::vector<Instruction> ParseInstructions(const std::vector<std::uint32_t>& dw) {
    std::vector<Instruction> result; std::size_t p=2;
    while (p<dw.size()) {
        const auto t=dw[p], op=t&0x7FFu;
        std::size_t n=0;
        if (op==OP_CUSTOMDATA) { if (p+1>=dw.size()) throw std::runtime_error("customdata eof"); n=dw[p+1]; }
        else n=(t>>24)&0x7Fu;
        if (!n || p+n>dw.size()) throw std::runtime_error("bad instruction length");
        Instruction ins; ins.off=p; ins.opcode=op; ins.tokens.assign(dw.begin()+p,dw.begin()+p+n);
        if (op!=OP_CUSTOMDATA && op!=OP_DCL_TEMPS && op!=OP_DCL_INDEXABLE_TEMP && op!=OP_DCL_GLOBAL_FLAGS) {
            try {
                std::size_t q=p+1;
                if (t>>31) { do { if(q>=p+n) throw std::runtime_error("ext opcode eof"); } while (dw[q++]>>31); }
                while (q<p+n) ins.operands.push_back(ParseOperand(dw,q,p+n));
                if (q!=p+n) ins.operands.clear();
            } catch (...) { ins.operands.clear(); }
        }
        result.push_back(std::move(ins)); p+=n;
    }
    return result;
}

struct Chunk { std::array<char,4> tag{}; std::vector<std::uint8_t> data; };
std::vector<Chunk> ParseChunks(const std::uint8_t* data, std::size_t size) {
    if (size<32 || std::memcmp(data,"DXBC",4)!=0) throw std::runtime_error("not DXBC");
    const auto total=ReadU32(data+24), count=ReadU32(data+28);
    if (total>size || count>64 || 32ull+4ull*count>size) throw std::runtime_error("bad DXBC header");
    std::vector<Chunk> out; out.reserve(count);
    for (std::uint32_t i=0;i<count;++i) {
        const auto off=ReadU32(data+32+4*i);
        if (std::uint64_t(off)+8>size) throw std::runtime_error("bad chunk offset");
        const auto len=ReadU32(data+off+4);
        if (std::uint64_t(off)+8+len>size) throw std::runtime_error("bad chunk size");
        Chunk c; std::memcpy(c.tag.data(),data+off,4); c.data.assign(data+off+8,data+off+8+len); out.push_back(std::move(c));
    }
    return out;
}
Chunk* FindChunk(std::vector<Chunk>& chunks, const char tag[5]) {
    for (auto& c:chunks) if (std::memcmp(c.tag.data(),tag,4)==0) return &c; return nullptr;
}

struct SignatureRecord { std::string name; std::uint32_t semidx=0, sysval=0, ctype=0, reg=0, maskrw=0; };
std::vector<SignatureRecord> ParseSignature(const std::vector<std::uint8_t>& d) {
    if (d.size()<8) throw std::runtime_error("short ISGN");
    const auto n=ReadU32(d.data()); if (8ull+24ull*n>d.size()) throw std::runtime_error("bad ISGN count");
    std::vector<SignatureRecord> out; out.reserve(n);
    for(std::uint32_t i=0;i<n;++i) {
        const auto b=8+24*i, no=ReadU32(d.data()+b);
        if(no>=d.size()) throw std::runtime_error("bad semantic offset");
        const char* s=reinterpret_cast<const char*>(d.data()+no); const char* end=reinterpret_cast<const char*>(d.data()+d.size()); const char* z=std::find(s,end,'\0'); if(z==end) throw std::runtime_error("unterminated semantic");
        SignatureRecord r; r.name.assign(s,z); r.semidx=ReadU32(d.data()+b+4); r.sysval=ReadU32(d.data()+b+8); r.ctype=ReadU32(d.data()+b+12); r.reg=ReadU32(d.data()+b+16); r.maskrw=ReadU32(d.data()+b+20); out.push_back(std::move(r));
    }
    return out;
}
std::vector<std::uint8_t> BuildSignature(const std::vector<SignatureRecord>& recs, std::uint32_t unknown) {
    std::vector<std::uint8_t> out(8+24*recs.size()); WriteU32(out,0,static_cast<std::uint32_t>(recs.size())); WriteU32(out,4,unknown);
    std::map<std::string,std::uint32_t> offsets;
    const auto stringBase=static_cast<std::uint32_t>(out.size());
    for (const auto& r:recs) if (!offsets.count(r.name)) { offsets[r.name]=static_cast<std::uint32_t>(out.size()); AppendBytes(out,r.name.data(),r.name.size()); out.push_back(0); }
    (void)stringBase;
    for (std::size_t i=0;i<recs.size();++i) {
        const auto b=8+24*i; const auto& r=recs[i];
        WriteU32(out,b,offsets[r.name]); WriteU32(out,b+4,r.semidx); WriteU32(out,b+8,r.sysval); WriteU32(out,b+12,r.ctype); WriteU32(out,b+16,r.reg); WriteU32(out,b+20,r.maskrw);
    }
    while(out.size()%4) out.push_back(0xAB); return out;
}

constexpr std::array<std::uint32_t,64> kMd5K{
0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
constexpr std::array<std::uint32_t,64> kMd5S{7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
std::uint32_t Rol(std::uint32_t x,std::uint32_t n){return (x<<n)|(x>>(32-n));}
void Md5Transform(std::array<std::uint32_t,4>& state,const std::uint8_t* block){
    std::array<std::uint32_t,16> m{}; for(int i=0;i<16;++i)m[i]=ReadU32(block+4*i);
    auto a=state[0],b=state[1],c=state[2],d=state[3]; const auto old=state;
    for(std::uint32_t i=0;i<64;++i){std::uint32_t f=0,g=0;if(i<16){f=(b&c)|((~b)&d);g=i;}else if(i<32){f=(d&b)|((~d)&c);g=(5*i+1)%16;}else if(i<48){f=b^c^d;g=(3*i+5)%16;}else{f=c^(b|(~d));g=(7*i)%16;}auto na=d;d=c;c=b;b=b+Rol(a+f+kMd5K[i]+m[g],kMd5S[i]);a=na;}
    state={old[0]+a,old[1]+b,old[2]+c,old[3]+d};
}
std::array<std::uint8_t,16> DxbcChecksum(const std::vector<std::uint8_t>& blob){
    if(blob.size()<20) throw std::runtime_error("short checksum input"); const auto* data=blob.data()+20; const std::size_t len=blob.size()-20; const std::uint64_t nbits=std::uint64_t(len)*8;
    std::array<std::uint32_t,4> state{0x67452301,0xefcdab89,0x98badcfe,0x10325476}; const std::size_t full=(len/64)*64;
    for(std::size_t i=0;i<full;i+=64) Md5Transform(state,data+i); const std::size_t rem=len-full;
    if(rem>=56){std::array<std::uint8_t,64> block{};std::memcpy(block.data(),data+full,rem);block[rem]=0x80;Md5Transform(state,block.data());std::array<std::uint8_t,64> words{};std::uint32_t w0=static_cast<std::uint32_t>(nbits),w15=static_cast<std::uint32_t>((nbits>>2)|1);std::memcpy(words.data(),&w0,4);std::memcpy(words.data()+60,&w15,4);Md5Transform(state,words.data());}
    else{std::array<std::uint8_t,64> buf{};std::uint32_t w0=static_cast<std::uint32_t>(nbits),w15=static_cast<std::uint32_t>((nbits>>2)|1);std::memcpy(buf.data(),&w0,4);std::memcpy(buf.data()+4,data+full,rem);buf[4+rem]=0x80;std::memcpy(buf.data()+60,&w15,4);Md5Transform(state,buf.data());}
    std::array<std::uint8_t,16> out{};std::memcpy(out.data(),state.data(),16);return out;
}

std::vector<std::uint8_t> RebuildDxbc(const std::uint8_t* original, std::size_t originalSize, std::vector<Chunk>& chunks) {
    const auto count=static_cast<std::uint32_t>(chunks.size()); std::vector<std::uint8_t> out(32+4*count); std::memcpy(out.data(),"DXBC",4); std::memcpy(out.data()+20,original+20,4); WriteU32(out,28,count);
    std::vector<std::uint32_t> offsets; offsets.reserve(count);
    for(auto& c:chunks){while(out.size()%4)out.push_back(0);offsets.push_back(static_cast<std::uint32_t>(out.size()));AppendBytes(out,c.tag.data(),4);AppendU32(out,static_cast<std::uint32_t>(c.data.size()));AppendBytes(out,c.data.data(),c.data.size());}
    WriteU32(out,24,static_cast<std::uint32_t>(out.size()));for(std::uint32_t i=0;i<count;++i)WriteU32(out,32+4*i,offsets[i]);std::fill(out.begin()+4,out.begin()+20,0);auto sum=DxbcChecksum(out);std::copy(sum.begin(),sum.end(),out.begin()+4);(void)originalSize;return out;
}

bool IsDirectMaskedTempDest(const Operand& o) {
    if (o.type != 0 || o.RegIndex() < 0 || o.numComponents != 2 || o.selectMode != 0 ||
        o.componentCount == 0 || o.componentCount > 4)
        return false;
    std::uint32_t seen = 0;
    for (std::uint32_t i = 0; i < o.componentCount; ++i) {
        if (o.components[i] > 3 || (seen & (1u << o.components[i])))
            return false;
        seen |= 1u << o.components[i];
    }
    return true;
}

} 

bool Checksum(const void* blob, std::size_t size, std::uint8_t out[16]) {
    if (!blob || size < 20) return false;
    try {
        const auto* bytes = static_cast<const std::uint8_t*>(blob);
        const std::vector<std::uint8_t> copy(bytes, bytes + size);
        const auto sum = DxbcChecksum(copy);
        std::memcpy(out, sum.data(), 16);
        return true;
    } catch (...) {
        return false;
    }
}

bool Rewrite(const void* source, std::size_t size, std::vector<std::uint8_t>& output, RewriteInfo* outInfo, std::string* error) {
    output.clear();
    try {
        if (!source || size<32) throw std::runtime_error("empty/short shader");
        const auto* data=static_cast<const std::uint8_t*>(source);
        auto chunks=ParseChunks(data,size); auto* isgn=FindChunk(chunks,"ISGN"); auto* shex=FindChunk(chunks,"SHEX"); if(!shex) shex=FindChunk(chunks,"SHDR"); // SHDR: shader model 4 bytecode, same token format
        if(!isgn||!shex) throw std::runtime_error("shader lacks ISGN or SHEX/SHDR");
        auto sig=ParseSignature(isgn->data); if(sig.empty()) throw std::runtime_error("empty input signature");
        int lowW=-1,lowI=-1;std::uint32_t maxInput=0;for(const auto&r:sig){maxInput=std::max(maxInput,r.reg);if(r.name=="BLENDWEIGHT"&&r.semidx==0)lowW=static_cast<int>(r.reg);if(r.name=="BLENDINDICES"&&r.semidx==0)lowI=static_cast<int>(r.reg);}if(lowW<0||lowI<0)throw std::runtime_error("no BLENDWEIGHT0/BLENDINDICES0");
        const std::uint32_t highW=maxInput+1, highI=maxInput+2;

        if(shex->data.size()%4)throw std::runtime_error("unaligned shader code chunk");std::vector<std::uint32_t> dw(shex->data.size()/4);std::memcpy(dw.data(),shex->data.data(),shex->data.size());if(dw.size()<2||dw[1]!=dw.size())throw std::runtime_error("bad shader code length");auto ins=ParseInstructions(dw);
        Instruction* tempDecl=nullptr;for(auto&x:ins)if(x.opcode==OP_DCL_TEMPS){tempDecl=&x;break;}if(!tempDecl||tempDecl->tokens.size()<2)throw std::runtime_error("no dcl_temps");const auto oldTemps=tempDecl->tokens[1];
        struct Weighted{std::size_t off;Operand weight;Operand cb;std::uint32_t rel;};std::vector<Weighted> weighted;std::vector<std::uint32_t> indexRegs;
        for(const auto&x:ins){if((x.opcode!=OP_MUL&&x.opcode!=OP_MAD)||x.operands.empty())continue;const Operand* wo=nullptr;const Operand* cb=nullptr;for(const auto&o:x.operands){if(o.type==1&&o.RegIndex()==lowW)wo=&o;if(o.type==8&&!cb)cb=&o;}if(wo&&cb){const int rel=FindCbRelativeTemp(*cb);if(rel<0)throw std::runtime_error("weighted CB has unfamiliar relative index");weighted.push_back({x.off,*wo,*cb,static_cast<std::uint32_t>(rel)});if(std::find(indexRegs.begin(),indexRegs.end(),rel)==indexRegs.end())indexRegs.push_back(rel);}}
        if(weighted.empty())throw std::runtime_error("no recognizable skinning accumulation");const auto firstWeight=weighted.front().off;
        const Instruction* idxMul=nullptr;for(const auto&x:ins){if(x.off>=firstWeight)break;if(x.opcode==OP_MUL&&!x.operands.empty())for(const auto&o:x.operands)if(o.type==1&&o.RegIndex()==lowI){idxMul=&x;break;}}
        if(!idxMul)throw std::runtime_error("no stock blend-index scale");const Operand* imm255=nullptr;const Operand* lowIOp=nullptr;for(const auto&o:idxMul->operands){if(o.type==4&&!imm255)imm255=&o;if(o.type==1&&o.RegIndex()==lowI)lowIOp=&o;}if(!imm255||!lowIOp)throw std::runtime_error("blend-index scale operands missing");
        const Instruction* shift=nullptr;const Operand* imm2=nullptr;for(const auto&x:ins){if(x.off>=firstWeight)break;if(x.opcode==OP_ISHL){for(const auto&o:x.operands)if(o.type==4){shift=&x;imm2=&o;break;}if(shift)break;}}if(!imm2)throw std::runtime_error("no skin index shift");
        const auto rHi=oldTemps,rLow=oldTemps+1;std::map<std::uint32_t,std::uint32_t> hiMap;for(std::size_t i=0;i<indexRegs.size();++i)hiMap[indexRegs[i]]=oldTemps+2+static_cast<std::uint32_t>(i);const auto newTemps=oldTemps+2+static_cast<std::uint32_t>(indexRegs.size());
        auto inputClone=[](const Operand&o,std::uint32_t r){Operand c=o;SetReg(c,r);return c;};
        std::vector<std::uint32_t> setup;auto emit=[&](std::uint32_t op,std::initializer_list<Operand> ops){auto t=MakeInstr(op,std::vector<Operand>(ops));setup.insert(setup.end(),t.begin(),t.end());};
        emit(OP_MUL,{MakeTemp(rHi,true),inputClone(*lowIOp,highI),*imm255});emit(OP_ROUND_NE,{MakeTemp(rHi,true),MakeTemp(rHi,false)});emit(OP_FTOU,{MakeTemp(rHi,true),MakeTemp(rHi,false)});emit(OP_ISHL,{MakeTemp(rHi,true),MakeTemp(rHi,false),*imm2});
        emit(OP_MUL,{MakeTemp(rLow,true),inputClone(*lowIOp,lowI),*imm255});emit(OP_ROUND_NE,{MakeTemp(rLow,true),MakeTemp(rLow,false)});emit(OP_FTOU,{MakeTemp(rLow,true),MakeTemp(rLow,false)});emit(OP_ISHL,{MakeTemp(rLow,true),MakeTemp(rLow,false),*imm2});emit(OP_INEG,{MakeTemp(rLow,true),MakeTemp(rLow,false)});emit(OP_IADD,{MakeTemp(rHi,true),MakeTemp(rHi,false),MakeTemp(rLow,false)});for(auto r:indexRegs)emit(OP_IADD,{MakeTemp(hiMap[r],true),MakeTemp(r,false),MakeTemp(rHi,false)});
        const Instruction* dclW=nullptr;const Instruction* dclI=nullptr;std::size_t lastInputDecl=0;for(const auto&x:ins){if(x.opcode>=OP_DCL_INPUT&&x.opcode<=OP_DCL_INPUT_LAST)lastInputDecl=std::max(lastInputDecl,x.off);if(x.opcode==OP_DCL_INPUT&&x.operands.size()==1&&x.operands[0].type==1){if(x.operands[0].RegIndex()==lowW)dclW=&x;if(x.operands[0].RegIndex()==lowI)dclI=&x;}}
        if(!dclW||!dclI||!lastInputDecl)throw std::runtime_error("blend input declarations not plain dcl_input");auto cloneDecl=[](const Instruction&x,std::uint32_t r){Operand o=x.operands[0];SetReg(o,r);std::vector<std::uint32_t> body;SerializeOperand(o,body);std::vector<std::uint32_t> out;out.push_back((x.tokens[0]&~0x7F000000u)|(static_cast<std::uint32_t>(body.size()+1)<<24));out.insert(out.end(),body.begin(),body.end());return out;};auto dclHiW=cloneDecl(*dclW,highW),dclHiI=cloneDecl(*dclI,highI);
        std::map<std::size_t,const Weighted*> weightByOff;for(const auto&w:weighted)weightByOff[w.off]=&w;std::vector<std::uint32_t> newDw{dw[0],0};bool setupDone=false,declDone=false;
        for(const auto&x:ins){if(x.off==firstWeight&&!setupDone){newDw.insert(newDw.end(),setup.begin(),setup.end());setupDone=true;}if(x.opcode==OP_DCL_TEMPS){auto t=x.tokens;t[1]=newTemps;newDw.insert(newDw.end(),t.begin(),t.end());}else newDw.insert(newDw.end(),x.tokens.begin(),x.tokens.end());if(x.off==lastInputDecl&&!declDone){newDw.insert(newDw.end(),dclHiW.begin(),dclHiW.end());newDw.insert(newDw.end(),dclHiI.begin(),dclHiI.end());declDone=true;}auto it=weightByOff.find(x.off);if(it!=weightByOff.end()){const auto&w=*it->second;if(x.operands.empty()||!IsDirectMaskedTempDest(x.operands[0]))throw std::runtime_error("unfamiliar skinning destination");Operand hiCb=w.cb;ReplaceRelativeTemp(hiCb,w.rel,hiMap[w.rel]);Operand hiWeight=w.weight;SetReg(hiWeight,highW);Operand acc=MakeTemp(static_cast<std::uint32_t>(x.operands[0].RegIndex()),false);auto extra=MakeInstr(OP_MAD,{x.operands[0],hiCb,hiWeight,acc});newDw.insert(newDw.end(),extra.begin(),extra.end());}}
        if(!setupDone||!declDone)throw std::runtime_error("rewrite insertion failed");newDw[1]=static_cast<std::uint32_t>(newDw.size());shex->data.resize(newDw.size()*4);std::memcpy(shex->data.data(),newDw.data(),shex->data.size());
        const auto unknown=ReadU32(isgn->data.data()+4);SignatureRecord baseW,baseI;bool haveW=false,haveI=false;for(const auto&r:sig){if(r.name=="BLENDWEIGHT"&&r.semidx==0){baseW=r;haveW=true;}if(r.name=="BLENDINDICES"&&r.semidx==0){baseI=r;haveI=true;}}if(!haveW||!haveI)throw std::runtime_error("signature bases missing");baseW.semidx=1;baseW.reg=highW;baseI.semidx=1;baseI.reg=highI;sig.push_back(baseW);sig.push_back(baseI);isgn->data=BuildSignature(sig,unknown);
        output=RebuildDxbc(data,size,chunks);
        if(outInfo){outInfo->lowWeightRegister=lowW;outInfo->lowIndexRegister=lowI;outInfo->highWeightRegister=highW;outInfo->highIndexRegister=highI;outInfo->oldTempCount=oldTemps;outInfo->newTempCount=newTemps;outInfo->weightedOperationCount=static_cast<std::uint32_t>(weighted.size());}
        return true;
    } catch (const std::exception& e) { if(error)*error=e.what(); output.clear(); return false; }
}
} 
