#pragma once
#include "zone_search.h"
#include <string>

bool zone_mode_initialize(std::string& why);
bool zone_mode_restore(std::string& why);
void zone_mode_reset();
void zone_mode_describe();
void zone_mode_worker();
void zone_mode_save_checkpoint();
std::string zone_mode_checkpoint_path();
pkh_zone::ZoneStatus zone_mode_status();
