#include "checkpoint_integrity.h"
#include <iostream>
#include "zone_search.h"
#include "hash.h"
#include "platform.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <sstream>

namespace pkh_zone {
namespace {
std::string hex(const uint8_t* bytes, size_t count) {
    static const char chars[]="0123456789abcdef";
    std::string result(count*2,'0');
    for(size_t i=0;i<count;++i) { result[2*i]=chars[bytes[i]>>4]; result[2*i+1]=chars[bytes[i]&15]; }
    return result;
}
std::string digest(const std::string& text) {
    uint8_t hash[32]; sha256(reinterpret_cast<const uint8_t*>(text.data()),text.size(),hash);
    return hex(hash,32);
}
std::string hex64(uint64_t value) {
    uint8_t bytes[8]; for(int i=7;i>=0;--i) { bytes[i]=static_cast<uint8_t>(value); value>>=8; }
    return hex(bytes,8);
}
bool unhex(const std::string& text,uint8_t* bytes,size_t count) {
    if(text.size()!=count*2) return false;
    for(size_t i=0;i<count;++i) {
        unsigned v=0;
        for(unsigned j=0;j<2;++j) {
            const char ch=text[2*i+j]; unsigned d;
            if(ch>='0'&&ch<='9') d=ch-'0';
            else if(ch>='a'&&ch<='f') d=ch-'a'+10;
            else return false;
            v=(v<<4)|d;
        }
        bytes[i]=static_cast<uint8_t>(v);
    }
    return true;
}
bool unsigned64(const std::string& text,uint64_t& value) {
    if(text.empty() || (text.size()>1 && text[0]=='0')) return false;
    value=0;
    for(char ch:text) {
        if(ch<'0'||ch>'9') return false;
        const unsigned digit=ch-'0';
        if(value>((std::numeric_limits<uint64_t>::max)()-digit)/10) return false;
        value=value*10+digit;
    }
    return true;
}
bool field(const std::string& line,const std::string& name,std::string& value) {
    const std::string prefix=name+'=';
    if(line.compare(0,prefix.size(),prefix)!=0) return false;
    value=line.substr(prefix.size()); return true;
}
bool same_rng(const ZoneTraversal& a,const ZoneTraversal& b) {
    return std::equal(a.rng,a.rng+4,b.rng);
}
std::string payload(const ZoneTraversal& walk,const std::string& identity) {
    std::string text="magic=PKHZONEBLOCK2\nidentity="+identity+
        "\nlow="+hex(walk.lo.b,32)+"\nhigh="+hex(walk.hi.b,32)+
        "\nseed="+hex(walk.seed.b,32)+"\ncurrent="+hex(walk.current.b,32)+
        "\noffset="+std::to_string(walk.offset)+"\nskip="+std::to_string(walk.skip)+'\n';
    for(int i=0;i<4;++i) text+="rng"+std::to_string(i)+'='+hex64(walk.rng[i])+'\n';
    text+="sequence="+std::to_string(walk.sequence)+"\nstarted="+(walk.started?"1":"0")+
        "\nexhausted="+(walk.exhausted?"1":"0")+
        "\nlast_position="+std::to_string(walk.last_position+1)+
        "\nlast_value="+std::to_string(walk.last_value+1)+
        "\nlast_base="+hex(walk.last_base.b,32)+'\n';
    return text;
}
bool decode(const std::string& raw,const ZoneTraversal& expected,
            const std::string& identity,ZoneTraversal& out,std::string& why) {
    auto bad=[&](const char* message) { why=message; return false; };
    std::string text; text.reserve(raw.size());
    for(size_t i=0;i<raw.size();++i) {
        if(!raw[i]) return bad("Checkpoint contains NUL");
        if(raw[i]=='\r') {
            if(i+1==raw.size()||raw[i+1]!='\n') return bad("Invalid checkpoint newline");
            continue;
        }
        text+=raw[i];
    }
    if(text.empty()||text.back()!='\n') return bad("Incomplete checkpoint");
    std::array<std::string,19> lines;
    size_t offset=0,checksum_offset=0;
    for(size_t i=0;i<lines.size();++i) {
        const size_t end=text.find('\n',offset);
        if(end==std::string::npos) return bad("Incomplete checkpoint fields");
        if(i==18) checksum_offset=offset;
        lines[i]=text.substr(offset,end-offset); offset=end+1;
    }
    if(offset!=text.size()||lines[0]!="magic=PKHZONEBLOCK2") return bad("Unsupported checkpoint format");
    std::string value;
    if(!field(lines[18],"checksum",value)||value!=digest(text.substr(0,checksum_offset)))
        return bad("Checkpoint checksum mismatch");
    if(!field(lines[1],"identity",value)||value!=identity) return bad("Checkpoint target/configuration mismatch");
    ZoneTraversal saved=expected;
    if(!field(lines[2],"low",value)||!unhex(value,saved.lo.b,32)||
       !field(lines[3],"high",value)||!unhex(value,saved.hi.b,32)||
       !field(lines[4],"seed",value)||!unhex(value,saved.seed.b,32)||
       !field(lines[5],"current",value)||!unhex(value,saved.current.b,32))
        return bad("Invalid checkpoint key encoding");
    if(be256_cmp(&saved.lo,&expected.lo)||be256_cmp(&saved.hi,&expected.hi))
        return bad("Checkpoint interval mismatch");
    uint64_t number=0;
    if(!field(lines[6],"offset",value)||!unsigned64(value,saved.offset)||
       !field(lines[7],"skip",value)||!unsigned64(value,number)||number!=uint64_t(expected.skip))
        return bad("Invalid checkpoint block size/offset");
    uint64_t any=0;
    for(int i=0;i<4;++i) {
        uint8_t bytes[8];
        if(!field(lines[8+i],"rng"+std::to_string(i),value)||!unhex(value,bytes,8))
            return bad("Invalid checkpoint RNG");
        saved.rng[i]=0;
        for(uint8_t byte:bytes) saved.rng[i]=(saved.rng[i]<<8)|byte;
        any|=saved.rng[i];
    }
    if(!any||!field(lines[12],"sequence",value)||!unsigned64(value,saved.sequence))
        return bad("Invalid checkpoint sequence/RNG");
    if(!field(lines[13],"started",value)||(value!="0"&&value!="1")) return bad("Invalid started flag");
    saved.started=value=="1";
    if(!field(lines[14],"exhausted",value)||(value!="0"&&value!="1")) return bad("Invalid exhausted flag");
    saved.exhausted=value=="1";
    if(!field(lines[15],"last_position",value)||!unsigned64(value,number)||number>64)
        return bad("Invalid changed position");
    saved.last_position=int(number)-1;
    if(!field(lines[16],"last_value",value)||!unsigned64(value,number)||number>16)
        return bad("Invalid replacement value");
    saved.last_value=int(number)-1;
    if(!field(lines[17],"last_base",value)||!unhex(value,saved.last_base.b,32))
        return bad("Invalid previous key");
    if(saved.started!=(saved.sequence!=0)||(!saved.started&&(saved.offset||saved.exhausted)))
        return bad("Inconsistent initial checkpoint state");
    if((saved.last_position<0)!=(saved.last_value<0)) return bad("Inconsistent replacement metadata");
    ZoneTraversal first; first.init(saved.skip,saved.lo,saved.hi,saved.seed);
    if(saved.started) {
        if(be256_cmp(&saved.current,&saved.block_lo)<0||be256_cmp(&saved.current,&saved.hi)>=0)
            return bad("Checkpoint block outside interval");
        be256 start,end; ZoneInfo info;
        const uint64_t count=saved.bounds(start,end,info);
        if(!count||saved.offset>=count) return bad("Checkpoint offset outside clipped block");
        if(saved.exhausted) {
            const uint64_t tail=saved.skip ? (UINT64_C(1)<<saved.skip)-1 : 0;
            if(saved.offset || (saved.sequence!=(std::numeric_limits<uint64_t>::max)() &&
                               (saved.valid_positions()&~tail)!=0))
                return bad("Invalid exhausted checkpoint state");
        }
        be256 key; ZoneInfo info_first;
        if(!first.next(key,info_first)) return bad("Invalid initial seed");
        for(int nib=0;nib<saved.skip;++nib)
            if(ZoneTraversal::digit_at(first.current,nib)!=ZoneTraversal::digit_at(saved.current,nib))
                return bad("Checkpoint changed a digit inside the block");
    }
    if(saved.sequence<=1 && (be256_cmp(&saved.current,&first.current)||!same_rng(saved,first)))
        return bad("Checkpoint initial state does not match seed");
    out=saved; return true;
}
} // namespace

bool ZoneSearch::init(int nibbles,const uint8_t low[32],const uint8_t high[32],
                      const uint8_t seed[32],const std::string& identity,std::string& why) {
    why.clear();
    static const uint8_t max_key[32]={
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
        0xba,0xae,0xdc,0xe6,0xaf,0x48,0xa0,0x3b,0xbf,0xd2,0x5e,0x8c,0xd0,0x36,0x41,0x40};
    const uint8_t zero[32]{};
    if(nibbles<0||nibbles>15||!low||!high||!seed||identity.empty()||
       std::memcmp(low,zero,32)==0||std::memcmp(low,high,32)>0||std::memcmp(high,max_key,32)>0) {
        why="Invalid zone interval, block size, seed or target identity"; return false;
    }
    be256 lo,hi,seed_value;
    std::memcpy(lo.b,low,32); std::memcpy(hi.b,high,32); std::memcpy(seed_value.b,seed,32);
    be256_add_u64(&hi,1);
    ZoneTraversal fresh; fresh.init(nibbles,lo,hi,seed_value);
    const std::string binding=digest(identity);
    std::lock_guard<std::mutex> lock(state_mutex_);
    initial_=committed_=fresh; identity_hash_=binding; initialized_=true; ++version_; ++generation_;
    return true;
}

bool ZoneSearch::prepare_batch(size_t max_pieces,uint64_t piece_keys,
                               ZoneBatch& batch,std::string& why) const {
    why.clear(); batch=ZoneBatch{};
    if(!max_pieces||!piece_keys) { why="Zone batch dimensions must be positive"; return false; }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(!initialized_) { why="Zone search is not initialized"; return false; }
        batch.next=committed_; batch.version=version_; batch.owner_=this; batch.generation_=generation_;
    }
    return fill_batch(max_pieces,piece_keys,batch,why);
}

