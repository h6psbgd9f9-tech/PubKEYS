#include "pult_control.h"
#include "worker_threads.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pult {
namespace {
std::string strip(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
bool ini(const std::string& text, std::map<std::string,std::string>& out, std::string& error) {
    if (text.size() > 65536) { error = "Control snapshot exceeds 64 KiB"; return false; }
    std::istringstream input(text); std::string line;
    while (std::getline(input, line)) {
        if (out.empty() && line.size() >= 3 && (unsigned char)line[0] == 0xef
            && (unsigned char)line[1] == 0xbb && (unsigned char)line[2] == 0xbf) line.erase(0,3);
        line = strip(line);
        if (line.empty() || line[0]=='#' || line[0]==';') continue;
        const auto equal = line.find('=');
        if (equal == std::string::npos) { error="Expected key=value"; return false; }
        const auto key=strip(line.substr(0,equal)), value=strip(line.substr(equal+1));
        if (key.empty() || out.count(key) || value.find('\0')!=std::string::npos) {
            error="Duplicate or invalid control key"; return false;
        }
        out[key]=value;
    }
    return true;
}
bool natural(const std::string& s, uint64_t& value) {
    if (s.empty() || s.size()>20 || s.find_first_not_of("0123456789")!=std::string::npos) return false;
    errno=0; char* end=nullptr; const auto n=std::strtoull(s.c_str(),&end,10);
    if(errno==ERANGE || !end || *end) return false;
    value=static_cast<uint64_t>(n); return true;
}
bool integer(const std::string& s, int lo, int hi, int& value) {
    if(s.empty())return false;
    errno=0; char* end=nullptr; const auto n=std::strtol(s.c_str(),&end,10);
    if(errno==ERANGE || !end || *end || n<lo || n>hi)return false;
    value=static_cast<int>(n);return true;
}
bool hex64(const std::string& s) {
    return s.size()==64 && s.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;
}
// Preflight the same inclusive bit range and frozen suffix used by the
// existing startup code, without touching its process-global configuration.
bool effective_range(const Config& c, std::string& error) {
    u256 low{}, high{}, maximum;
    low[c.range_start_bit/64]=UINT64_C(1)<<(c.range_start_bit%64);
    for(int bit=0;bit<c.range_end_bit;++bit)high[bit/64]|=UINT64_C(1)<<(bit%64);
    u256_sub_u64(maximum,EC_N,1);
    if(u256_cmp(high,maximum)>0)u256_set(high,maximum);
    if(c.det_hex>=0){
        const int shift=((c.range_end_bit-1)/4)*4;
        u256 first{},last{};first[shift/64]=uint64_t(c.det_hex)<<(shift%64);u256_set(last,first);
        for(int bit=0;bit<shift;++bit)last[bit/64]|=UINT64_C(1)<<(bit%64);
        if(u256_cmp(first,low)>0)u256_set(low,first);
        if(u256_cmp(last,high)<0)u256_set(high,last);
    }
    if(c.mode==TRAV_WAVE&&!c.wave_start.empty()){
        u256 start;hex_to_u256(c.wave_start.c_str(),start);
        if(u256_cmp(start,low)<0||u256_cmp(start,high)>0){error="wave_start is outside the selected range";return false;}
    }
    if(c.mode==TRAV_SEQ&&!c.mask_hex.empty()){
        u256 key;hex_to_u256(c.mask_hex.c_str(),key);
        if(u256_cmp(key,high)>0){error="The mask and selected range do not intersect";return false;}
        if(u256_cmp(key,low)<0){
            if(c.mask_froz==64){error="The fixed key is outside the selected range";return false;}
            const int shift=4*c.mask_froz;
            u256 candidate;u256_set(candidate,low);
            for(int bit=0;bit<shift;++bit){
                const uint64_t flag=UINT64_C(1)<<(bit%64);
                candidate[bit/64]=(candidate[bit/64]&~flag)|(key[bit/64]&flag);
            }
            if(u256_cmp(candidate,low)<0){
                u256 step{};step[shift/64]=UINT64_C(1)<<(shift%64);
                u256 next;u256_add(next,candidate,step);
                if(u256_cmp(next,candidate)<0){error="The mask and selected range do not intersect";return false;}
                u256_set(candidate,next);
            }
            if(u256_cmp(candidate,high)>0){error="The mask and selected range do not intersect";return false;}
        }
    }
    return true;
}
std::string numbered(const char* prefix, uint64_t revision) {
    std::ostringstream s;s<<prefix<<std::setw(12)<<std::setfill('0')<<revision<<".ini";return s.str();
}
bool read_text(const std::filesystem::path& path, std::string& text, std::string& error) {
    std::error_code ec;
    if(!std::filesystem::is_regular_file(path,ec) || std::filesystem::is_symlink(path,ec)) {
        error="Control snapshot is not a regular local file";return false;
    }
#ifdef _WIN32
    const DWORD attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_REPARSE_POINT)) {error="Reparse control file rejected";return false;}
#endif
    const auto size=std::filesystem::file_size(path,ec);
    if(ec || size>65536){error="Control snapshot exceeds 64 KiB";return false;}
    std::ifstream input(path,std::ios::binary);
    if(!input){error="Cannot read control snapshot";return false;}
    text.assign(std::istreambuf_iterator<char>(input),{});return true;
}
bool protect_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
    std::vector<unsigned char> buffer(size);
    const bool token_ok=GetTokenInformation(token,TokenUser,buffer.data(),size,&size)!=FALSE;
    CloseHandle(token);if(!token_ok)return false;
    SID_IDENTIFIER_AUTHORITY nt=SECURITY_NT_AUTHORITY;PSID system=nullptr;
    if(!AllocateAndInitializeSid(&nt,1,SECURITY_LOCAL_SYSTEM_RID,0,0,0,0,0,0,0,&system))return false;
    EXPLICIT_ACCESSW entries[2]{};
    for(auto& entry:entries){entry.grfAccessPermissions=GENERIC_ALL;entry.grfAccessMode=SET_ACCESS;
        entry.grfInheritance=SUB_CONTAINERS_AND_OBJECTS_INHERIT;entry.Trustee.TrusteeForm=TRUSTEE_IS_SID;}
    entries[0].Trustee.ptstrName=(LPWSTR)((TOKEN_USER*)buffer.data())->User.Sid;
    entries[1].Trustee.ptstrName=(LPWSTR)system;
    PACL acl=nullptr;const DWORD built=SetEntriesInAclW(2,entries,nullptr,&acl);
    const DWORD applied=built==ERROR_SUCCESS ? SetNamedSecurityInfoW((LPWSTR)path.c_str(),SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr) : built;
    if(acl)LocalFree(acl);FreeSid(system);return applied==ERROR_SUCCESS;
