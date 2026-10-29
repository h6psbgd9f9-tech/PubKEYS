#!/usr/bin/env python3
# ============================================================================
#  check_makefiles.py — сходятся ли списки файлов в Makefile и Makefile.win
#
#  Два файла сборки существуют не по прихоти: nmake не понимает ни $(shell),
#  ни $(foreach), на которых держится определение архитектур видеокарты.
#  Ужиться в одном файле не выйдет.
#
#  Но две копии одного списка -- это обещание однажды разойтись. Кто-нибудь
#  добавит файл в один Makefile и забудет про второй; сборка под ту систему
#  просто не найдёт функцию, а под эту соберётся, и расхождение проживёт до
#  первой чужой машины.
#
#  Раз убрать дублирование нельзя, сделаем его ЗАМЕТНЫМ.
# ============================================================================
import re, sys

gnu = open('Makefile', encoding='utf-8').read()
win = open('Makefile.win', encoding='utf-8').read()

m = re.search(r'^SRC = (.*?)\n\n', gnu, re.S | re.M)
if not m:
    print("не нашёл SRC в Makefile"); sys.exit(2)
src = set(x.rsplit('/',1)[-1] for x in m.group(1).replace('\\', '').split() if x.endswith('.cpp'))

m = re.search(r'OBJS = (.*?)\n\n', win, re.S)
if not m:
    print("не нашёл OBJS в Makefile.win"); sys.exit(2)
objs = m.group(1).replace('\\', '').split()
wsrc = set(o.replace('.obj', '.cpp') for o in objs if o != 'scan_secp.obj')

only_gnu = sorted(src - wsrc)
only_win = sorted(wsrc - src)

if only_gnu or only_win:
    print("СПИСКИ ФАЙЛОВ РАЗОШЛИСЬ:")
    if only_gnu: print("  есть в Makefile, нет в Makefile.win :", ", ".join(only_gnu))
    if only_win: print("  есть в Makefile.win, нет в Makefile :", ", ".join(only_win))
    print("\n  Добавьте недостающее, иначе сборка под одну из систем")
    print("  не найдёт часть программы.")
    sys.exit(1)

# И наоборот: файл лежит в папке, но не собирается нигде. Так уже было с
# DETERMINISTIC_V2.cpp -- 1530 строк мёртвого кода, попадавших в архив.
import os
on_disk = set(f for f in os.listdir('.') if f.endswith('.cpp'))
known = src | {'gpu_selftest.cpp', 'gpu_main.cpp', 'plat_selftest.cpp',
               'cuda_field_test.cpp', 'cuda_host_test.cpp', 'cuda_host.cpp',
               'cuda_step_test.cpp', 'filter_parity_test.cpp', 'gpu_plan_test.cpp',
               'gpu_plan_probe.cpp', 'scan_targets_test.cpp', 'test_u128.cpp',
               'hybrid_test.cpp', 'wave_audit.cpp', 'zone_audit.cpp'}
orphan = sorted(on_disk - known)
if orphan:
    print("Лежат в папке, но не собираются нигде:", ", ".join(orphan))
    print("  Это либо забытый файл, либо мёртвый код.")
    sys.exit(1)

print(f"Списки сходятся: {len(src)} файлов в обоих Makefile, лишних в папке нет.")