bool ZoneSearch::prepare_after(const ZoneBatch& previous,size_t max_pieces,uint64_t piece_keys,
                                ZoneBatch& batch,std::string& why) const {
    why.clear();batch=ZoneBatch{};
    if(previous.owner_!=this || previous.version==(std::numeric_limits<uint64_t>::max)() ||
       !max_pieces || !piece_keys) {why="Invalid speculative zone predecessor";return false;}
    // No shared traversal state is read here. A stop/reset may discard this
    // work; commit_batch still checks both the owner and exact state version.
    batch.next=previous.next;batch.version=previous.version+1;batch.owner_=this;batch.generation_=previous.generation_;
    return fill_batch(max_pieces,piece_keys,batch,why);
}

bool ZoneSearch::fill_batch(size_t max_pieces,uint64_t piece_keys,ZoneBatch& batch,std::string& why) {
    if(max_pieces>batch.full_starts.max_size()/32){why="Batch capacity overflow";return false;}
    batch.pieces.reserve(max_pieces);batch.full_starts.reserve(max_pieces*32);
    be256 block_begin{},end{};ZoneInfo info;uint64_t count=0;bool refresh=true;
    for(size_t i=0;i<max_pieces && !batch.next.exhausted;++i) {
        if(refresh){count=batch.next.bounds(block_begin,end,info);refresh=false;}
        be256 begin=block_begin;
        if(!count) {
            if(batch.next.exhausted) break;
            why="Zone produced an empty active block"; batch=ZoneBatch{}; return false;
        }
        if(batch.next.offset>=count) { why="Zone offset outside current block"; batch=ZoneBatch{}; return false; }
        be256_add_u64(&begin,batch.next.offset);
        ZonePiece piece; piece.count=(std::min)(piece_keys,count-batch.next.offset); piece.info=info;
        std::memcpy(piece.key,begin.b,32);
        if(batch.keys>(std::numeric_limits<uint64_t>::max)()-piece.count) {
            why="Zone batch key count overflow"; batch=ZoneBatch{}; return false;
        }
        batch.pieces.push_back(piece); batch.keys+=piece.count;
        if(piece.count==piece_keys)batch.full_starts.insert(batch.full_starts.end(),piece.key,piece.key+32);
        batch.next.offset+=piece.count;
        if(batch.next.offset==count) {batch.next.advance();refresh=true;}
    }
    return true;
}

