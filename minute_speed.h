#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <algorithm>
#include <string>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <functional>
#include <utility>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif
// Main search thread only. The status occupies an unfinished console line.
// LogScope removes it before messages and restores it below complete lines.
inline int power_after_key(int current, int key) {
    const int delta = (key == '+' || key == '=') ? 5 : (key == '-' || key == '_') ? -5 : 0;
    return (std::max)(50, (std::min)(100, current + delta));
}

class MinuteSpeed {
public:
    using Clock = std::chrono::steady_clock;
    using Counters = std::pair<uint64_t,uint64_t>;
    using CounterProvider = std::function<Counters()>;
    using SampleProvider = std::function<std::string()>;
    explicit MinuteSpeed(Clock::time_point now, int power_pct = 100,
                         const char* log_path = nullptr, const std::string& context = {},
                         CounterProvider counters = {}, SampleProvider sample = {})
        : last_(now), power_pct_(power_pct), log_path_(log_path ? log_path : ""),
          counters_(std::move(counters)),sample_(std::move(sample)) {
#ifdef _WIN32
        output_ = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode=0; enabled_=GetConsoleMode(output_, &mode)!=0;
#endif
        measured_at_=timestamp();
        log_event("start", 0, 0, 0, context);
        refresh_sample();
        refresh_live_counts();
        active_=this; show();
    }
    ~MinuteSpeed() noexcept {
        try { finish(); } catch (...) {}
        if(active_==this)active_=nullptr;
    }
    void finish(){ finish_impl(true); }
    void finish(uint64_t completed, uint64_t found) {
        // Short runs can finish before the first 30-second sample. Preserve
        // their real final counters without publishing a fabricated rate.
        if (!finished_) { displayed_ = completed; found_ = found; }
        finish_impl(false);
    }
    void pause(Clock::duration duration){last_+=duration;}
    void set_paused(bool paused, bool pending=false){
        hide(); paused_=paused; pending_=pending;refresh_live_counts();
        log_event(paused ? (pending ? "pause_pending" : "pause") : "resume",0,displayed_,0);
        refresh_text(); show();
    }
    void set_power(int power_pct){hide();power_pct_=power_pct;refresh_live_counts();log_event("power",0,displayed_,0);refresh_text();show();}
    void update(Clock::time_point now,uint64_t completed,uint64_t found=0){
        if(!enabled_ && log_path_.empty())return;
        double seconds=std::chrono::duration<double>(now-last_).count();
        if(seconds<30.0)return;
        double rate=static_cast<double>(completed-previous_)/seconds;
        hide();
        measured_at_=timestamp();
        log_event("sample",seconds,completed,rate);
        rate_text_=format_rate(rate);
        previous_=completed;
        displayed_=completed;
        found_=found;
        refresh_sample();
        refresh_text();
        last_=now;show(true);
    }
    static std::wstring format_rate(double rate){
        wchar_t digits[64];
#ifdef _WIN32
        swprintf_s(digits,L"%.0f",rate);
#else
        std::swprintf(digits,64,L"%.0f",rate);
#endif
        std::wstring number=digits,grouped;
        for(size_t i=0;i<number.size();++i){
            if(i && (number.size()-i)%3==0)grouped+=L' ';
            grouped+=number[i];
        }
        return L"\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c: "+grouped+L" Key/S";
    }
    static std::wstring format_status(int power_pct, const std::wstring& rate){
        return L"\u041c\u043e\u0449\u043d\u043e\u0441\u0442\u044c: "+std::to_wstring(power_pct)+L"% | "+rate;
    }
    static std::wstring format_count(uint64_t value){
        const auto digits=std::to_wstring(value);
        std::wstring grouped;
        for(size_t i=0;i<digits.size();++i){
            if(i && (digits.size()-i)%3==0)grouped+=L' ';
            grouped+=digits[i];
        }
        return grouped;
    }
    static std::wstring format_status(int power_pct, const std::wstring& rate,
                                     uint64_t completed,uint64_t found){
        return format_status(power_pct,rate)+L" | Ключей: "+format_count(completed)+
            L" | Найдено: "+format_count(found);
    }
    struct LogScope {
        MinuteSpeed* owner=active_;
        LogScope(){if(owner){owner->hide();++owner->suspended_;}}
        void finish() noexcept {
            // Detach before optional rendering: an allocation failure here
            // must not escape a destructor while a finding is being saved.
            auto* previous=owner;owner=nullptr;
            if(previous){
                --previous->suspended_;
                try{previous->refresh_live_counts();previous->show();}catch(...){}
            }
        }
        void resume(){if(!owner){owner=active_;if(owner){owner->hide();++owner->suspended_;}}}
        ~LogScope() noexcept {finish();}
        LogScope(const LogScope&)=delete;
        LogScope& operator=(const LogScope&)=delete;
    };
    int print(FILE* stream,const char* format,...){
        LogScope scope;va_list args;va_start(args,format);
        int result=std::vfprintf(stream,format,args);va_end(args);std::fflush(stream);return result;
    }
    MinuteSpeed(const MinuteSpeed&)=delete;
    MinuteSpeed& operator=(const MinuteSpeed&)=delete;
private:
    void refresh_sample(){
        // Sample only at construction and real 30-second measurements. Log
        // restoration and power/pause changes reuse this local-only cache.
        if(!sample_)return;
        try{
            std::string key=sample_();
            if(key.size()!=64)return;
            for(char& digit:key){
                if(digit>='A'&&digit<='F')digit=char(digit-'A'+'a');
                if(!((digit>='0'&&digit<='9')||(digit>='a'&&digit<='f')))return;
            }
            private_sample_.assign(key.begin(),key.end());
        }catch(...){} // An unavailable display sample cannot interrupt search.
    }
    void refresh_live_counts(){
        // Restoring a log line must show current totals before the first rate
        // sample too. This display-only refresh never moves previous_ or last_.
        if(counters_)try{const auto counts=counters_();displayed_=counts.first;found_=counts.second;}catch(...){}
        refresh_text();
    }
    void finish_impl(bool refresh_counts){
        hide();
        if(!finished_){
            if(refresh_counts)refresh_live_counts();
            log_event("stop",0,displayed_,0);finished_=true;
        }
        enabled_=false;if(active_==this)active_=nullptr;
    }
    static std::string timestamp(){
        const std::time_t now=std::time(nullptr); std::tm local{};
#ifdef _WIN32
        if(localtime_s(&local,&now)!=0)return "time unavailable";
#else
        if(!localtime_r(&now,&local))return "time unavailable";
#endif
        char value[32]={};
        std::strftime(value,sizeof(value),"%Y-%m-%d %H:%M:%S",&local);
        return value;
    }
    void log_event(const char* event,double seconds,uint64_t completed,double rate,
                   const std::string& context={}){
        if(log_path_.empty() || log_failed_)return;
        try{
            const std::filesystem::path path=std::filesystem::u8path(log_path_);
            if(path.has_parent_path())std::filesystem::create_directories(path.parent_path());
            // Bound the log to two 10 MiB files; never touch the findings file.
            if(std::filesystem::exists(path) && std::filesystem::file_size(path)>=10*1024*1024){
                auto previous=path; previous+=".previous";
                std::filesystem::remove(previous);
                std::filesystem::rename(path,previous);
            }
            const bool header=!std::filesystem::exists(path) || std::filesystem::file_size(path)==0;
            std::ofstream log(path,std::ios::app);
            if(header)log<<"local_time,event,power_percent,interval_seconds,total_keys,keys_per_second,context\n";
            log<<timestamp()<<','<<event<<','<<power_pct_<<','<<std::fixed<<std::setprecision(3)
               <<seconds<<','<<completed<<','<<std::setprecision(0)<<rate<<','<<context<<'\n';
            log.flush();
            if(!log)throw std::runtime_error("performance log write failed");
        }catch(...){
            log_failed_=true;
            std::fprintf(stderr,"\nPubKEYS: cannot write performance log: %s\n",log_path_.c_str());
        }
    }
    void refresh_text(){
#ifdef _WIN32
        std::wstring formatted=L"["+std::wstring(measured_at_.begin(),measured_at_.end())+L"] "+
            format_status(power_pct_, paused_ ? (pending_ ? L"Ожидание паузы..." : L"ПАУЗА — вычисления приостановлены") : rate_text_,
                          displayed_,found_);
        if(sample_)formatted+=L"\r\nPRIVATE KEY   : "+private_sample_;
        wcscpy_s(text_,formatted.c_str());
#endif
    }
#ifdef _WIN32
    bool erase_reflowed_frame(const CONSOLE_SCREEN_BUFFER_INFO& info){
        // Read native cells: ReadConsoleOutputCharacterW omits trailing cells
        // of wide glyphs, so its string offsets cannot identify cursor cells.
        constexpr unsigned capacity=8192;
        const unsigned width=unsigned(info.dwSize.X);
        if(!width||width>capacity)return false;
        const unsigned text_length=unsigned(std::wcslen(text_));
        const unsigned old_rows=unsigned((std::max)(0,int(end_.Y)-int(anchor_.Y)))+4;
        const unsigned rows=(std::min)(capacity/width,
            (std::max)(old_rows,2*text_length/width+8));
        const SHORT top=SHORT((std::max)(0,int(info.dwCursorPosition.Y)-int(rows)+1));
        const SHORT height=SHORT(info.dwCursorPosition.Y-top+1);
        CHAR_INFO cells[capacity]{};
        SMALL_RECT rectangle{0,top,SHORT(width-1),info.dwCursorPosition.Y};
        if(!ReadConsoleOutputW(output_,cells,COORD{SHORT(width),height},COORD{0,0},&rectangle)||
           rectangle.Top!=top||rectangle.Bottom!=info.dwCursorPosition.Y||
           rectangle.Left!=0||rectangle.Right!=SHORT(width-1))return false;
        const unsigned cursor=unsigned(info.dwCursorPosition.Y-top)*width+
            unsigned(info.dwCursorPosition.X);
        // A deferred wrap can leave the cursor on the last written cell.
        const unsigned limit=cursor+(unsigned(info.dwCursorPosition.X)==width-1?1:0);
        wchar_t actual[capacity]{},expected[512]{};unsigned positions[capacity]{};
        unsigned actual_size=0,expected_size=0;
        for(unsigned i=0;i<limit;++i){
            if(cells[i].Attributes&COMMON_LVB_TRAILING_BYTE)continue;
            const wchar_t c=cells[i].Char.UnicodeChar;
            if(!c||c==L' '||c==L'\r'||c==L'\n')continue;
            actual[actual_size]=c;positions[actual_size++]=i;
        }
        for(unsigned i=0;i<text_length;++i){const wchar_t c=text_[i];
            if(c!=L' '&&c!=L'\r'&&c!=L'\n')expected[expected_size++]=c;
        }
        if(!expected_size)return false;
        const std::wstring_view observed(actual,actual_size),wanted(expected,expected_size);
        const auto at=observed.rfind(wanted);
        // The entire cached status AND private row must match, with no foreign
        // nonblank text inside the frame or after it before the current cursor.
        if(at==std::wstring_view::npos||at+expected_size!=actual_size)return false;
        const unsigned first=positions[at],last=positions[at+expected_size-1]+1;
        if(last>cursor+1)return false;
        const COORD start{SHORT(first%width),SHORT(top+first/width)};
        DWORD erased=0;
        if(!FillConsoleOutputCharacterW(output_,L' ',last-first,start,&erased)||
           erased!=last-first)return false;
        SetConsoleCursorPosition(output_,start);
        return true;
    }
    bool erase_resized_frame(const CONSOLE_SCREEN_BUFFER_INFO& info){
        if(erase_reflowed_frame(info))return true;
        // Classic console buffer resize clips each old physical row instead
        // of reflowing it. Recognize that exact frame before erasing anything;
        // unrelated log text or an unrecognized reflow is left untouched.
        if(info.dwSize.X<16)return false;
        struct Run{COORD at{};DWORD length=0;const wchar_t* text=nullptr;};
        Run runs[512]{};unsigned count=0;
        int x=anchor_.X,y=anchor_.Y;DWORD begin=0,length=0;
        auto flush=[&]{
            if(length){runs[count++]={{static_cast<SHORT>(x-int(length)),static_cast<SHORT>(y)},length,text_+begin};length=0;}
        };
        for(DWORD i=0;text_[i];++i){
            if(text_[i]==L'\r'){flush();x=0;continue;}
            if(text_[i]==L'\n'){flush();++y;continue;}
            if(!length)begin=i;
            ++length;++x;
            if(x==width_){flush();x=0;++y;}
        }
        flush();
        wchar_t observed[512]{};unsigned visible_runs=0;
        for(unsigned i=0;i<count;++i){
            auto& run=runs[i];
            if(run.at.Y<0||run.at.Y>=info.dwSize.Y||run.at.X>=info.dwSize.X){run.length=0;continue;}
            run.length=(std::min)(run.length,DWORD(info.dwSize.X-run.at.X));
            DWORD read=0;
            if(!ReadConsoleOutputCharacterW(output_,observed,run.length,run.at,&read)||
               read!=run.length||std::wmemcmp(observed,run.text,run.length)!=0)return false;
            ++visible_runs;
        }
        if(!visible_runs)return false;
        for(unsigned i=0;i<count;++i)if(runs[i].length){DWORD erased=0;
            FillConsoleOutputCharacterW(output_,L' ',runs[i].length,runs[i].at,&erased);
        }
        // Rewind only if the cursor is still at our former endpoint, possibly
        // clamped by the narrower buffer. Never move it over a later log.
        const SHORT expected_x=(std::min)(end_.X,SHORT(info.dwSize.X-1));
        const SHORT expected_y=(std::min)(end_.Y,SHORT(info.dwSize.Y-1));
        if(info.dwCursorPosition.X==expected_x&&info.dwCursorPosition.Y==expected_y&&
           anchor_.Y>=0&&anchor_.Y<info.dwSize.Y)
            SetConsoleCursorPosition(output_,anchor_);
        return true;
    }
#endif
    void hide(){
#ifdef _WIN32
        if(!visible_)return;
        std::fflush(stdout);CONSOLE_SCREEN_BUFFER_INFO info{};DWORD count=0;
        // Do not erase unrelated output if another writer moved the cursor.
        if(GetConsoleScreenBufferInfo(output_,&info)){
            if(info.dwSize.X!=width_){(void)erase_resized_frame(info);}
            else if(info.dwCursorPosition.Y==end_.Y&&info.dwCursorPosition.X==end_.X){
                const DWORD cells=(end_.Y-anchor_.Y)*width_+end_.X-anchor_.X;
                FillConsoleOutputCharacterW(output_,L' ',cells,anchor_,&count);
                SetConsoleCursorPosition(output_,anchor_);
            }
        }
        visible_=false;
#endif
    }
    void show(bool recover_line=false){
#ifdef _WIN32
        if(!enabled_||visible_||suspended_)return;
        std::fflush(stdout);CONSOLE_SCREEN_BUFFER_INFO info{};
        if(!GetConsoleScreenBufferInfo(output_,&info)||info.dwSize.X<2)return;
        if(info.dwCursorPosition.X!=0){
            // A resized/reflowed line or an unrelated writer can invalidate
            // anchor_. Recover at the next measurement without erasing it.
            // LogScope still waits for a complete log line before restoring.
            if(!recover_line)return;
            DWORD count=0;
            if(!WriteConsoleW(output_,L"\r\n",2,&count,nullptr)||
               !GetConsoleScreenBufferInfo(output_,&info))return;
        }
        anchor_=info.dwCursorPosition;
        width_=info.dwSize.X;
        DWORD length=(DWORD)wcslen(text_);
        DWORD written=0;
        if(WriteConsoleW(output_,text_,length,&written,nullptr)&&
           GetConsoleScreenBufferInfo(output_,&info)){
            // Console columns need not equal the number of UTF-16 characters.
            end_=info.dwCursorPosition;
            // Count explicit CR/LF as cursor movement, not printable cells.
            // Both logical rows may also wrap before scrolling the buffer.
            int expected_y=anchor_.Y,expected_x=anchor_.X;
            for(DWORD i=0;i<written;++i){
                if(text_[i]==L'\r')expected_x=0;
                else if(text_[i]==L'\n')++expected_y;
                else if(++expected_x==width_){expected_x=0;++expected_y;}
            }
            anchor_.Y-=static_cast<SHORT>((std::max)(0,expected_y-int(end_.Y)));
            if(anchor_.Y>=0)visible_=true;
        }
#endif
    }
    inline static MinuteSpeed* active_=nullptr;
    Clock::time_point last_;uint64_t previous_=0,displayed_=0,found_=0;bool enabled_=false;int suspended_=0;
    int power_pct_=100; bool paused_=false, pending_=false;
    std::string log_path_,measured_at_; bool log_failed_=false,finished_=false;
    std::wstring rate_text_=L"\u0421\u043a\u043e\u0440\u043e\u0441\u0442\u044c: \u2014 Key/S";
    CounterProvider counters_;
    SampleProvider sample_;
    std::wstring private_sample_=L"\u2014";
#ifdef _WIN32
    HANDLE output_=INVALID_HANDLE_VALUE;COORD anchor_{},end_{};
    wchar_t text_[512]={};bool visible_=false;SHORT width_=0;
#endif
};