#else
    return ::chmod(path.c_str(),0700)==0;
#endif
}
std::string one_line(std::string s) {
    for(char& c:s)if(c=='\r'||c=='\n'||c=='\0')c=' ';return s;
}
std::string normalized_hex(std::string value) {
    value.erase(std::remove_if(value.begin(),value.end(),[](unsigned char c){return std::isspace(c)!=0;}),value.end());
    for(char& c:value)if(c>='A'&&c<='F')c=char(c-'A'+'a');
    return value;
}
Config comparable_config(Config c,bool gpu_active) {
    c.threads=bounded_worker_threads(c.threads);
    if(gpu_active){c.threads=1;if(c.chunk_bits<28)c.chunk_bits=28;}
    if(c.mode==TRAV_WAVE||c.mode==TRAV_ZONE||c.mode==TRAV_HYBRID)c.threads=1;
    if(c.mode==TRAV_ZONE&&c.zone_debug)c.debug_chunks=true;
    c.mask_hex=normalized_hex(c.mask_hex);c.wave_start=normalized_hex(c.wave_start);
    if(c.mode!=TRAV_SEQ||c.mask_hex.empty()){c.mask_hex.clear();c.mask_froz=0;}
    return c;
}
std::filesystem::path comparison_path(const std::string& value) {
    namespace fs=std::filesystem;std::error_code ec;
    const auto absolute=fs::absolute(fs::u8path(value),ec);
    if(ec)return fs::u8path(value).lexically_normal();
    const auto canonical=fs::weakly_canonical(absolute,ec);
    return ec?absolute.lexically_normal():canonical;
}
bool same_path(const std::string& a,const std::string& b) {
    const auto left=comparison_path(a),right=comparison_path(b);
#ifdef _WIN32
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;
#else
    return left==right;
#endif
}
std::string requested_found_path(const std::string& value) {
    // Match found_in_result without creating a directory or a file. Startup
    // falls back beside the program when RESULT could not be created.
    const bool bare=value.find_first_of("/\\:")==std::string::npos;
    std::error_code ec;
    return bare&&std::filesystem::is_directory("RESULT",ec)?"RESULT/"+value:value;
}
}

