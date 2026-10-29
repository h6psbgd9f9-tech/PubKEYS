// ============================================================================
//  plat_selftest.cpp — проверка слоя platform НА ТОЙ СИСТЕМЕ, ГДЕ ЗАПУЩЕН.
//
//  Это первое, что нужно запустить на новой системе — раньше самой программы.
//
//  Причина простая: Windows-часть platform.cpp написана, но ни разу не
//  собрана. Ни на маке, ни в среде, где это писалось, нет ни MSVC, ни mingw.
//  Каждая функция там может оказаться неверной, и почти ни одна ошибка не
//  проявится падением: exe_dir вернёт не тот каталог, fsync промолчит,
//  права не закроются, фоновая команда не запустится. Программа при этом
//  будет считать ключи и выглядеть исправной.
//
//  Поэтому проверки ниже не спрашивают у функций, что они думают, а
//  СМОТРЯТ НА РЕЗУЛЬТАТ: каталог создан на диске, файл на месте, вывод
//  команды совпал с ожидаемым, фоновая команда действительно оставила след.
// ============================================================================
#include "platform.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#ifdef _WIN32
  #include <direct.h>
#else
  #include <unistd.h>
#endif

static int fails = 0;

static int u8len(const std::string &s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}
static void check(bool ok, const std::string &name, const std::string &detail) {
    std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << name;
    for (int i = u8len(name); i < 40; i++) std::cout << ' ';
    std::cout << detail << "\n";
    if (!ok) fails++;
}

static bool file_exists(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return (bool)f;
}

