// Reuse the exact result/point/WIF/archive assertions; the old test entry point
// is not run. This fixture adds a deterministic blocked optional job and a real
// local metadata-command receiver, with no network credentials or GPU code.
#ifndef PKH_FOUND_POSTPROCESS_TEST
#error Compile only the isolated notify object and this fixture with the test hook.
#endif
#define main unused_plain_found_entry
#include "plain_found_test.cpp"
#undef main
#include <condition_variable>
#include <exception>

namespace {
struct JobGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered=false,released=false;
    void block() {
        std::unique_lock<std::mutex> lock(mutex);entered=true;changed.notify_all();
        changed.wait(lock,[&]{return released;});
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);released=true;changed.notify_all();
    }
    ~JobGate(){release();set_found_postprocess_test_hook({});(void)wait_found_postprocessing(30000);}
};
void blocked_optional_work(const std::filesystem::path& executable) {
    setup("blocked_optional_job",TRAV_WAVE);g_cfg.want_nk=false;
    const auto capture=std::filesystem::absolute("metadata-only-notices.txt");
    const auto wrong_capture=std::filesystem::absolute("changed-settings-must-not-be-used.txt");
    const auto capture_command=[&](const std::filesystem::path& path) {
        return plat_quote(executable.u8string())+" --capture-notice "+plat_quote(path.u8string());
    };
    g_cfg.notify_cmd=capture_command(capture);
    ChunkIdx ordinal;u256 start;uint64_t offered=0;int phase=-1;std::mt19937_64 random(7137);
    require(next_chunk(ordinal,start,offered,phase,random)&&offered>=16,"actual wave interval has work after the first hit");
    u256 first,later;u256_add_u64(first,start,3);u256_add_u64(later,start,13);
    const Point first_point=ec_mul_G(first),later_point=ec_mul_G(later);
    require(g_db.add_hex(public_key(first_point,true))&&g_db.add_hex(public_key(later_point,false)),"two synthetic early/later public targets accepted");
    g_db.build();require(push_targets_to_backend(),"actual CPU backend receives continuation targets");
    const auto expected_first=expected_record(first,first_point,1),expected_later=expected_record(later,later_point,1);
    JobGate gate;set_found_postprocess_test_hook([&]{gate.block();});
    scan_chunk(start,8);scanned+=8;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        require(gate.changed.wait_for(lock,std::chrono::seconds(3),[&]{return gate.entered;}),"optional job enters controlled background gate");
    }
    require(g_found_num.load()==1&&g_count.load()==8&&!g_stop.load(),"first durable hit and scan return while optional work remains blocked");
    require(pending_found_postprocessing()==1,"running optional job included in pending count");
    verify_fields(read(std::filesystem::u8path(g_cfg.found_path)),expected_first);
    require(plat_fsync_file(g_cfg.found_path),"first full plaintext record already durable before optional work");
    require(!std::filesystem::exists(std::filesystem::path("RESULT")/(expected_first.addr_c+".7z")),"archive work really has not run before the gate releases");

    std::atomic<bool> second_done{false};std::exception_ptr error;
    u256 remainder;u256_add_u64(remainder,start,8);
    std::thread second([&]{try{scan_chunk(remainder,8);}catch(...){error=std::current_exception();}second_done.store(true,std::memory_order_release);});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!second_done.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool progressed_before_release=second_done.load(std::memory_order_acquire);
    if(!progressed_before_release)gate.release(); // never strand a failed regression
    second.join();if(error)std::rethrow_exception(error);scanned+=8;
    require(progressed_before_release,"later CPU scan completes before blocked optional work is released");
    require(g_count.load()==16&&g_found_num.load()==2&&!g_stop.load()&&g_stop_flag==0,"later candidate counted and saved while optional worker is blocked");
    const auto saved=read(std::filesystem::u8path(g_cfg.found_path));
    require(occurrences(saved,"=== НАЙДЕНО #")==2&&saved.find(expected_first.priv_hex)!=std::string::npos&&saved.find(expected_later.priv_hex)!=std::string::npos,"both complete private records durable before archive/notification work resumes");
    require(pending_found_postprocessing()==2,"running and queued optional jobs both counted");
    require(report_found_in_range(first,first_point.x,first_point.y,1,g_range_start,g_range_end),"duplicate durable result remains accepted while optional queue is blocked");
    require(g_found_num.load()==2&&pending_found_postprocessing()==2&&read(std::filesystem::u8path(g_cfg.found_path))==saved,"duplicate hit neither rewrites Found nor enqueues optional work twice");
    require(!wait_found_postprocessing(10),"bounded drain times out while optional work is intentionally blocked");
    require(!std::filesystem::exists(capture),"no metadata command runs before optional worker gate releases");

    // Queued work must retain its command snapshot, not read mutable settings.
    g_cfg.notify_cmd=capture_command(wrong_capture);
    const auto original_directory=std::filesystem::current_path();
    const auto changed_directory=fixture_root/"changed-working-directory";
    require(std::filesystem::create_directory(changed_directory),"separate empty directory for queued-path snapshot check");
    std::filesystem::current_path(changed_directory);
    gate.release();require(wait_found_postprocessing(30000),"optional queue drains after release");
    std::filesystem::current_path(original_directory);
    set_found_postprocess_test_hook({});
    require(pending_found_postprocessing()==0&&pending_background_notifications()==0,"all optional and notification work finished");
    require(!std::filesystem::exists(wrong_capture),"queued work uses its original immutable settings snapshot");
    require(std::filesystem::is_empty(changed_directory),"queued artifacts/log/notification commands retain original absolute paths and working directory");
    const auto notices=normalized(read(capture));
    std::istringstream input(notices);std::string text,file;size_t received=0;
    while(std::getline(input,text)) {
        require(bool(std::getline(input,file)),"metadata receiver got an attachment field");++received;
        require(text.find("PubKEYS")!=std::string::npos&&!text.empty(),"external receiver gets finding status text");
        require(file.empty(),"external metadata notification has no attachment");
        for(const auto& value:{expected_first,expected_later})
            require(text.find(value.priv_hex)==std::string::npos&&text.find(value.wif_c)==std::string::npos&&text.find(value.wif_u)==std::string::npos&&text.find(value.addr_c)==std::string::npos,"external payload contains no private/WIF or result/card/archive identifier");
        require(text.find(".png")==std::string::npos&&text.find(".7z")==std::string::npos,"external payload contains no card or archive path");
    }
    require(received==2,"one metadata-only local notification for each durable finding");
    // verify_saved also tests exact point/WIF data, plaintext Found, unencrypted
    // archive test/list/extract, durable dedup, no stop and no live files.
    verify_saved({expected_first,expected_later});
}
void failed_optional_artifacts() {
    setup("unavailable_optional_directory",TRAV_ZONE);g_cfg.want_nk=false;
    g_cfg.found_path=(std::filesystem::current_path()/"durable-local-found.txt").u8string();
    {std::ofstream blocker("RESULT",std::ios::binary);blocker<<"isolated optional-directory obstruction";require(bool(blocker),"test obstruction written only in fresh fixture");}
    u256 first,later;u256_add_u64(first,g_range_start,3);u256_add_u64(later,g_range_start,13);
    const Point first_point=ec_mul_G(first),later_point=ec_mul_G(later);
    require(g_db.add_hex(public_key(first_point,true))&&g_db.add_hex(public_key(later_point,false)),"optional-failure synthetic targets accepted");
    g_db.build();require(push_targets_to_backend(),"optional-failure targets delivered to actual CPU backend");
    const auto a=expected_record(first,first_point,1),b=expected_record(later,later_point,1);
    scan_chunk(g_range_start,16);scanned+=16;
    require(g_count.load()==16&&g_found_num.load()==2&&!g_stop.load()&&g_stop_flag==0,"optional output obstruction does not stop later candidates or durable findings");
    require(wait_found_postprocessing(30000),"failed optional jobs are bounded and drain");
    const auto saved=read(std::filesystem::u8path(g_cfg.found_path));
    require(occurrences(saved,"=== НАЙДЕНО #")==2,"both full private records survive optional output failure");
    for(const auto& expected:{a,b}) {
        const auto key_at=saved.find("PRIVATE KEY    : "+expected.priv_hex),begin=saved.rfind("=== НАЙДЕНО #",key_at),end=saved.find("=== НАЙДЕНО #",key_at);
        require(key_at!=std::string::npos&&begin!=std::string::npos,"failed optional job retains exact private record");
        verify_fields(saved.substr(begin,end==std::string::npos?std::string::npos:end-begin),expected);
    }
    require(plat_fsync_file(g_cfg.found_path),"full result remains durable despite unavailable optional directory");
    require(std::filesystem::is_regular_file("RESULT"),"optional job never replaces the isolated obstruction");
    const auto log=read("notify.log");
    require(occurrences(log,"optional local key card unavailable")>=2&&occurrences(log,"optional local archive unavailable")>=2,"optional artifact errors recorded without rejecting saved results");
    require(log.find(a.priv_hex)==std::string::npos&&log.find(b.priv_hex)==std::string::npos&&log.find(a.wif_c)==std::string::npos&&log.find(b.wif_c)==std::string::npos,"optional failure log contains no private key or WIF");
    require(pending_found_postprocessing()==0&&pending_background_notifications()==0,"failed optional work leaves no pending jobs or network workers");
}
}
int main(int argc,char**argv) {
    if(argc>=2&&std::string(argv[1])=="--capture-notice") {
        if(argc!=5)return 5;
        std::ofstream output(std::filesystem::u8path(argv[2]),std::ios::binary|std::ios::app);
        output<<argv[3]<<'\n'<<argv[4]<<'\n';output.flush();return output?0:6;
    }
    SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS);SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
    try {
        require(argc==2,"supply fresh isolated fixture directory");const auto executable=std::filesystem::absolute(std::filesystem::u8path(argv[0]));
        fixture_root=std::filesystem::absolute(std::filesystem::u8path(argv[1]));
        require(std::filesystem::is_directory(fixture_root)&&std::filesystem::is_empty(fixture_root),"async fixture directory exists and is empty");
        {Silence silence;std::filesystem::current_path(fixture_root);init_colors();build_table();blocked_optional_work(executable);failed_optional_artifacts();}
        std::printf("PASS: async finding continuation checks=%llu cases=%llu archives=%llu scanned=%llu; blocked optional worker, first and later durable private records/counts before release, exact queue count/timeout/drain, immutable settings, real local metadata receiver with no secret/attachment, unavailable optional artifacts preserve both full results; network=0 GPU=0\n",
            static_cast<unsigned long long>(checks),static_cast<unsigned long long>(cases),static_cast<unsigned long long>(archives),static_cast<unsigned long long>(scanned));return 0;
    }catch(const std::exception& failure){std::fprintf(stderr,"FAIL: async finding continuation: %s\n",failure.what());return 1;}
}
