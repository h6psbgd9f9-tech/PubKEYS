# ============================================================================
#  PubkeyHunt — сборка
#
#  ОДНА КОМАНДА `make` СОБИРАЕТ ВСЁ, ЧТО УСТАРЕЛО, И НИЧЕГО СВЕРХ ТОГО.
#
#  Что было не так у прежнего Makefile:
#
#    1. Цель own зависела от готового файла pubkeyhunt. После `make portable`
#       команда `make own` отвечала «Nothing to be done» и оставляла ЧУЖОЙ
#       бинарник -- собранный другой целью, с другими флагами. Молча.
#    2. Правка заголовка пересобирала всё целиком (единственная зависимость --
#       список $(HDR) на всю программу), а правка Makefile или смена HALF на
#       командной строке не пересобирала НИЧЕГО.
#    3. Объектные файлы разных конфигураций писались в один scan_secp.o.
#
#  Теперь: раздельная компиляция, зависимости от заголовков считает сам
#  компилятор (-MMD), объекты каждой конфигурации лежат в своей папке
#  build/<конфигурация>/, а смена конфигурации всегда приводит к перелинковке.
#  Переключаться между own и secp можно как угодно часто -- объекты обеих
#  конфигураций сохраняются, обратное переключение стоит одну линковку.
# ============================================================================

# ---------------------------------------------------------------- выбор ----
#  BACKEND = secp | own      горячий цикл: libsecp256k1 либо своя математика
#  NATIVE  = 0 | 1           подстройка под процессор сборочной машины
#
#  ПО УМОЛЧАНИЮ СБОРКА ПЕРЕНОСИМАЯ (NATIVE=0), И ЭТО ОСОЗНАННО.
#
#  Раньше по умолчанию стояла подстройка под процессор сборочной машины.
#  Такой бинарник нельзя отнести на другой мак: собрали на M5, запустили на
#  M1 -- там нет части инструкций, и программа умирает с «Illegal
#  instruction» мгновенно, без единого слова. Выглядит как поломка программы,
#  хотя дело в наборе команд.
#
#  Отказаться от подстройки оказалось бесплатно. Замер на этом же горячем
#  цикле, 2 потока, база 200 тысяч целей:
#
#      под свой процессор   58 млн ключей/с
#      переносимая          60 млн ключей/с
#
#  Разница в пределах шума. Цикл состоит из 64-битных умножений и сложений,
#  векторизовать там нечего, и подстройке просто не за что зацепиться. В
#  комментариях тут прежде стояло «медленнее процентов на десять» -- замер
#  это опроверг.
#
#  Кому всё же нужна подстройка: make native (или NATIVE=1).
BACKEND ?= secp
NATIVE  ?= 0

CXX ?= g++
CC  ?= gcc

# Ключ подстройки под процессор выбирается проверкой, а не угадыванием:
# gcc понимает -march=native, clang на Apple Silicon — только -mcpu=native.
ARCHTEST := $(shell $(CC) -march=native -E - </dev/null >/dev/null 2>&1 && echo -march=native || \
                   ($(CC) -mcpu=native  -E - </dev/null >/dev/null 2>&1 && echo -mcpu=native))
ifeq ($(NATIVE),1)
  ARCH := $(ARCHTEST)
else
  ARCH :=
endif

# Программа сама показывает, переносимая она или нет: бинарник часто копируют
# на другую машину, и узнать об ограничении лучше до переноса, а не после.
NATDEF := -DPKH_NATIVE_BUILD=$(if $(ARCH),1,0)

WARN     := -Wall -Wno-unused-function
OPT      := -O3 $(ARCH) -funroll-loops
CXXFLAGS ?= -I. $(OPT) -I. -std=c++17 $(WARN)
CFLAGS   ?= $(OPT) -std=c99   $(WARN)
LDFLAGS  ?= -pthread

TARGET = pubkeyhunt

