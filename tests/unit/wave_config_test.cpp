#include "common.h"
#include "traverse.h"
#include <fstream>
#include <set>
#include <cstdio>
// Run only in an empty scratch directory: this test creates config.ini.
int main() {
 if(std::ifstream("config.ini").good()) {std::puts("Refusing to overwrite existing config.ini");return 2;}
 int failed=0; std::set<std::string> names;
 {std::ofstream f("config.ini"); f << "mode=wave\n";}
 Config old; load_config_ini(old); if(old.wave_nibbles!=4) ++failed;
 for(int n : {3,4,5,8,10}) {
  Config c; c.wave_nibbles=n; c.mode=TRAV_WAVE; c.tg_token.clear(); c.tg_chat.clear();
  save_config_ini(c); Config d; load_config_ini(d); sanitize_config(d);
  if(d.wave_nibbles!=n) ++failed;
  g_cfg=d; g_cfg.det_hex=-1; g_cfg.range_start_bit=70; g_cfg.range_end_bit=71;
  set_block_nibbles(d.wave_nibbles); compute_range();
  if(ZONE_BLOCK!=(1ULL<<(4*n))) ++failed;
  names.insert(checkpoint_path());
  wave_set_seed(12345);
  std::mt19937_64 rng(12345);
  for (unsigned j=0;j<100;++j) {
   ChunkIdx idx;idx.from_u64(j);u256 key,end;uint64_t count=0;int zone=-1;
   if(!next_chunk(idx,key,count,zone,rng)) {++failed;continue;}
   if(!count) continue;
   if(count!=ZONE_BLOCK || (key[0] & (ZONE_BLOCK-1))) ++failed;
   u256_add_u64(end,key,count-1);
   if(u256_cmp(key,g_range_start)<0 || u256_cmp(end,g_range_end)>0) ++failed;
  }
 }
 if(names.size()!=5) ++failed;
 for(int n : {2,11,15}) {std::ofstream f("config.ini"); f<<"wave_nibbles="<<n<<"\n"; f.close(); Config c; load_config_ini(c); if(c.wave_nibbles!=4) ++failed; c.wave_nibbles=n;sanitize_config(c);if(c.wave_nibbles!=4) ++failed;}
 std::printf("Wave config roundtrip, legacy default, invalid bounds and checkpoint separation: %s\n",failed?"FAIL":"PASS");
 return failed?1:0;
}
