/* ============================================================================
 *  port_compat.h — расширения GCC/Clang, которых нет у MSVC.
 *
 *  Программа писалась под clang на маке, и в ней естественным образом
 *  расставлены вещи, которых в стандарте C++ нет вовсе:
 *
 *      unsigned __int128        -- 128-битное целое      (см. u128_compat.h)
 *      __builtin_expect         -- подсказка о вероятной ветке
 *      __builtin_prefetch       -- запрос строки кэша заранее
 *      __attribute__((unused))  -- «функция может не использоваться»
 *
 *  У MSVC нет ни одного из них, и сборка под Windows об это спотыкается.
 *  Здесь для каждого дан эквивалент. Под GCC и Clang файл не делает НИЧЕГО:
 *  там всё это есть в готовом виде, и подменять его нечем и незачем.
 *
 *  ПОЧЕМУ ОПРЕДЕЛЯЮТСЯ ИМЕНА С ДВУМЯ ПОДЧЁРКИВАНИЯМИ. Обычно такие имена
 *  трогать нельзя -- они принадлежат реализации. Но здесь они определяются
 *  ТОЛЬКО под MSVC, где этих имён не существует: ничего не переопределяется,
 *  а места вызова остаются нетронутыми. Вариант «завести свои имена и
 *  переписать девять мест» был бы чище на бумаге, но означал бы правки в
 *  горячем цикле engine.cpp и kangaroo.cpp ради косметики.
 * ========================================================================== */
#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H

#include "u128_compat.h"

#if defined(_MSC_VER) && !defined(__clang__)

  #include <intrin.h>

  /* Подсказка о вероятной ветке. У MSVC аналога нет: [[likely]] из C++20 --
     атрибут оператора, а не выражение, и на месте __builtin_expect(x, 0)
     внутри if не стоит. Поэтому просто отдаём само выражение: смысл
     программы не меняется, теряется только подсказка планировщику. */
  #ifndef __builtin_expect
    #define __builtin_expect(expr, expected) (expr)
  #endif

  /* Запрос строки кэша заранее. _mm_prefetch -- ровно та же инструкция
     prefetcht0. Третий аргумент GCC (степень локальности 0..3) у MSVC
     передаётся выбором подсказки; всюду в этом проекте стоит 3, то есть
     «нужно во всех уровнях кэша», чему соответствует _MM_HINT_T0. */
  #ifndef __builtin_prefetch
    #define __builtin_prefetch(addr, rw, locality) \
        _mm_prefetch((const char *)(addr), _MM_HINT_T0)
  #endif

  /* __attribute__((unused)) и прочие -- молча отбрасываются. */
  #ifndef __attribute__
    #define __attribute__(x)
  #endif

  /* __thread -- расширение GCC для переменных, у каждого потока своих.
     У MSVC это __declspec(thread), у C++11 -- thread_local. Берём
     __declspec: scan_secp.c, где стоят все три такие переменные,
     собирается как C, а thread_local -- ключевое слово C++. */
  #ifndef __thread
    #define __thread __declspec(thread)
  #endif

  /* clock_gettime(CLOCK_MONOTONIC) -- POSIX, в Windows нет.
   *
     ЧЕСТНО ПРО ОГРАНИЧЕНИЕ: здесь подставлен timespec_get из C11, а это
     СТЕННЫЕ часы, а не монотонные. Разница проявится, только если во время
     замера систему переведут (перевод по NTP, смена часового пояса) -- тогда
     один замер окажется выбросом.
   *
     Взято так осознанно. Правильный ответ на Windows -- QueryPerformanceCounter,
     но он тянет за собой <windows.h> в заголовок, который включается КАЖДЫМ
     файлом проекта, а вместе с ним десятки макросов вроде min/max, ломающих
     чужой код. Цена не стоит того: единственное место вызова -- необязательное
     профилирование по фазам в scan_secp.c, включаемое переменной окружения
     SCAN_PROFILE=1, и оно меряет доли миллисекунды внутри одного пакета.
     Понадобится настоящая монотонность -- это одна строка вот здесь. */
  #include <time.h>
  #ifndef CLOCK_MONOTONIC
    #define CLOCK_MONOTONIC 1
  #endif
  static inline int clock_gettime(int clk, struct timespec *ts)
  {
      (void)clk;
      return timespec_get(ts, TIME_UTC) == TIME_UTC ? 0 : -1;
  }

  /* setenv/unsetenv -- это POSIX, в Windows их нет. Используются в notify.cpp
     ровно в одном месте: пароль архива передаётся внешнему zip через
     переменную окружения (в командной строке он был бы виден всей системе в
     списке процессов), и сразу после запуска убирается.
   *
     _putenv_s с пустым значением на Windows именно УДАЛЯЕТ переменную -- это
     задокументированное поведение, а не побочный эффект. */
  #include <stdlib.h>

  static inline int setenv(const char *name, const char *value, int overwrite)
  {
      if (!overwrite && getenv(name) != NULL) return 0;   /* уже есть -- не трогаем */
      return _putenv_s(name, value) == 0 ? 0 : -1;
  }

  static inline int unsetenv(const char *name)
  {
      return _putenv_s(name, "") == 0 ? 0 : -1;
  }

#endif /* _MSC_VER */

#endif /* PORT_COMPAT_H */
