#pragma once
#include "hybrid_search.h"
#include <string>

bool hybrid_mode_initialize(std::string& why);
bool hybrid_mode_restore(std::string& why);
void hybrid_mode_reset();
void hybrid_mode_describe();
void hybrid_mode_worker();
void hybrid_mode_save_checkpoint();
std::string hybrid_mode_checkpoint_path();
pkh_hybrid::HybridStatus hybrid_mode_status();