int main() {
    plat_console_utf8();

    std::cout << "\n=== слой platform, система: " << plat_name() << " ===\n\n";

    // ------------------------------------------------------------ exe_dir
    //
    // Проверяем не «непустую строку», а то, что в этом каталоге ЛЕЖИТ сама
    // программа. Непустую строку вернёт и ошибочная реализация.
    {
        const std::string dir = plat_exe_dir();
        bool found = false;
        if (!dir.empty()) {
            // Ищем себя: имя может быть с .exe и без.
            for (const char *n : { "plat_selftest", "plat_selftest.exe",
                                   "pkplat", "pkplat.exe" })
                if (file_exists(dir + "/" + n)) { found = true; break; }
        }
        check(!dir.empty() && found, "exe_dir: каталог самой программы",
              dir.empty() ? "пусто" : (found ? dir : dir + "  (себя не нашёл!)"));
    }

    // -------------------------------------------------------------- chdir
    {
        const std::string dir = plat_exe_dir();
        const bool ok = !dir.empty() && plat_chdir(dir);
        check(ok, "chdir в каталог программы", ok ? "перешли" : "не вышло");
    }

    // ------------------------------------------------------- создать папку
    const std::string tdir = "PLAT_TEST_DIR";
    {
        const bool a = plat_make_dir_private(tdir);
        const bool b = plat_make_dir_private(tdir);   // второй раз: уже есть
        check(a && b, "создать папку (и повторно)",
              a ? (b ? "создана, повтор не ошибка" : "ПОВТОР дал ошибку") : "не создана");
    }

    // ------------------------------------------------------ создать файл
    const std::string tfile = tdir + "/test.txt";
    {
        plat_make_file_private(tfile);
        {
            std::ofstream f(tfile, std::ios::trunc);
            f << "проверка\n";
        }
        plat_make_file_private(tfile);
        check(file_exists(tfile), "файл с закрытыми правами",
              file_exists(tfile) ? tfile : "НЕ СОЗДАН");
        std::cout << "        " << plat_privacy_note() << "\n";
    }

    // ------------------------------------------------------------- fsync
    {
        const bool f1 = plat_fsync_file(tfile);
        const bool f2 = plat_fsync_dir(tdir);
        check(f1, "сброс файла на диск", f1 ? "сброшен" : "НЕ СБРОШЕН");
        check(f2, "сброс каталога", f2 ? "сброшен (на Windows не требуется)"
                                       : "НЕ СБРОШЕН");
    }

    // ------------------------------------------ АТОМАРНАЯ ЗАМЕНА ФАЙЛА
    //
    // САМАЯ ВАЖНАЯ ПРОВЕРКА ЭТОГО ФАЙЛА, И ЕЁ ЗДЕСЬ НЕ БЫЛО.
    //
    // platform.h прямо пишет: std::rename на Windows отказывается заменять
    // существующий файл, и чекпоинт обновлялся ровно один раз, первый, а
    // дальше каждая запись тихо проваливалась. Это самая дорогая из известных
    // ошибок этого проекта -- и функция, которая её закрывает, не
    // проверялась. Человек прогонял plat_selftest, видел «Все проверки
    // пройдены» и считал, что сохранение позиции работает.
    //
    // Проверяем ОБА случая -- когда цели ещё нет и когда она уже есть, -- и
    // смотрим на СОДЕРЖИМОЕ, а не на возвращённое значение.
    {
        const std::string dst = tdir + "/ckpt.dat";
        const std::string tmp = dst + ".tmp";
        auto put = [](const std::string &p, const std::string &s) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s;
        };
        auto get = [](const std::string &p) {
            std::ifstream f(p, std::ios::binary);
            std::string s; std::getline(f, s); return s;
        };
        std::remove(dst.c_str());

        put(tmp, "ПЕРВЫЙ");
        const bool r1 = plat_replace_file(tmp, dst);
        const bool c1 = (get(dst) == "ПЕРВЫЙ") && !file_exists(tmp);
        check(r1 && c1, "замена файла: цели ещё нет",
              std::string(r1 ? "" : "вернул false; ") +
              (c1 ? "содержимое верное" : "СОДЕРЖИМОЕ НЕ ТО"));

        put(tmp, "ВТОРОЙ");
        const bool r2 = plat_replace_file(tmp, dst);
        const bool c2 = (get(dst) == "ВТОРОЙ") && !file_exists(tmp);
        check(r2 && c2, "замена ПОВЕРХ существующего",
              std::string(r2 ? "" : "вернул false; ") +
              (c2 ? "перезаписан"
                  : "ОСТАЛСЯ ПРЕЖНИЙ — ЧЕКПОИНТ НЕ БУДЕТ ОБНОВЛЯТЬСЯ"));

        std::remove(dst.c_str());
        std::remove(tmp.c_str());
    }

    // ------------------------------- каталог, которого нет; файл вместо него
    //
    // EEXIST означает «что-то уже есть», а не «есть каталог». Обычный файл с
    // именем RESULT делал бы Found.txt незаписываемым при успешном отчёте.
    {
        const std::string clash = tdir + "/not_a_dir";
        { std::ofstream f(clash, std::ios::trunc); f << "x"; }
        const bool ok = !plat_make_dir_private(clash);
        check(ok, "файл вместо каталога отвергнут",
              ok ? "отвергнут" : "ПРИНЯТ ЗА КАТАЛОГ — находка не запишется");
        std::remove(clash.c_str());
    }

    // --------------------------------- перевод в кодировку системы
    //
    // Пустая строка отсюда означала бы, что сообщение в Signal пропало целиком.
    {
        const std::string s = plat_to_native("проверка");
        check(!s.empty(), "перевод UTF-8 в кодировку системы",
              s.empty() ? "ВЕРНУЛА ПУСТО (сообщение пропало бы)"
                        : ("длина " + std::to_string(s.size()) + " байт"));
    }

    // ------------------------------------- открытие файла по пути UTF-8
    //
    // На Windows это _wfopen: путь с кириллицей через узкий fopen просто не
    // открывается, и чекпоинт с находкой оседали бы неизвестно где.
    {
        const std::string p = tdir + "/файл-проверка.txt";
        bool wrote = false, read_back = false;
        if (FILE *f = plat_fopen(p, "wb")) {
            wrote = fwrite("ok", 1, 2, f) == 2;
            fclose(f);
        }
        if (FILE *f = plat_fopen(p, "rb")) {
            char b[4] = {0};
            read_back = fread(b, 1, 2, f) == 2 && b[0] == 'o' && b[1] == 'k';
            fclose(f);
        }
        check(wrote && read_back, "путь с кириллицей: запись и чтение",
              (wrote && read_back) ? "прочитано обратно"
                                   : "НЕ ОТКРЫЛСЯ — пути с кириллицей не работают");
        plat_remove_file(p);
    }

    // --------------------------------------------------- вывод команды
    //
    // echo есть везде. Сверяем не «что-то вернулось», а именно то слово,
    // которое просили: пустой или чужой вывод означал бы, что команда не
    // выполнилась, а мы этого не заметили.
    {
        const std::string out = plat_run_capture("echo PLATOK");
        const bool ok = out.find("PLATOK") != std::string::npos;
        std::string shown = out;
        while (!shown.empty() && (shown.back()=='\n' || shown.back()=='\r')) shown.pop_back();
        check(ok, "вывод внешней команды", ok ? "получено: " + shown
                                              : "получено: [" + shown + "]");
    }

    // ------------------------------------------------- есть ли команда
    {
        const bool yes = plat_have_command("echo") || plat_have_command("cmd");
        const bool no  = plat_have_command("takoy_komandy_net_12345");
        check(yes && !no, "поиск команды в PATH",
              std::string(yes ? "известную нашёл" : "известную НЕ нашёл") +
              std::string(no ? ", несуществующую тоже (плохо)" : ", несуществующую нет"));
    }

    // -------------------------------------------------- фоновая команда
    //
    // Самая тонкая проверка. Фоновая команда по определению ничего не
    // возвращает, поэтому единственный честный способ убедиться, что она
    // выполнилась, -- посмотреть на её СЛЕД. Ждём с запасом: на холодной
    // системе запуск оболочки занимает заметное время.
    {
        const std::string mark = tdir + "/detached.txt";
        std::remove(mark.c_str());
#ifdef _WIN32
        plat_run_detached("cmd /C echo ok > " + mark);
#else
        plat_run_detached("echo ok > " + mark);
#endif
        bool seen = false;
        for (int i = 0; i < 50 && !seen; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            seen = file_exists(mark);
        }
        check(seen, "фоновая команда выполнилась",
              seen ? "след найден" : "СЛЕДА НЕТ за 5 секунд");
        std::remove(mark.c_str());
    }

    // ------------------------------------------------ строка для тишины
    {
        const std::string d = plat_devnull_2();
        const bool ok =
#ifdef _WIN32
            d == "2>NUL";
#else
            d == "2>/dev/null";
#endif
        check(ok, "куда девать ошибки команд", d);
    }

    std::remove(tfile.c_str());
#ifdef _WIN32
    _rmdir(tdir.c_str());
#else
    rmdir(tdir.c_str());
#endif

    std::cout << "\n";
    if (fails) {
        std::cout << "Провалено проверок: " << fails << "\n"
                  << "Пока они не пройдут, запускать поиск на этой системе нельзя:\n"
                  << "перечисленное выше отвечает за сохранение позиции и находок.\n";
    } else {
        std::cout << "Все проверки пройдены — слой platform на "
                  << plat_name() << " работает.\n";
    }
    return fails ? 1 : 0;
}
