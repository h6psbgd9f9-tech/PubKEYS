// ============================================================================
//  ui.h — определение оборудования, ввод, меню запуска.
// ============================================================================
#pragma once
#include "common.h"

struct HwInfo {
    std::string cpu_name = "неизвестен";
    int cpu_logical = 1, cpu_perf = 0, cpu_eff = 0, rec_threads = 1;
};
extern HwInfo g_hw;

void detect_hardware();
void print_hw_info();

// def_yes -- что выбирает ENTER. Подписано прямо в вопросе.
bool        ask_yes_no(const std::string &prompt, bool def_yes);
int         ask_int(const std::string &prompt, int def_val, int min_val, int max_val);
std::string ask_string(const std::string &prompt, const std::string &def_val);

bool run_startup_menu(int argc, char **argv);

// The native panel supplies a validated configuration and owns confirmation.
// Shared range/checkpoint preparation still follows the console's code path.
void ui_set_noninteractive(bool enabled);
// Only the owning application may enable this after a retained immutable file
// handle proves that the same loaded database and target identity remain valid.
void ui_set_preloaded_database(bool enabled);