bool ZoneSearch::commit_batch(const ZoneBatch& batch,std::string& why) {
    why.clear(); std::lock_guard<std::mutex> lock(state_mutex_);
    if(!initialized_||batch.owner_!=this||batch.version!=version_||batch.generation_!=generation_) {
        why="Zone batch belongs to an obsolete or different search state"; return false;
    }
    committed_=batch.next; ++version_; return true;
}
ZoneStatus ZoneSearch::status() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    ZoneStatus result; result.initialized=initialized_; result.started=committed_.started;
    result.exhausted=committed_.exhausted; result.sequence=committed_.sequence;
    result.offset=committed_.offset; result.block_keys=initialized_?committed_.block_keys():0;
    result.skip=committed_.skip; result.version=version_;
    std::memcpy(result.current,committed_.current.b,32); return result;
}
void ZoneSearch::reset() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if(initialized_) { committed_=initial_; ++version_; ++generation_; }
}
std::string ZoneSearch::checkpoint_path() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if(!initialized_) return {};
    // Block2 keeps the canonical lower partial block reachable. Block1 states
    // can use a different residue and transition domain; keep their files and
    // start a new namespace instead of silently reinterpreting an old walk.
    return "checkpoint_zone_block2_"+digest(identity_hash_+hex(initial_.lo.b,32)+
        hex(initial_.hi.b,32)+":"+std::to_string(initial_.skip))+".dat";
}
bool ZoneSearch::save(const std::string& path,std::string& why) const {
    why.clear(); std::lock_guard<std::mutex> io_lock(io_mutex_);
    ZoneTraversal snapshot; std::string identity;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(!initialized_) { why="Zone search is not initialized"; return false; }
        snapshot=committed_; identity=identity_hash_;
    }
    if(path.empty()||path.find('\0')!=std::string::npos) { why="Invalid checkpoint path"; return false; }
    if (rejected_path_ == path) {
        std::string saved;
        if (!preserve_rejected_checkpoint(path, saved, why)) return false;
        rejected_path_.clear();
        if (!saved.empty()) std::cerr << "  Отклонённый чекпоинт сохранён: " << saved << "\n";
    }
    const std::string body=payload(snapshot,identity);
    const std::string text=body+"checksum="+digest(body)+'\n', temp=path+".tmp";
    std::ofstream file(plat_wide(temp).c_str(),std::ios::binary|std::ios::trunc);
    if(!file) { why="Cannot open zone checkpoint temporary file"; return false; }
    file.write(text.data(),static_cast<std::streamsize>(text.size())); file.flush();
    const bool wrote=bool(file); file.close();
    if(!wrote||file.fail()||!plat_fsync_file(temp)) {
        why="Cannot flush zone checkpoint"; plat_remove_file(temp); return false;
    }
    if(!plat_replace_file(temp,path)) {
        why="Cannot replace zone checkpoint; previous file retained"; plat_remove_file(temp); return false;
    }
    return true;
}
bool ZoneSearch::load(const std::string& path,std::string& why) {
    why.clear(); std::lock_guard<std::mutex> io_lock(io_mutex_);
    ZoneTraversal expected; std::string identity; uint64_t version;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if(!initialized_) { why="Zone search is not initialized"; return false; }
        expected=initial_; identity=identity_hash_; version=version_;
    }
    if(path.empty()||path.find('\0')!=std::string::npos) { why="Invalid checkpoint path"; return false; }
    rejected_path_ = path;
    std::ifstream file(plat_wide(path).c_str(),std::ios::binary|std::ios::ate);
    if(!file) { why="Cannot open zone checkpoint"; return false; }
    const std::streamoff size=file.tellg();
    if(size<=0||size>8192) { why="Invalid zone checkpoint size"; return false; }
    file.seekg(0,std::ios::beg); std::string raw(static_cast<size_t>(size),'\0');
    if(!file.read(&raw[0],static_cast<std::streamsize>(size))) { why="Cannot read complete zone checkpoint"; return false; }
    ZoneTraversal loaded;
    if(!decode(raw,expected,identity,loaded,why)) return false;
    std::lock_guard<std::mutex> lock(state_mutex_);
    if(version!=version_) { why="Zone state changed while loading checkpoint"; return false; }
    committed_=loaded; ++version_; ++generation_; rejected_path_.clear(); return true;
}
} // namespace pkh_zone
