// Real, nonquiet CPU finding pipeline in disposable directories. No live
// configuration, database, result, checkpoint, GPU, or network is used.
#include "common.h"
#include "engine.h"
#include "hybrid_search.h"
#include "notify.h"
#include "platform.h"
#include "search.h"
#include "search_range.h"
#include "targets.h"
#include "traverse.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>

#ifdef PKH_HAVE_CUDA
#error This fixture requires CPU-only application objects.
#endif

namespace {
uint64_t checks=0,cases=0,archives=0,scanned=0;
std::filesystem::path fixture_root;
const std::string stale_password="unused legacy setting: spaces, quote \" and unicode \xD0\xBF";
void require(bool ok,const char* label) {++checks;if(!ok)throw std::runtime_error(label);}
struct Silence {
    std::ostringstream output,errors;
    std::streambuf *old_output=std::cout.rdbuf(output.rdbuf()),*old_errors=std::cerr.rdbuf(errors.rdbuf());
    ~Silence(){std::cout.rdbuf(old_output);std::cerr.rdbuf(old_errors);}
};
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);require(bool(input),"expected local fixture file opens");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
std::string normalized(std::string text) {
    text.erase(std::remove(text.begin(),text.end(),'\r'),text.end());return text;
}
size_t occurrences(const std::string& text,const std::string& wanted) {
    size_t count=0,at=0;while((at=text.find(wanted,at))!=std::string::npos){++count;at+=wanted.size();}return count;
}
std::string scalar_hex(const u256 value) {uint8_t bytes[32];u256_to_be32(bytes,value);return to_hex(bytes,32);}
std::string public_key(const Point& point,bool compressed) {
    uint8_t bytes[65]{};bytes[0]=compressed?uint8_t(2|(point.y[0]&1)):4;
    fe_to_be32(bytes+1,point.x);if(!compressed)fe_to_be32(bytes+33,point.y);
    return to_hex(bytes,compressed?33:65);
}
Point negative(Point value) {if(!value.inf)fe_neg(value.y,value.y);return value;}
void verify_wif(const std::string& value,const u256 actual,bool compressed) {
    std::vector<uint8_t> decoded;require(base58_decode(value,decoded),"saved WIF decodes");
    require(decoded.size()==(compressed?38:37)&&decoded[0]==0x80,"saved WIF size and network");
    uint8_t bytes[32];u256_to_be32(bytes,actual);
    require(std::equal(bytes,bytes+32,decoded.begin()+1),"saved WIF encodes actual scalar");
    if(compressed)require(decoded[33]==1,"saved WIF compressed marker");
    uint8_t first[32],second[32];sha256(decoded.data(),decoded.size()-4,first);sha256(first,32,second);
    require(std::equal(second,second+4,decoded.end()-4),"saved WIF checksum valid");
}
void verify_fields(const std::string& raw,const FoundInfo& expected) {
    const auto text=normalized(raw);
    for(const auto& field:std::vector<std::pair<std::string,std::string>>{
        {"PUBLIC KEY (c)",expected.pub_c},{"PUBLIC KEY (u)",expected.pub_u},
        {"PRIVATE KEY",expected.priv_hex},{"WIF (c)",expected.wif_c},{"WIF (u)",expected.wif_u},
        {"X",expected.x_hex},{"Y",expected.y_hex}}) {
        std::istringstream lines(text);std::string line;size_t matching=0;
        while(std::getline(lines,line)) {
            const auto colon=line.find(':');if(colon==std::string::npos)continue;
            auto label=line.substr(0,colon),value=line.substr(colon+1);
            const auto trim=[](std::string& s){const auto begin=s.find_first_not_of(" \t");if(begin==std::string::npos){s.clear();return;}s=s.substr(begin,s.find_last_not_of(" \t")-begin+1);};
            trim(label);trim(value);if(label==field.first&&value==field.second)++matching;
        }
        require(matching==1,"plaintext field contains exact result once");
    }
    require(text.find(stale_password)==std::string::npos,"legacy password is not written to result");
}
int run_archive(const std::string& arguments,const std::filesystem::path& log) {
    std::string binary=seven_zip_binary();require(!binary.empty(),"local 7-Zip available for archive verification");
    if(binary.size()>1&&binary.front()=='"'&&binary.back()=='"')binary=binary.substr(1,binary.size()-2);
    SECURITY_ATTRIBUTES attributes{sizeof attributes,nullptr,TRUE};
    HANDLE output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&attributes,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attributes,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(output!=INVALID_HANDLE_VALUE&&input!=INVALID_HANDLE_VALUE,"isolated archiver streams open");
    STARTUPINFOW startup{};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=input;startup.hStdOutput=startup.hStdError=output;
    PROCESS_INFORMATION process{};
    auto command=plat_wide(plat_quote(binary)+" "+arguments);
    const BOOL started=CreateProcessW(plat_wide(binary).c_str(),command.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&startup,&process);
    CloseHandle(input);CloseHandle(output);require(started!=FALSE,"local archiver starts");
    const DWORD status=WaitForSingleObject(process.hProcess,30000);DWORD exit_code=1;
    if(status==WAIT_TIMEOUT){TerminateProcess(process.hProcess,3);WaitForSingleObject(process.hProcess,5000);}
    GetExitCodeProcess(process.hProcess,&exit_code);CloseHandle(process.hThread);CloseHandle(process.hProcess);
    require(status==WAIT_OBJECT_0,"local archive check completes without password prompt");return int(exit_code);
}
void verify_archive(const FoundInfo& expected) {
    ++archives;const auto archive=std::filesystem::absolute(std::filesystem::path("RESULT")/(expected.addr_c+".7z"));
    require(std::filesystem::is_regular_file(archive),"normal result creates additional archive");
    const auto suffix=std::to_string(archives);
    require(run_archive("t -y "+plat_quote(archive.u8string()),"archive-test-"+suffix+".log")==0,"archive tests without password");
    const auto listing="archive-list-"+suffix+".log";
    require(run_archive("l -slt -y "+plat_quote(archive.u8string()),listing)==0,"archive lists without password");
    const auto details=read(listing);
    require(details.find("Encrypted = -")!=std::string::npos&&details.find("Encrypted = +")==std::string::npos,"archive contents explicitly unencrypted");
    const auto destination=std::filesystem::absolute("extract-"+suffix);
    require(run_archive("x -y "+plat_quote("-o"+destination.u8string())+" "+plat_quote(archive.u8string()),"archive-extract-"+suffix+".log")==0,"archive extracts without password");
    size_t files=0;for(const auto& item:std::filesystem::recursive_directory_iterator(destination))if(item.is_regular_file()) {
        ++files;verify_fields(read(item.path()),expected);
    }
    require(files==1,"archive contains exactly one readable result record");
}
FoundInfo expected_record(const u256 key,const Point& point,int kind) {
    const FoundInfo value=make_found(key,point.x,point.y,kind);
    u256 actual;u256_set(actual,key);if(kind==2)u256_sub(actual,EC_N,key);
    const Point actual_point=ec_mul_G(actual),wanted=kind==2?negative(point):point;
    require(!actual_point.inf&&fe_eq(actual_point.x,wanted.x)&&fe_eq(actual_point.y,wanted.y),"actual private scalar independently regenerates saved public point");
    require(value.priv_hex==scalar_hex(actual)&&value.pub_c==public_key(actual_point,true)&&value.pub_u==public_key(actual_point,false),"record uses actual scalar and both correct public encodings");
    require(value.mirror==(kind==2),"record mirror flag correct");
    verify_wif(value.wif_c,actual,true);verify_wif(value.wif_u,actual,false);
    require(archive_password(value).empty(),"new archive password is empty despite stale config value");
    return value;
}
void setup(const char* label,int mode) {
    ++cases;const auto directory=fixture_root/label;require(std::filesystem::create_directory(directory),"fresh isolated case directory");
    std::filesystem::current_path(directory);g_cfg=Config{};g_cfg.mode=mode;g_cfg.threads=1;g_cfg.chunk_bits=12;
    g_cfg.wave_nibbles=g_cfg.zone_nibbles=g_cfg.hybrid_nibbles=3;g_cfg.det_hex=-1;g_cfg.want_nk=true;
    g_cfg.mask_hex.clear();g_cfg.mask_froz=0;g_cfg.archive_pass=stale_password;
    g_cfg.tg_token.clear();g_cfg.tg_chat.clear();g_cfg.signal_to.clear();g_cfg.signal_account.clear();
    g_cfg.notify_cmd.clear();g_cfg.tg_progress=g_cfg.signal_progress=false;signal_set_ready(false);
    g_cfg.found_path=(directory/"RESULT"/"Found.txt").u8string();
    g_quiet=false;g_search.reset_progress();g_pause.reset(&g_stop);g_stop=false;g_stop_flag=0;g_shutdown_requested=false;
    g_test_found.clear();g_last_found.clear();g_t0=search_now();
    u256_from_u64(g_range_start,UINT64_C(1000000)+cases*100000);u256_add_u64(g_range_end,g_range_start,8192+36);
    set_block_nibbles(3);compute_total_blocks256();compute_top_nibble();compute_zone_plan();
    zone_set_seed(7137);wave_set_seed(7137);u256 one;u256_from_u64(one,1);set_scan_step(one);
    g_db.items.clear();g_db.n_comp=g_db.n_uncomp=g_db.n_xonly=0;
    require(!g_cfg.found_path.empty()&&!g_cfg.tg_token.size()&&!g_cfg.tg_chat.size()&&!g_cfg.signal_to.size()&&!g_cfg.notify_cmd.size(),"real notification credentials and external command empty");
}
void verify_saved(const std::vector<FoundInfo>& expected) {
    require(wait_found_postprocessing(30000),"optional local artifact queue drains before archive checks and fixture cleanup");
    const auto found=read(std::filesystem::u8path(g_cfg.found_path));
    require(occurrences(found,"=== НАЙДЕНО #")==expected.size(),"plaintext Found contains exact committed record count");
    require(found.find("ЗАШИФРОВАННАЯ НАХОДКА")==std::string::npos,"Found is a complete plaintext record rather than encrypted index");
    for(const auto& value:expected) {
        const auto key_at=found.find("PRIVATE KEY    : "+value.priv_hex);
        require(key_at!=std::string::npos,"expected private scalar present in normal Found record");
        const auto begin=found.rfind("=== НАЙДЕНО #",key_at),end=found.find("=== НАЙДЕНО #",key_at);
        require(begin!=std::string::npos,"private scalar belongs to complete plaintext record");
        verify_fields(found.substr(begin,end==std::string::npos?std::string::npos:end-begin),value);verify_archive(value);
    }
    require(g_found_num.load()==expected.size(),"committed count equals durable plaintext results");
    require(plat_fsync_file(g_cfg.found_path),"plaintext result can be flushed and reopened");
    require(!g_stop.load()&&g_stop_flag==0&&!g_shutdown_requested,"saving a hit never requests search stop");
    require(pending_background_notifications()==0,"empty notification config starts no background worker");
    require(g_test_found.empty(),"real writer used rather than quiet test hook");
    require(g_cfg.archive_pass==stale_password,"legacy config was ignored without mutating caller configuration");
    for(const auto& item:std::filesystem::recursive_directory_iterator(std::filesystem::current_path())) {
        const auto name=item.path().filename().u8string();
        require(name!="config.ini"&&name.find("checkpoint")==std::string::npos&&name.find(".pending.")==std::string::npos&&name.find(".pubkeyhunt-")==std::string::npos,"normal result leaves no config/checkpoint or pending temp file");
    }
}
void database_mode(const char* label,int mode) {
    setup(label,mode);u256 start{};uint64_t count=0;int phase=-1;std::mt19937_64 random(7137);
    pkh_hybrid::HybridSearch hybrid;pkh_hybrid::HybridBatch batch;std::string why;
    if(mode==TRAV_HYBRID) {
        uint8_t low[32],high[32],seed[32]{};u256_to_be32(low,g_range_start);u256_to_be32(high,g_range_end);seed[31]=91;
        require(hybrid.init(3,low,high,seed,"plain-found-synthetic",why),"actual hybrid initializes");
        require(hybrid.prepare_batch(1,4096,batch,why)&&batch.pieces.size()==1,"actual hybrid produces candidate interval");
        fe_from_be32(start,batch.pieces[0].key);count=batch.pieces[0].count;
    } else {
        ChunkIdx first;require(next_chunk(first,start,count,phase,random),"actual CPU mode produces candidate interval");
    }
    require(count>=4&&count<=4096,"small actual candidate interval has later work");
    u256 first,last,mirror;u256_add_u64(first,start,1);u256_add_u64(last,start,count-1);u256_sub(mirror,EC_N,first);
    require(!search_key_in_range(mirror),"test mirror really outside main range");
    const Point a=ec_mul_G(first),b=ec_mul_G(last);
    require(g_db.add_hex(public_key(a,true))&&g_db.add_hex(public_key(negative(a),false))&&g_db.add_hex(public_key(b,false)),"synthetic compressed/uncompressed and mirror targets accepted");
    g_db.build();require(push_targets_to_backend(),"actual CPU backend receives synthetic database");
    const auto direct=expected_record(first,a,1),reversed=expected_record(first,a,2),later=expected_record(last,b,1);
    scan_chunk(start,2);scanned+=2;
    require(g_found_num.load()==2&&g_count.load()==2&&!g_stop.load(),"early normal finding saves direct/mirror without stopping later candidates");
    const auto before=read(std::filesystem::u8path(g_cfg.found_path));
    require(report_found_in_range(first,a.x,a.y,1,g_range_start,g_range_end)&&report_found_in_range(first,a.x,a.y,2,g_range_start,g_range_end),"already committed direct/mirror reports succeed");
    require(g_found_num.load()==2&&read(std::filesystem::u8path(g_cfg.found_path))==before,"repeated report adds no duplicate result or counter");
    u256 rest;u256_add_u64(rest,start,2);scan_chunk(rest,count-2);scanned+=count-2;
    require(g_count.load()==count&&g_found_num.load()==3,"CPU scan continues after saved hit and commits later candidate");
    if(mode==TRAV_HYBRID)require(hybrid.commit_batch(batch,why),"hybrid interval commits only after durable result writing");
    verify_saved({direct,reversed,later});
}
void explicit_kangaroo() {
    setup("explicit_kangaroo",TRAV_SEQ);u256 lo,hi,key,later,invalid{};
    u256_from_u64(lo,800000001);u256_add_u64(hi,lo,31);u256_add_u64(key,lo,7);u256_add_u64(later,lo,25);
    const Point point=ec_mul_G(key),other=ec_mul_G(later);
    require(!search_key_in_range(key),"explicit-range finding is independent of ordinary global bounds");
    const auto direct=expected_record(key,point,1),mirror=expected_record(key,point,2),next=expected_record(later,other,1);
    require(report_found_in_range(key,point.x,point.y,1,lo,hi)&&report_found_in_range(key,point.x,point.y,2,lo,hi),"kangaroo explicit-range direct/mirror commits plaintext");
    const auto before=read(std::filesystem::u8path(g_cfg.found_path));
    require(report_found_in_range(key,point.x,point.y,1,lo,hi)&&report_found_in_range(key,point.x,point.y,2,lo,hi),"kangaroo repeated committed hits succeed");
    require(g_found_num.load()==2&&read(std::filesystem::u8path(g_cfg.found_path))==before,"kangaroo repeated hits do not duplicate durable record");
    require(report_found_in_range(later,other.x,other.y,1,lo,hi),"kangaroo later hit commits after prior save");
    Point bad=point;bad.y[0]^=1;u256 outside;u256_sub_u64(outside,lo,1);const Point own=ec_mul_G(outside);
    require(!report_found_in_range(key,bad.x,bad.y,1,lo,hi)&&!report_found_in_range(invalid,point.x,point.y,1,lo,hi)&&!report_found_in_range(outside,own.x,own.y,2,lo,hi),"malformed/invalid/out-of-range sources rejected before persistence");
    verify_saved({direct,mirror,next});
}
}
int main(int argc,char**argv) {
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try {
        require(argc==2,"supply empty isolated fixture directory");fixture_root=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        require(std::filesystem::is_directory(fixture_root)&&std::filesystem::is_empty(fixture_root),"fixture directory exists and is empty");
        {Silence silence;std::filesystem::current_path(fixture_root);init_colors();build_table();
            database_mode("sequential",TRAV_SEQ);database_mode("kxe",TRAV_KXE);database_mode("wave",TRAV_WAVE);
            database_mode("zone",TRAV_ZONE);database_mode("random",TRAV_RANDOM);database_mode("hybrid",TRAV_HYBRID);explicit_kangaroo();}
        std::printf("PASS: plaintext result checks=%llu cases=%llu archives=%llu scanned=%llu; six actual CPU DB modes and explicit kangaroo, nonquiet durable full Found, exact scalar/public/WIF, mirror outside range, stale password ignored, archive test/list/extract without password, dedup and continue; notifications=0 GPU=0\n",
            static_cast<unsigned long long>(checks),static_cast<unsigned long long>(cases),static_cast<unsigned long long>(archives),static_cast<unsigned long long>(scanned));return 0;
    } catch(const std::exception& error){std::fprintf(stderr,"FAIL: plaintext result: %s\n",error.what());return 1;}
}