bool command_is_new(uint64_t revision,uint64_t last){return revision>last;}
bool settings_revision_matches(uint64_t actual,uint64_t expected,uint64_t snapshot){
    return expected>actual && expected==snapshot;
}
bool same_editable_settings(const Config& candidate,const Config& accepted_request,
                            const Config& effective,int current_power,bool gpu_active){
    Config a=comparable_config(candidate,gpu_active),b=comparable_config(accepted_request,gpu_active);
    if(a.gpu_power_pct==0)a.gpu_power_pct=current_power;
    b.gpu_power_pct=current_power;
    if(!same_path(a.pubkey_path,effective.pubkey_path)
        ||!same_path(requested_found_path(a.found_path),effective.found_path))return false;
    return std::tie(a.mode,a.threads,a.gpu_power_pct,a.progress_sec,a.check_sec,a.range_start_bit,a.range_end_bit,
        a.want_nk,a.chunk_bits,a.digit_hours,a.det_hex,a.debug_chunks,a.zone_nibbles,a.zone_debug,
        a.wave_nibbles,a.wave_start,a.hybrid_nibbles,a.hybrid_debug,a.mask_hex,a.mask_froz,a.tg_progress,a.signal_progress)
        ==std::tie(b.mode,b.threads,b.gpu_power_pct,b.progress_sec,b.check_sec,b.range_start_bit,b.range_end_bit,
        b.want_nk,b.chunk_bits,b.digit_hours,b.det_hex,b.debug_chunks,b.zone_nibbles,b.zone_debug,
        b.wave_nibbles,b.wave_start,b.hybrid_nibbles,b.hybrid_debug,b.mask_hex,b.mask_froz,b.tg_progress,b.signal_progress);
}
bool parse_settings_text(const std::string& text,const Config& defaults,Settings& output,std::string& error) {
    std::map<std::string,std::string> values;if(!ini(text,values,error))return false;
    Settings parsed;parsed.config=defaults;auto& c=parsed.config;
    if(!values.count("revision") || !natural(values["revision"],parsed.revision) || !parsed.revision) {
        error="A positive settings revision is required";return false;
    }
    for(const auto& pair:values){const auto& k=pair.first;const auto& v=pair.second;int* target=nullptr;int lo=0,hi=0;
        if(k=="revision")continue;
        if(k=="pubkey"||k=="pubkey_path"){c.pubkey_path=v;continue;}
        if(k=="found"||k=="found_path"){c.found_path=v;continue;}
        if(k=="mask_hex"){c.mask_hex=v;continue;}
        if(k=="wave_start"){c.wave_start=v;continue;}
        if(k=="mode") {
            const std::map<std::string,int> modes{{"random",TRAV_RANDOM},{"seq",TRAV_SEQ},{"kxe",TRAV_KXE},{"wave",TRAV_WAVE},{"zone",TRAV_ZONE},{"hybrid",TRAV_HYBRID}};
            auto m=modes.find(v);if(m!=modes.end())c.mode=m->second;
            else if(!integer(v,1,6,c.mode)){error="Unknown traversal mode";return false;}continue;
        }
        if(k=="digit_hours") {
            errno=0;char* end=nullptr;const double hours=std::strtod(v.c_str(),&end);
            if(v.empty()||errno==ERANGE||!end||*end||!std::isfinite(hours)||hours<0||hours>8760){error="digit_hours must be 0..8760";return false;}
            c.digit_hours=hours;continue;
        }
        if(k=="want_nk"||k=="debug"||k=="zone_debug"||k=="hybrid_debug"||k=="tg_progress"||k=="signal_progress"){
            int n;if(!integer(v,0,1,n)){error=k+" must be 0 or 1";return false;}
            if(k=="want_nk")c.want_nk=n!=0;else if(k=="debug")c.debug_chunks=n!=0;
            else if(k=="zone_debug")c.zone_debug=n!=0;else if(k=="hybrid_debug")c.hybrid_debug=n!=0;
            else if(k=="tg_progress")c.tg_progress=n!=0;else c.signal_progress=n!=0;continue;
        }
        if(k=="threads"){target=&c.threads;lo=1;hi=1024;}
        else if(k=="gpu_power"||k=="gpu_power_pct"){target=&c.gpu_power_pct;lo=50;hi=100;}
        else if(k=="progress"){target=&c.progress_sec;lo=1;hi=36000000;}
        else if(k=="check"){target=&c.check_sec;lo=1;hi=36000000;}
        else if(k=="range_start"){target=&c.range_start_bit;lo=0;hi=255;}
        else if(k=="range_end"){target=&c.range_end_bit;lo=1;hi=256;}
        else if(k=="chunk"){target=&c.chunk_bits;lo=10;hi=40;}
        else if(k=="det_hex"){
            if(v.size()==1 && v.find_first_not_of("abcdefABCDEF")==std::string::npos)c.det_hex=(v[0]|32)-'a'+10;
            else if(!integer(v,-1,15,c.det_hex)){error="det_hex must be -1 or a HEX digit";return false;}continue;
        }
        else if(k=="wave_nibbles"){target=&c.wave_nibbles;lo=3;hi=10;}
        else if(k=="zone_nibbles"){target=&c.zone_nibbles;lo=3;hi=15;}
        else if(k=="hybrid_nibbles"){target=&c.hybrid_nibbles;lo=3;hi=15;}
        else if(k=="mask_froz"){target=&c.mask_froz;lo=0;hi=64;}
        else {error="Unsupported settings key: "+k;return false;}
        if(!integer(v,lo,hi,*target)){error="Invalid setting: "+k;return false;}
    }
    if(c.pubkey_path.empty()||c.found_path.empty()){error="Input and results paths are required";return false;}
    if(c.range_end_bit<=c.range_start_bit){error="range_end must exceed range_start";return false;}
    if((!c.mask_hex.empty() && (!hex64(c.mask_hex)||c.mask_froz<1||c.mask_froz>64))
        ||(c.mask_hex.empty()&&c.mask_froz!=0)){error="mask_hex requires exactly 64 HEX characters and mask_froz 1..64";return false;}
    if(!c.wave_start.empty()&&!hex64(c.wave_start)){error="wave_start requires exactly 64 HEX characters";return false;}
    // These are the same admissible leading digits as the console range UI.
    if(c.det_hex>=0){const int shift=((c.range_end_bit-1)/4)*4;
        const int min_digit=c.range_start_bit>=shift ? 1<<(c.range_start_bit-shift) : 0;
        const int max_digit=(1<<(c.range_end_bit-shift))-1;
        if(c.det_hex<min_digit||c.det_hex>max_digit){error="Selected HEX digit does not occur in the range";return false;}}
    if(!effective_range(c,error))return false;
    output=std::move(parsed);error.clear();return true;
}
bool parse_command_text(const std::string& text,Command& output,std::string& error){
    std::map<std::string,std::string> values;if(!ini(text,values,error))return false;
    Command parsed;
    if(!values.count("revision")||!natural(values["revision"],parsed.revision)||!parsed.revision){error="A positive command revision is required";return false;}
    parsed.action=values["action"];
    if(parsed.action!="pause"&&parsed.action!="resume"&&parsed.action!="stop"&&parsed.action!="power"&&parsed.action!="apply"){
        error="Unknown control action";return false;}
    for(const auto& entry:values)if(entry.first!="revision"&&entry.first!="action"&&entry.first!="value"
        &&entry.first!="power"&&entry.first!="settings_revision"){error="Unsupported command key";return false;}
    if(parsed.action=="power"&&!integer(values.count("value")?values["value"]:values["power"],50,100,parsed.power)){
        error="Power must be 50..100";return false;}
    if(parsed.action=="apply"&&(!natural(values["settings_revision"],parsed.settings_revision)||!parsed.settings_revision)){
        error="Apply requires a settings revision";return false;}
    output=std::move(parsed);error.clear();return true;
}