SRC = main.cpp pult_control.cpp kxe_sass_backend.cpp util.cpp config.cpp state.cpp search.cpp targets.cpp engine.cpp zone_search.cpp zone_mode.cpp hybrid_search.cpp hybrid_mode.cpp \
      platform.cpp gpu_info.cpp \
      traverse.cpp traverse_modes.cpp notify.cpp found_card.cpp key_qr.cpp qrcodegen.cpp telegram_found.cpp ui.cpp tests/app/selftest.cpp

# Путь к ИСХОДНИКАМ libsecp256k1 (папка, внутри которой есть src/ и include/).
# Собранная библиотека не нужна — берутся только заголовки с реализацией.
# Сначала смотрим рядом с собой, потом в домашней папке.
#
# Было просто $(HOME)/secp256k1. У этой установки библиотека лежит в самой
# папке программы, поэтому голая команда make упиралась в «не вижу
# исходники» и требовала каждый раз дописывать SECP_DIR=... руками.
SECP_DIR ?= $(if $(wildcard ./secp256k1/src/field_5x52_impl.h),./secp256k1,$(HOME)/secp256k1)
# make не раскрывает тильду, а переменная из командной строки перебивает
# обычное присваивание — отсюда override.
override SECP_DIR := $(patsubst ~/%,$(HOME)/%,$(SECP_DIR))

# Размер пакета: make HALF=1024. Что быстрее на конкретной машине — вопрос
# замера, а не рассуждения; прогоняйте по три раза, разброс между запусками
# бывает больше разницы между вариантами.
HALFDEF := $(if $(HALF),-DSECP_HALF=$(HALF),)

# Глубина конвейера предвыборки фильтра: make SCANPF=16
# 8 стоит по умолчанию по замеру: 8 и 16 дали 31.85 и 31.95 Mkey/s на Apple M5
# при базе 1,6 млн целей -- разница в пределах шума, подбирать нечего.
SCANPF ?= 8
PFDEF := $(if $(SCANPF),-DSCAN_PF=$(SCANPF) -DPF_DEPTH=$(SCANPF),)

ifeq ($(BACKEND),secp)
  DEFS := -DUSE_SECP $(HALFDEF) $(PFDEF) $(NATDEF)
  INC  := -I. -I$(SECP_DIR)/src -I$(SECP_DIR)/include
  COBJ := scan_secp.o
else
  DEFS := $(HALFDEF) $(PFDEF) $(NATDEF)
  INC  := -I.
  COBJ :=
endif

# Имя конфигурации входит в путь к объектам: разные HALF, SCANPF, backend и
# режим подстройки под процессор не пересекаются и не требуют make clean.
CONFIG := $(BACKEND)-$(if $(ARCH),native,portable)-h$(if $(HALF),$(HALF),512)-p$(SCANPF)
BUILD  := build/$(CONFIG)

OBJ := $(addprefix $(BUILD)/,$(SRC:.cpp=.o) $(COBJ))
DEP := $(OBJ:.o=.d)

# ---------------------------------------------------------------------------
#  make                обычная сборка: libsecp256k1, ПЕРЕНОСИМАЯ -- пойдёт на
#                      любом маке с Apple Silicon. Это то, что нужно почти
#                      всегда, в том числе когда бинарник несут на другой мак.
#  make own            своя математика, ноль зависимостей, тоже переносимая
#  make native         подстройка под ЭТОТ процессор: на другой машине может
#                      не запуститься, а выигрыша по замеру нет
#  make test / bench   собирает при необходимости и запускает
#  make clean          сносит объекты и бинарник
#
#  Переопределяется из командной строки:
#      make SECP_DIR=/путь/к/secp256k1
#      make SCANPF=16
#      make HALF=1024
#      make BACKEND=own NATIVE=0        (то же, что make portable)
#
#  ПРО ПОДСТРОЙКУ ПОД ПРОЦЕССОР.
#
#  NATIVE=1 собирает под ТОТ процессор, на котором идёт сборка. Такой бинарник
#  нельзя перенести на другую машину: если там нет тех же наборов инструкций,
#  он упадёт с SIGILL -- причём не при запуске, а на первой же команде,
#  которой нет. Поэтому по умолчанию стоит NATIVE=0: замер показал, что
#  подстройка на этом коде не даёт ничего (58 против 60 млн/с), а сломать
#  перенос может.
# ---------------------------------------------------------------------------
.PHONY: all own portable secp secp-portable native own-native test bench clean help

