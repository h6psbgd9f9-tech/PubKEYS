// Uses only an inactive screen buffer inside this test's own hidden console.
// No user console, input, application search, GPU or network is accessed.
#include "minute_speed.h"
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks=0,failures=0;
std::ofstream report;
void check(bool okay,const char* label){++checks;if(!okay)++failures;report<<(okay?"PASS ":"FAIL ")<<label<<'\n';report.flush();}
void demand(bool okay,const char* label){if(!okay)throw std::runtime_error(label);}
std::wstring wide(const std::string& text){return {text.begin(),text.end()};}
size_t occurrences(const std::wstring& text,const std::wstring& token){size_t count=0,at=0;while((at=text.find(token,at))!=std::wstring::npos){++count;at+=token.size();}return count;}
const std::wstring prefix=L"PRIVATE KEY   : ";
const std::string key_a=std::string(60,'0')+"ABCD",key_b=std::string(63,'0')+"2",key_c=std::string(63,'0')+"3";
const std::wstring key_a_lower=wide(std::string(60,'0')+"abcd");
class Screen {
    HANDLE saved_=GetStdHandle(STD_OUTPUT_HANDLE);
public:
    HANDLE handle=CreateConsoleScreenBuffer(GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,CONSOLE_TEXTMODE_BUFFER,nullptr);
    Screen(){demand(handle!=INVALID_HANDLE_VALUE,"create own inactive console buffer");demand(SetStdHandle(STD_OUTPUT_HANDLE,handle)!=FALSE,"select inactive test output handle");}
    ~Screen(){SetStdHandle(STD_OUTPUT_HANDLE,saved_);if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    void size(SHORT width,SHORT height){
        SMALL_RECT viewport{0,0,19,4};demand(SetConsoleWindowInfo(handle,TRUE,&viewport)!=FALSE,"shrink only private viewport");
        demand(SetConsoleScreenBufferSize(handle,COORD{width,height})!=FALSE,"size only private buffer");
    }
    CONSOLE_SCREEN_BUFFER_INFO info()const{CONSOLE_SCREEN_BUFFER_INFO value{};demand(GetConsoleScreenBufferInfo(handle,&value)!=FALSE,"read own buffer information");return value;}
    void clear(SHORT width=240,SHORT height=100){size(width,height);DWORD written=0;demand(FillConsoleOutputCharacterW(handle,L' ',DWORD(width)*height,COORD{0,0},&written)!=FALSE,"clear own buffer");demand(SetConsoleCursorPosition(handle,COORD{0,0})!=FALSE,"position own cursor");}
    void cursor(SHORT x,SHORT y){demand(SetConsoleCursorPosition(handle,COORD{x,y})!=FALSE,"move only test cursor");}
    void write(const std::wstring& value){DWORD written=0;demand(WriteConsoleW(handle,value.data(),DWORD(value.size()),&written,nullptr)!=FALSE&&written==value.size(),"write synthetic test log");}
    std::vector<CHAR_INFO> cells()const{const auto value=info();std::vector<CHAR_INFO> result(size_t(value.dwSize.X)*value.dwSize.Y);SMALL_RECT region{0,0,SHORT(value.dwSize.X-1),SHORT(value.dwSize.Y-1)};demand(ReadConsoleOutputW(handle,result.data(),value.dwSize,COORD{0,0},&region)!=FALSE,"read own inactive physical cells");return result;}
    std::wstring contents()const{const auto physical=cells();std::wstring result;result.reserve(physical.size());for(const auto& cell:physical)result+=cell.Char.UnicodeChar?cell.Char.UnicodeChar:L' ';return result;}
    size_t nonempty_rows()const{const auto value=info();const auto text=contents();size_t count=0;for(SHORT y=0;y<value.dwSize.Y;++y)if(text.substr(size_t(y)*value.dwSize.X,value.dwSize.X).find_first_not_of(L" \0",0,2)!=std::wstring::npos)++count;return count;}
};
void cached_sample_and_csv(Screen& screen,const std::filesystem::path& csv){
    screen.clear();std::string sample;unsigned calls=0;bool throws=false;uint64_t live_count=0,live_found=0;
    const auto start=MinuteSpeed::Clock::now();
    {
        MinuteSpeed status(start,100,csv.u8string().c_str(),"test=status-private-sample",[&]{return MinuteSpeed::Counters{live_count,live_found};},[&]{++calls;if(throws)throw std::runtime_error("synthetic provider unavailable");return sample;});
        check(calls==1,"provider read once at construction");
        check(screen.contents().find(prefix+L"\u2014")!=std::wstring::npos,"placeholder before a valid current interval sample");
        check(screen.nonempty_rows()==2&&screen.info().dwCursorPosition.Y==1,"wide console has exactly two occupied logical rows");
        sample=key_a;live_count=100;live_found=1;
        {MinuteSpeed::LogScope log;screen.write(L"unrelated log retained\r\n");}
        check(calls==1&&screen.contents().find(prefix+L"\u2014")!=std::wstring::npos,"log restoration does not fetch or change the sample");
        status.set_power(65);status.set_paused(true);status.set_paused(false);
        check(calls==1&&screen.contents().find(prefix+L"\u2014")!=std::wstring::npos,"power and pause restoration do not fetch the sample");
        for(int tick=1;tick<600;++tick)status.update(start+std::chrono::milliseconds(tick*50),uint64_t(tick)*50,1);
        check(calls==1&&screen.contents().find(key_a_lower)==std::wstring::npos,"599 simulated 50ms monitor ticks make zero sample-provider calls before a full 30-second interval");
        status.update(start+std::chrono::seconds(30),30000,1);
        check(calls==2&&screen.contents().find(prefix+key_a_lower)!=std::wstring::npos,"accepted 30-second update shows exactly 64 lowercase hex digits including leading zeros");
        check(occurrences(screen.contents(),prefix)==1,"first sample row has no history trail");
        sample=key_b;live_count=31000;live_found=2;status.set_power(75);
        {MinuteSpeed::LogScope log;screen.write(L"second log retained\r\n");}
        status.update(start+std::chrono::seconds(31),31000,2);
        check(calls==2&&screen.contents().find(prefix+key_a_lower)!=std::wstring::npos&&screen.contents().find(wide(key_b))==std::wstring::npos,"counter and log refresh keep the cached sample between measurement intervals");
        live_count=90000;status.update(start+std::chrono::seconds(60),90000,2);
        check(calls==3&&screen.contents().find(prefix+wide(key_b))!=std::wstring::npos,"second accepted measurement refreshes the sample");
        check(screen.contents().find(key_a_lower)==std::wstring::npos&&occurrences(screen.contents(),prefix)==1,"previous private sample erased instead of appended");
        check(screen.contents().find(L"2 000 Key/S")!=std::wstring::npos,"log and power refresh preserve the rate measurement baseline");
        {
            MinuteSpeed::LogScope outer;screen.write(L"partial log");
            {MinuteSpeed::LogScope inner;sample=key_c;live_count=180000;status.update(start+std::chrono::seconds(90),180000,3);}
            check(screen.contents().find(prefix)==std::wstring::npos,"nested log scopes keep both status rows hidden during an unfinished log");
            screen.write(L" complete\r\n");
        }
        check(calls==4&&screen.contents().find(prefix+wide(key_c))!=std::wstring::npos&&screen.contents().find(L"partial log complete")!=std::wstring::npos,"both rows restored after the complete log with the latest timed sample");
        sample="invalid sample";live_count=300000;status.update(start+std::chrono::seconds(120),300000,3);
        check(calls==5&&screen.contents().find(prefix+wide(key_c))!=std::wstring::npos,"invalid callback output cannot replace a valid fixed-width sample");
        throws=true;live_count=450000;status.update(start+std::chrono::seconds(150),450000,3);
        check(calls==6&&screen.contents().find(prefix+wide(key_c))!=std::wstring::npos,"provider exception keeps the previous valid sample and the status responsive");
        status.finish();
        check(calls==6&&screen.contents().find(prefix)==std::wstring::npos,"finish erases both rows without refreshing the sample");
        check(screen.contents().find(L"unrelated log retained")!=std::wstring::npos&&screen.contents().find(L"second log retained")!=std::wstring::npos&&screen.contents().find(L"partial log complete")!=std::wstring::npos,"all unrelated logged output remains intact");
    }
    std::ifstream input(csv,std::ios::binary);const std::string records{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    check(records.find("PRIVATE KEY")==std::string::npos&&!std::regex_search(records,std::regex("[0-9a-fA-F]{64}")),"performance CSV contains neither private labels nor 64-digit private material");
    for(const auto& expected:std::vector<std::string>{",sample,65,30.000,30000,1000,",",sample,75,30.000,90000,2000,",",sample,75,30.000,180000,3000,",",sample,75,30.000,300000,4000,",",sample,75,30.000,450000,5000,"})
        check(records.find(expected)!=std::string::npos,"CSV count delta duration and rate baseline remain unchanged");
}
void width_and_scroll(Screen& screen,SHORT width,SHORT height,bool bottom){
    screen.clear(width,height);if(bottom){screen.cursor(0,height-3);screen.write(L"scroll-safe log\r\n");}
    std::string sample=key_a;unsigned calls=0;const auto start=MinuteSpeed::Clock::now();
    {
        MinuteSpeed status(start,100,nullptr,{}, {},[&]{++calls;return sample;});
        check(screen.contents().find(key_a_lower)!=std::wstring::npos,"wrapped initial private sample is complete");
        sample=key_b;status.update(start+std::chrono::seconds(30),30000,1);
        check(calls==2&&screen.contents().find(wide(key_b))!=std::wstring::npos,"wrapped timed sample refreshes");
        check(screen.contents().find(key_a_lower)==std::wstring::npos&&occurrences(screen.contents(),prefix)==1,"wrapped or scrolled old sample leaves no history row");
        status.finish();check(screen.contents().find(prefix)==std::wstring::npos&&screen.contents().find(wide(key_b))==std::wstring::npos,"wrapped or scrolled frame is fully erased on finish");
        if(bottom)check(screen.contents().find(L"scroll-safe log")!=std::wstring::npos,"bottom scrolling preserves the adjacent unrelated log");
    }
}
void resize_frame(Screen& screen,SHORT initial_width=240,SHORT final_width=80){
    screen.clear(initial_width,60);screen.write(L"resize-safe log\r\n");std::string sample=key_a;const auto start=MinuteSpeed::Clock::now();
    {
        MinuteSpeed status(start,100,nullptr,{}, {},[&]{return sample;});
        const auto before=screen.info();const auto before_text=screen.contents();const auto before_cells=screen.cells();
        screen.size(final_width,60);const auto resized=screen.info();const auto resized_text=screen.contents();const auto resized_cells=screen.cells();
        report<<"GEOMETRY before_width="<<before.dwSize.X<<" before_cursor_x="<<before.dwCursorPosition.X<<" before_cursor_y="<<before.dwCursorPosition.Y
            <<" before_private_row="<<(before_text.find(prefix)/before.dwSize.X)<<" after_width="<<resized.dwSize.X<<" after_cursor_x="<<resized.dwCursorPosition.X<<" after_cursor_y="<<resized.dwCursorPosition.Y
            <<" after_private_row="<<(resized_text.find(prefix)/resized.dwSize.X)<<" after_sample_row="<<(resized_text.find(key_a_lower)/resized.dwSize.X)<<'\n';report.flush();
        const auto describe=[&](const char* label,const std::wstring& text,const std::vector<CHAR_INFO>& physical,SHORT width){
            for(unsigned row=0;row<7;++row){const auto line=text.substr(size_t(row)*width,width);const auto position=[](size_t value){return value==std::wstring::npos?-1:static_cast<int>(value);};
                report<<"ROW_GEOMETRY "<<label<<" row="<<row<<" first="<<position(line.find_first_not_of(L' '))<<" last="<<position(line.find_last_not_of(L' '))
                    <<" nonspace="<<std::count_if(line.begin(),line.end(),[](wchar_t c){return c!=L' ';})<<" private_prefix="<<position(line.find(prefix))<<" status_bracket="<<position(line.find(L'['))
                    <<" wide_cells="<<std::count_if(physical.begin()+size_t(row)*width,physical.begin()+size_t(row+1)*width,[](const CHAR_INFO& cell){return (cell.Attributes&(COMMON_LVB_LEADING_BYTE|COMMON_LVB_TRAILING_BYTE))!=0;})<<'\n';}
            report<<"TOKEN_GEOMETRY "<<label<<" sample_start_row="<<(text.find(key_a_lower)/width)<<" sample_start_col="<<(text.find(key_a_lower)%width)<<" private_count="<<occurrences(text,prefix)<<'\n';report.flush();
        };
        describe("before",before_text,before_cells,before.dwSize.X);describe("resized",resized_text,resized_cells,resized.dwSize.X);
        sample=key_b;status.update(start+std::chrono::seconds(30),30000,1);
        check(screen.contents().find(wide(key_b))!=std::wstring::npos,"new sample is visible after narrowing the buffer");
        check(screen.contents().find(key_a_lower)==std::wstring::npos&&occurrences(screen.contents(),prefix)==1,"resize does not retain the old private sample as history");
        check(screen.contents().find(L"resize-safe log")!=std::wstring::npos,"resize recovery preserves the previous unrelated log");
        status.finish();check(screen.contents().find(prefix)==std::wstring::npos,"finish after resize clears every private status row");
    }
}
void foreign_text_after_resize(Screen& screen){
    screen.clear(240,60);std::string sample=key_a;const auto start=MinuteSpeed::Clock::now();
    MinuteSpeed status(start,100,nullptr,{}, {},[&]{return sample;});
    screen.write(L"\r\nforeign output after status remains\r\n");screen.size(80,60);
    sample=key_b;status.update(start+std::chrono::seconds(30),30000,1);
    check(screen.contents().find(L"foreign output after status remains")!=std::wstring::npos,"resized ownership recovery does not erase foreign text after the old frame");
    status.finish();
    check(screen.contents().find(L"foreign output after status remains")!=std::wstring::npos,"finish preserves foreign output after resized frame ownership was lost");
}
int child(const std::filesystem::path& output){
    report.open(output,std::ios::binary);if(!report)return 2;
    try{Screen screen;cached_sample_and_csv(screen,output.parent_path()/"performance.csv");width_and_scroll(screen,80,60,false);width_and_scroll(screen,40,60,false);width_and_scroll(screen,80,8,true);resize_frame(screen);resize_frame(screen,80,240);foreign_text_after_resize(screen);
        screen.clear();{MinuteSpeed legacy(MinuteSpeed::Clock::now());check(screen.contents().find(prefix)==std::wstring::npos&&screen.nonempty_rows()==1,"callers without a sample provider keep their existing single status row");}
    }catch(const std::exception& failure){check(false,failure.what());}
    report<<"SUMMARY checks="<<checks<<" failures="<<failures<<" own_inactive_console_only=1 inputs=0 GPU=0 network=0\n";report.flush();return failures?1:0;
}
}
int wmain(int argc,wchar_t**argv){
    if(argc==3&&std::wstring(argv[1])==L"--console-child")return child(std::filesystem::path(argv[2]));
    if(argc!=2)return 2;
    const auto report_path=std::filesystem::absolute(std::filesystem::path(argv[1]));
    std::wstring executable(32768,L'\0');const DWORD length=GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));if(!length)return 3;executable.resize(length);
    std::wstring command=L"\""+executable+L"\" --console-child \""+report_path.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof startup;startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&startup,&process))return 4;
    const DWORD waited=WaitForSingleObject(process.hProcess,30000);if(waited!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,5);DWORD result=5;GetExitCodeProcess(process.hProcess,&result);CloseHandle(process.hThread);CloseHandle(process.hProcess);return int(result);
}