bool Control::open(const std::string& directory,const std::string& executable_directory,const Config& defaults,std::string& error){
    namespace fs=std::filesystem;std::error_code ec;auto supplied=fs::u8path(directory);
    if(!supplied.is_absolute()){error="Pult session directory must be absolute";return false;}
    const auto requested_root=fs::u8path(executable_directory)/"gui_sessions";
    const auto root=fs::weakly_canonical(requested_root,ec);
    if(ec){error="Cannot resolve GUI session root";return false;}
    directory_=fs::canonical(supplied,ec);
    if(ec||directory_.parent_path()!=root||!fs::is_directory(directory_,ec)){
        error="Session must be a direct directory under the engine's gui_sessions";return false;}
    // No junction/symlink may redirect either private transport directory.
    for(const auto& part:{requested_root,supplied,root,directory_}){
#ifdef _WIN32
        const DWORD attrs=GetFileAttributesW(part.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT)){error="Reparse session directory rejected";return false;}
#else
        if(fs::is_symlink(fs::symlink_status(part,ec))){error="Symlink session directory rejected";return false;}
#endif
    }
    if(!protect_directory(directory_)){error="Cannot make GUI session private";return false;}
    std::string text;if(!read_text(directory_/"settings.ini",text,error)||!parse_settings_text(text,defaults,settings_,error))return false;
    started_=std::chrono::steady_clock::now();return true;
}
bool Control::read_settings(uint64_t revision,const Config& defaults,Settings& output,std::string& error){
    const auto immutable=directory_/numbered("settings_",revision);
    std::error_code ec;const auto path=std::filesystem::exists(immutable,ec)?immutable:directory_/"settings.ini";
    std::string text;if(!read_text(path,text,error)||!parse_settings_text(text,defaults,output,error))return false;
    if(!settings_revision_matches(settings_.revision,revision,output.revision)){error="Settings revision is stale or does not match apply";return false;}
    return true;
}
bool Control::next_command(Command& output,std::string& error){
    namespace fs=std::filesystem;std::error_code ec;std::map<uint64_t,fs::path> pending;
    for(fs::directory_iterator it(directory_,ec),end;!ec&&it!=end;it.increment(ec)){
        const auto name=it->path().filename().u8string();
        if(name.size()<13||name.rfind("command_",0)!=0||name.substr(name.size()-4)!=".ini")continue;
        uint64_t revision=0;if(natural(name.substr(8,name.size()-12),revision)&&command_is_new(revision,last_command_))pending[revision]=it->path();
    }
    if(ec){error="Cannot enumerate GUI commands";return false;}
    fs::path path;uint64_t named_revision=0;
    if(!pending.empty()){named_revision=pending.begin()->first;path=pending.begin()->second;}
    else {path=directory_/"commands.ini";if(!fs::exists(path,ec)){error.clear();return false;}}
    std::string text;
    if(!read_text(path,text,error)||!parse_command_text(text,output,error)){
        // A malformed immutable command is rejected once and cannot block Stop.
        if(named_revision)last_command_=named_revision;return false;
    }
    if(named_revision&&output.revision!=named_revision){last_command_=named_revision;error="Command filename revision mismatch";return false;}
    if(!command_is_new(output.revision,last_command_)){error.clear();return false;}
    error.clear();return true;
}
void Control::applied(const Settings& settings){
    if(settings.revision!=settings_.revision){initial_sample_ready_=false;current_sample_.clear();resume_available_=false;}
    settings_=settings;
}
bool Control::export_resume_settings(const Config& c,const std::string& checkpoint_identity,const std::string& checkpoint_path){
    resume_available_=false;
    if(!settings_.revision||c.mode<TRAV_RANDOM||c.mode>TRAV_HYBRID)return false;
    const bool exact=!checkpoint_identity.empty();
    if(exact&&(!hex64(checkpoint_identity)||checkpoint_path.empty()
        ||checkpoint_path.find_first_of("/\\\r\n\0",0,5)!=std::string::npos))return false;
    static const char* modes[]={"","random","seq","zone","kxe","wave","hybrid"};
    const uint64_t generation=resume_generation_+1;
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
    out<<"revision="<<settings_.revision<<"\nactual_revision="<<settings_.revision
        <<"\nresume_generation="<<generation<<"\ncheckpoint_policy="<<(exact?"exact":"sampling")<<'\n';
    if(exact)out<<"checkpoint_identity="<<checkpoint_identity<<"\ncheckpoint_path="<<checkpoint_path<<'\n';
    out<<"mode="<<modes[c.mode]<<"\npubkey="<<one_line(c.pubkey_path)<<"\nfound="<<one_line(c.found_path)
        <<"\nthreads="<<c.threads<<"\ngpu_power="<<c.gpu_power_pct
        <<"\nprogress="<<c.progress_sec<<"\ncheck="<<c.check_sec
        <<"\nrange_start="<<c.range_start_bit<<"\nrange_end="<<c.range_end_bit
        <<"\nwant_nk="<<(c.want_nk?1:0)<<"\nchunk="<<c.chunk_bits
        <<"\ndigit_hours="<<c.digit_hours<<"\ndet_hex="<<c.det_hex
        <<"\ndebug="<<(c.debug_chunks?1:0)<<"\nzone_nibbles="<<c.zone_nibbles
        <<"\nzone_debug="<<(c.zone_debug?1:0)<<"\nwave_nibbles="<<c.wave_nibbles
        <<"\nhybrid_nibbles="<<c.hybrid_nibbles<<"\nhybrid_debug="<<(c.hybrid_debug?1:0)
        <<"\nwave_start="<<one_line(c.wave_start)<<"\nmask_hex="<<one_line(c.mask_hex)
        <<"\nmask_froz="<<c.mask_froz<<"\ntg_progress="<<(c.tg_progress?1:0)
        <<"\nsignal_progress="<<(c.signal_progress?1:0)<<'\n';
    if(!write_atomic("resume_settings.ini",out.str()))return false;
    resume_generation_=generation;resume_available_=true;return true;
}
bool Control::write_atomic(const std::string& name,const std::string& text){
    const auto path=directory_/name,temporary=directory_/(name+".tmp");
    std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
    if(!output)return false;output.write(text.data(),static_cast<std::streamsize>(text.size()));output.flush();output.close();if(!output)return false;
#ifdef _WIN32
    for(int attempt=0;attempt<4;++attempt){
        if(MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
        const DWORD error=GetLastError();
        if(error!=ERROR_SHARING_VIOLATION&&error!=ERROR_ACCESS_DENIED)break;
        Sleep(5);
    }
    return false;
#else
    std::error_code ec;std::filesystem::rename(temporary,path,ec);return !ec;
#endif
}
void Control::status(const std::string& phase,uint64_t count,uint64_t found,int power,double rate,const std::string& sample,const std::string& error,
                     const std::string& stage,const std::string& detail){
    if(!sample.empty())current_sample_=sample;
    const auto elapsed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()-started_).count();
    std::ostringstream s;s<<std::fixed<<std::setprecision(3)<<"phase="<<phase<<"\nrate="<<rate<<"\ncount="<<count
        <<"\nfound="<<found<<"\npower="<<power<<"\nmode=";
    static const char* modes[]={"","random","seq","zone","kxe","wave","hybrid"};
    s<<modes[settings_.config.mode]<<"\nactual_revision="<<settings_.revision<<"\nlast_command="<<last_command_
        <<"\ncommand_revision="<<last_command_<<"\nnoop_revision="<<noop_revision_
        <<"\nrejected_revision="<<rejected_revision_<<"\nresume_generation="<<(resume_available_?resume_generation_:0)
        <<"\nelapsed_seconds="<<elapsed<<"\nerror="<<one_line(error)
        <<"\nstage="<<one_line(stage)<<"\ndetail="<<one_line(detail)
        <<"\ncurrent_sample_hex="<<current_sample_<<"\ninitial_sample_ready="<<(initial_sample_ready_?1:0)
        <<"\nsample_generation="<<settings_.revision<<"\npid=";
#ifdef _WIN32
    s<<GetCurrentProcessId();
#else
    s<<getpid();
#endif
    // A GUI read or a transient disk error must never terminate GPU work.
    s<<"\n";write_atomic("status.ini",s.str());
}
bool Control::initial_sample(const std::map<std::string,std::string>& fields){
    if(initial_sample_ready_)return true;
    std::ostringstream s;s<<"sample_generation="<<settings_.revision<<"\n";
    for(const auto& pair:fields)s<<pair.first<<'='<<one_line(pair.second)<<'\n';
    initial_sample_ready_=write_atomic("sample_initial.txt",s.str());return initial_sample_ready_;
}
}