# ПРОВЕРКА ИСХОДНИКОВ libsecp256k1 ИДЁТ ПЕРВОЙ, А НЕ ПЕРЕД ЛИНКОВКОЙ.
#
# Файлы на C++ собираются с -DUSE_SECP, но самих исходников libsecp256k1 им
# не нужно -- им хватает локального scan_secp.h. Поэтому без проверки здесь
# make успевал скомпилировать все двенадцать .cpp и только потом спотыкался
# на scan_secp.c. Пользователь ждал полную сборку ради сообщения «не вижу
# исходники», которое можно было выдать сразу.
ifeq ($(BACKEND),secp)
all: secp-check
endif
all: $(TARGET)

# Готовые сочетания. Это рекурсивный вызов того же Makefile с другими
# переменными, а не отдельные правила сборки: правило одно, и разойтись им
# между собой физически негде. Прежние четыре цели дублировали командные
# строки, и secp-portable успел разойтись с secp по -std и по -I.
# Переносимые (пойдут на любом маке с Apple Silicon):
own:           ; @$(MAKE) --no-print-directory BACKEND=own  NATIVE=0 all
portable:      ; @$(MAKE) --no-print-directory BACKEND=own  NATIVE=0 all
secp:          ; @$(MAKE) --no-print-directory BACKEND=secp NATIVE=0 all
secp-portable: ; @$(MAKE) --no-print-directory BACKEND=secp NATIVE=0 all

# Подстроенные под ЭТОТ процессор -- только для своей машины:
native:        ; @$(MAKE) --no-print-directory BACKEND=secp NATIVE=1 all
own-native:    ; @$(MAKE) --no-print-directory BACKEND=own  NATIVE=1 all

# ПЕРЕЛИНКОВКА ПРИ СМЕНЕ КОНФИГУРАЦИИ.
#
# Бинарник один на все конфигурации, а объекты у каждой свои. Без этой метки
# после `make portable` команда `make own` видела бы свежий pubkeyhunt, свои
# объекты старше него -- и не делала бы ничего, оставив бинарник от прошлой
# цели. Метка текущей конфигурации создаётся заново при каждом переключении,
# поэтому линковка происходит обязательно.
$(TARGET): $(OBJ) .config-$(CONFIG)
	@echo "  link   $(TARGET)  [$(CONFIG)]"
	@$(CXX) $(OBJ) -o $@ $(LDFLAGS)

.config-$(CONFIG):
	@rm -f .config-*
	@touch $@

$(BUILD)/%.o: %.cpp Makefile | $(BUILD)
	@mkdir -p $(dir $@)
	@echo "  c++    $<"
	@$(CXX) $(CXXFLAGS) $(DEFS) $(INC) -MMD -MP -c $< -o $@

$(BUILD)/scan_secp.o: scan_secp.c Makefile | $(BUILD) secp-check
	@echo "  cc     $<"
	@$(CC) $(CFLAGS) $(DEFS) $(INC) -MMD -MP -c $< -o $@

$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: secp-check
secp-check:
	@test -f "$(SECP_DIR)/src/field_5x52_impl.h" || \
	  (echo ""; \
	   echo "Не вижу исходники libsecp256k1 в $(SECP_DIR)"; \
	   echo "Нужна папка, внутри которой есть src/ и include/."; \
	   echo ""; \
	   echo "Если она лежит в другом месте:"; \
	   echo "    make SECP_DIR=/путь/к/secp256k1"; \
	   echo ""; \
	   echo "Если её нет вовсе -- можно собрать на своей математике,"; \
	   echo "без внешних зависимостей (медленнее примерно вдвое):"; \
	   echo "    make own"; \
	   echo ""; exit 1)

# Зависимости от заголовков считает компилятор (-MMD): правка одного .h
# пересобирает только то, что его включает, и пересобирает обязательно.
# Прежний список $(HDR) на всю программу давал ровно обратное -- любая правка
# любого заголовка пересобирала всё.
-include $(DEP)

# The exact interval digit masks also feed the standalone range audit.
$(BUILD)/traverse_modes.o $(BUILD)/ui.o $(BUILD)/tests/app/selftest.o: interval_digits.h
$(BUILD)/tests/app/selftest.o: tests/app/range_audit.h
$(BUILD)/main.o $(BUILD)/ui.o: single_instance.h
$(BUILD)/notify.o: found_display.h

test: $(TARGET)
	./$(TARGET) --test

bench: $(TARGET)
	./$(TARGET) --bench 1

clean:
	rm -rf build .config-* $(TARGET)

help:
	@sed -n '/^#  make  /,/^# ----/p' Makefile

# ============================================================================
#  ПОД КАКИЕ КАРТЫ СОБИРАТЬ
#
#  Та же развилка, что с NATIVE на маке, только последствия жёстче. Там
#  неподходящая сборка теряла проценты скорости; здесь она просто не
#  запустится: у видеокарт машинный код разный для каждого поколения.
#
#  nvcc -arch=sm_89 даёт файл, который пойдёт ТОЛЬКО на 8.9 (RTX 4050, 4060,
#  4090). На RTX 3050 (8.6) он не запустится, на RTX 5090 (12.0) тоже.
#
#  СПИСОК АРХИТЕКТУР НЕ ВПИСАН РУКАМИ, А СПРОШЕН У nvcc.
#
#  Сперва он был вписан: 7.5, 8.6, 8.9, 12.0. Список из четырёх строк, который
#  устаревает ровно так же, как таблица моделей видеокарт, -- и от которой мы
#  в gpu_info.cpp отказались по этой же причине. Выйдет новое поколение, и
#  «универсальная» сборка его не увидит; а на CUDA старее 12.8 строка sm_120
#  вовсе не соберётся, и человек получит ошибку там, где просил универсальность.
#
#  Поэтому спрашиваем: nvcc --list-gpu-arch печатает всё, что этот Toolkit
#  умеет. Берём каждую, плюс PTX самой новой -- промежуточное представление,
#  которое драйвер переведёт сам под карту, вышедшую позже сборки.
#
#  PTX работает только ВВЕРХ: он выручит на будущих картах, но не заменит
#  машинный код для старых, поэтому старые перечислены явно.
#
#  Цена -- размер файла и время сборки, по разу на поколение. За
#  переносимость платят этим.
NVCC ?= nvcc

# Все архитектуры, известные этому nvcc. compute_XX -> sm_XX.
CUDA_ARCHS := $(shell $(NVCC) --list-gpu-arch 2>/dev/null | sed 's/compute_//' | sort -n -u)

# Машинный код под каждую + PTX самой новой.
GENCODE_ALL := $(foreach a,$(CUDA_ARCHS),-gencode arch=compute_$(a),code=sm_$(a)) \
               $(if $(CUDA_ARCHS),-gencode arch=compute_$(lastword $(CUDA_ARCHS)),code=compute_$(lastword $(CUDA_ARCHS)))

# По умолчанию -- универсально. Под свою карту: make ... GENCODE="-arch=native"
GENCODE ?= $(GENCODE_ALL)

# Показать, что вышло. Полезно до долгой сборки: видно, какие карты поддержаны
# и не пуст ли список (пустой значит, что nvcc не найден).
gpu-arch:
	@if [ -z "$(CUDA_ARCHS)" ]; then \
	  echo "  nvcc не найден или не отвечает — список архитектур пуст."; \
	  echo "  CUDA Toolkit: https://developer.nvidia.com/cuda-downloads"; \
	  echo "  На Apple Silicon CUDA не существует."; \
	else \
	  echo "  Ваш nvcc умеет: $(CUDA_ARCHS)"; \
	  echo ""; \
	  echo "  Кому это отвечает:"; \
	  echo "    75   GTX 16xx, RTX 20xx"; \
	  echo "    86   RTX 30xx"; \
	  echo "    89   RTX 40xx"; \
	  echo "   120   RTX 50xx"; \
	  echo ""; \
	  echo "  Будет собрано под все перечисленные плюс PTX самой новой,"; \
	  echo "  чтобы пошло и на картах, вышедших после этой сборки."; \
	fi
	@echo ""
	@echo "  Своя карта: nvidia-smi --query-gpu=name,compute_cap --format=csv"

# Советник по видеокарте. Собирается БЕЗ CUDA -- через nvidia-smi, поэтому
# посмотреть его можно на любой машине, в том числе на MacBook.
gpu-info: gpu_info.cpp gpu_main.cpp gpu_info.h single_instance.h
	$(CXX) -I. -std=c++17 -O2 -DGPU_ADVICE_STANDALONE -o pkgpu gpu_info.cpp gpu_main.cpp
	@echo "Готово: ./pkgpu"

# То же, но с опросом через CUDA Runtime API: точные числа от самой карты.
gpu-info-cuda: gpu_info.cpp gpu_main.cpp gpu_info.h single_instance.h
	$(CXX) -I. -std=c++17 -O2 -DGPU_ADVICE_STANDALONE -DPKH_HAVE_CUDA \
	       -I/usr/local/cuda/include -o pkgpu \
	       gpu_info.cpp gpu_main.cpp -L/usr/local/cuda/lib64 -lcudart
	@echo "Готово: ./pkgpu  (с опросом через CUDA)"

# Проверка советника. Не требует видеокарты: разбор и арифметика -- чистые
# функции от чисел, им безразлично, откуда числа взялись.
gpu-test: gpu_info.cpp tests/unit/gpu_selftest.cpp gpu_info.h
	$(CXX) -I. -std=c++17 -O2 -o pkgputest gpu_info.cpp tests/unit/gpu_selftest.cpp
	./pkgputest

.PHONY: gpu-info gpu-info-cuda gpu-test gpu-arch

# ============================================================================
#  СБОРКА ПОД WINDOWS
#
#  Три способа, по убыванию распространённости. Все дают обычный .exe, который
#  копируется на любую машину рядом с базой -- как сейчас на маке.
#
#  1. MSVC (Visual Studio Build Tools) -- родной для Windows.
#     В «x64 Native Tools Command Prompt»:
#         nmake /f Makefile.win
#
#  2. MinGW-w64 -- этот же Makefile работает как есть:
#         mingw32-make
#
#  3. Кросс-сборка с Linux/WSL:
#         make win
#
#  Кириллица в консоли включается самой программой (plat_console_utf8),
#  кодовую страницу руками менять не нужно.
CROSS ?= x86_64-w64-mingw32-g++

win:
	@command -v $(CROSS) >/dev/null 2>&1 || { \
	  echo "  $(CROSS) не найден."; \
	  echo "  Ubuntu/WSL:  sudo apt install mingw-w64"; \
	  exit 1; }
	$(CROSS) -I. -std=c++17 -O2 -static -o pubkeyhunt.exe \
	    $(SRC) scan_secp.c -DUSE_SECP -I$(SECP_DIR) -I$(SECP_DIR)/src \
	    -static-libgcc -static-libstdc++
	@echo "Готово: pubkeyhunt.exe"

# Проверка слоя систем. ЭТО ЗАПУСКАЕТСЯ ПЕРВЫМ на новой системе.
#
# Windows-часть platform.cpp написана, но не собиралась ни разу. Пока эти
# проверки не пройдут, запускать поиск нельзя: там сохранение позиции,
# создание папки находок и права на файлы с приватными ключами.
# Поле для видеокарты против проверенной реализации. Видеокарта не нужна:
# cuda_field.cuh собирается и обычным компилятором -- в этом весь замысел.
# Именно этот тест нашёл потерю переноса в fe_mul основной программы.
field-test: cuda_field.cuh tests/unit/cuda_field_test.cpp
	$(CXX) -I. -std=c++17 -O2 -o pkfieldtest tests/unit/cuda_field_test.cpp platform.cpp
	./pkfieldtest

# Сходятся ли два файла сборки. Дублирование убрать нельзя (nmake не GNU
# make), поэтому делаем его заметным.
check-makefiles:
	@python3 check_makefiles.py

plat-test: platform.cpp tests/unit/plat_selftest.cpp platform.h
	$(CXX) -I. -std=c++17 -O2 -o plat_selftest platform.cpp tests/unit/plat_selftest.cpp
	./plat_selftest

plat-test-win:
	$(CROSS) -I. -std=c++17 -O2 -static -o plat_selftest.exe platform.cpp tests/unit/plat_selftest.cpp
	@echo "Готово: plat_selftest.exe — запустите его на Windows ПЕРВЫМ"

.PHONY: win plat-test plat-test-win check-makefiles field-test

# ============================================================================
#  ЯДРА CUDA
#
#  Собираются только там, где есть nvcc. Процессорная часть от них не зависит
#  и собирается везде -- это разделение нарочное: пока ядра не проверены на
#  настоящей карте, они не должны мешать работать тому, что работает.
# ============================================================================

# Что процессор готовит для карты. Видеокарта НЕ нужна -- в этом весь замысел:
# таблица кратных, начальные точки нитей и покрытие отрезка проверяются здесь.
# Именно этот тест нашёл дыру: центр пакета не проверялся, верхний край
# заезжал на соседнюю нить.
host-test: cuda_host.cpp tests/unit/cuda_host_test.cpp cuda_batch.cuh cuda_field.cuh
	$(CXX) -I. -std=c++17 -O2 -o pkhosttest tests/unit/cuda_host_test.cpp cuda_host.cpp platform.cpp
	./pkhosttest

# Ядра. Список архитектур -- из GENCODE, то есть от самого nvcc.
cuda: cuda_scan.cu cuda_host.cpp cuda_scalar_guard.h
	$(NVCC) -I. -std=c++17 -O3 $(GENCODE) -c cuda_scan.cu -o cuda_scan.o
	$(CXX)  -I. -std=c++17 -O2 -c cuda_host.cpp -o cuda_host.o
	@echo "Готово: cuda_scan.o, cuda_host.o"

# Карта против процессора, ключ в ключ. ЭТО ЗАПУСКАТЬ ПЕРВЫМ НА КАРТЕ.
cuda-test: cuda_scan.cu cuda_host.cpp tests/unit/cuda_selftest.cu cuda_scalar_guard.h
	$(NVCC) -I. -std=c++17 -O2 $(GENCODE) -o pkcudatest \
	    tests/unit/cuda_selftest.cu cuda_scan.cu cuda_host.cpp platform.cpp
	./pkcudatest

filter-test:
	$(CXX) -I. -std=c++17 -O2 -DPKH_BLOOM_MUL32=0 -o filter_parity_test tests/unit/filter_parity_test.cpp
	./filter_parity_test
	$(CXX) -I. -std=c++17 -O2 -DPKH_BLOOM_MUL32=1 -o filter_parity_test_mul32 tests/unit/filter_parity_test.cpp
	./filter_parity_test_mul32

gpu-plan-test:
	$(CXX) -I. -std=c++17 -O2 -DCUDA_HALF=512 -o gpu_plan_test tests/unit/gpu_plan_test.cpp gpu_info.cpp platform.cpp
	./gpu_plan_test

targets-test:
	$(CC) -std=c11 -O2 -Isecp256k1 -Isecp256k1/src -c scan_secp.c -o scan_targets_test_scan.o
	$(CXX) -I. -std=c++17 -O2 -o scan_targets_test tests/unit/scan_targets_test.cpp scan_targets_test_scan.o
	./scan_targets_test

hybrid-test:
	$(CXX) -I. -std=c++17 -O2 -pthread -o hybrid_test tests/unit/hybrid_test.cpp hybrid_search.cpp platform.cpp
	./hybrid_test

prefetch-test:
	$(CXX) -I. -std=c++17 -O2 -pthread -o prefetch_test tests/unit/prefetch_test.cpp zone_search.cpp hybrid_search.cpp platform.cpp
	./prefetch_test

cpu-tests: filter-test field-test host-test gpu-plan-test targets-test hybrid-test prefetch-test

.PHONY: host-test cuda cuda-test filter-test gpu-plan-test targets-test hybrid-test prefetch-test cpu-tests
