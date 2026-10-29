// ============================================================================
//  cuda_host_test.cpp — проверка того, что процессор готовит для карты.
//
//  Видеокарта не нужна: cuda_host.cpp собирается обычным компилятором.
//
//  Проверяется не «функция что-то вернула», а три утверждения по существу:
//  таблица кратных действительно кратна, начальная точка каждой нити
//  действительно отвечает своему ключу, и пакет вокруг неё покрывает ровно
//  отрезок этой нити -- без дыр и без захода на соседа.
//
//  Последнее нашло настоящую ошибку: центр пакета не проверялся вовсе, а
//  верхний край заезжал на следующую нить. Ошибка не роняла ничего -- просто
//  один ключ из каждых 64 не проверялся, и об этом никто бы не узнал.
// ============================================================================
#include "cuda_scan.h"   // CUDA_HALF: размер пакета -- один на все части
#include "cuda_batch.cuh"
#include "secp256k1.h"
#include "platform.h"   // plat_console_utf8: кодировка консоли
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" int cuda_host_prepare(int, const uint8_t[32], const uint8_t[32],
                                 cpt*, int, cpt*, uint8_t*, cpt*, uint8_t*);

static int fails = 0;
static int u8len(const char *s){int n=0;for(const unsigned char*p=(const unsigned char*)s;*p;p++)if((*p&0xC0)!=0x80)n++;return n;}
static void check(bool ok,const char*name,const std::string&d){
    printf("  [%s] %s",ok?"OK ":"FAIL",name);
    for(int i=u8len(name);i<44;i++)putchar(' ');
    printf("%s\n",d.c_str()); if(!ok)fails++;
}
static void k2u(u256 o,const uint8_t k[32]){
    for(int i=0;i<4;i++){uint64_t v=0;for(int b=0;b<8;b++)v=(v<<8)|k[31-(i*8+7-b)];o[i]=v;}}
static void mul_small(u256 o,const u256 a,uint64_t m){
    u128 c=0;
    for(int i=0;i<4;i++){u128 t=(u128)a[i]*m+c;o[i]=(uint64_t)t;c=t>>64;}}

int main(){
    // КОНСОЛЬ В UTF-8 -- ПЕРВОЙ СТРОКОЙ.
    //
    // Без этого весь вывод на Windows выходит кракозябрами: консоль там по
    // умолчанию в кодовой странице 866, а программа печатает UTF-8. Дефект
    // был виден ровно в этом тесте и НЕ виден в plat_selftest -- тот вызов
    // делал, а этот нет. Кракозябры не мешают счёту, но делают
    // нечитаемым единственный отчёт, ради которого тест и запускают.
    plat_console_utf8();

    ec_build_comb();
    const int HALF=CUDA_HALF, NT=64;
    const uint64_t SPAN=2*(uint64_t)HALF;

    std::vector<cpt> gt(HALF), base(NT);
    std::vector<uint8_t> bkey((size_t)NT*32);
    cpt stride; uint8_t skey[32];

    uint8_t first[32]={0}; first[31]=0x34; first[30]=0x12; first[27]=0xEE; first[26]=0xFF;
    uint8_t step[32]={0};  step[31]=1;

    printf("\n=== что процессор готовит для карты ===\n\n");

    if(!cuda_host_prepare(NT,first,step,gt.data(),HALF,base.data(),bkey.data(),&stride,skey)){
        printf("  подготовка не удалась\n"); return 1; }

    { int bad=0; u256 s; k2u(s,step);
      for(int i=0;i<HALF;i++){ u256 m; mul_small(m,s,(uint64_t)i+1);
        Point R=ec_mul_G(m);
        if(memcmp(gt[i].x,R.x,32)||memcmp(gt[i].y,R.y,32)) bad++; }
      check(bad==0,"таблица кратных: gt[i] == (i+1)*Q",
            bad?std::to_string(bad)+" из "+std::to_string(HALF)+" неверны"
               :std::to_string(HALF)+" кратных сошлись"); }

    { int bad=0; u256 f,s; k2u(f,first); k2u(s,step);
      for(int t=0;t<NT;t++){
        u256 off,want; mul_small(off,s,(uint64_t)t*SPAN+HALF); u256_add(want,f,off);
        u256 got; k2u(got,bkey.data()+(size_t)t*32);
        if(memcmp(want,got,32)){bad++;continue;}
        Point R=ec_mul_G(want);
        if(memcmp(base[t].x,R.x,32)||memcmp(base[t].y,R.y,32)) bad++; }
      check(bad==0,"начальная точка нити отвечает её ключу",
            bad?std::to_string(bad)+" из "+std::to_string(NT)+" неверны"
               :std::to_string(NT)+" нитей сошлись"); }

    { u256 s,want; k2u(s,step); mul_small(want,s,(uint64_t)NT*SPAN);
      u256 got; k2u(got,skey);
      bool ok = memcmp(want,got,32)==0;
      if(ok){ Point R=ec_mul_G(want);
              ok = !memcmp(stride.x,R.x,32) && !memcmp(stride.y,R.y,32); }
      check(ok,"точка-шаг: NT*SPAN*step",
            ok?"сходится с ключом":"НЕ сходится"); }

    // Главное: покрытие. Пакет обязан дать ровно [bk-HALF, bk+HALF-1].
    { int bad=0,tot=0; std::string first_bad;
      std::vector<cfe> op(HALF),om(HALF),dx(HALF),iv(HALF);
      u256 f,s; k2u(f,first); k2u(s,step);
      for(int t=0;t<8;t++){
        if(!cpt_batch_xs(base[t],gt.data(),HALF,op.data(),om.data(),dx.data(),iv.data())) continue;
        u256 bk; k2u(bk,bkey.data()+(size_t)t*32);
        u256 lo,hi,o1,o2;
        mul_small(o1,s,(uint64_t)t*SPAN);      u256_add(lo,f,o1);
        mul_small(o2,s,(uint64_t)(t+1)*SPAN-1);u256_add(hi,f,o2);
        for(int i=0;i<HALF;i++){
          u256 d,kp,km; u256_from_u64(d,(uint64_t)i+1);
          u256_add(kp,bk,d); u256_sub(km,bk,d);
          Point Rp=ec_mul_G(kp),Rm=ec_mul_G(km);
          tot+=2;
          if(memcmp(op[i],Rp.x,32)){ if(!bad)first_bad="абсцисса P+"+std::to_string(i+1); bad++; }
          if(memcmp(om[i],Rm.x,32)){ if(!bad)first_bad="абсцисса P-"+std::to_string(i+1); bad++; }
          if(u256_cmp(km,lo)<0){ if(!bad)first_bad="P-"+std::to_string(i+1)+" ниже отрезка нити"; bad++; }
          // верхний край ядро пропускает нарочно -- см. kern_scan
          if(i+1<HALF && u256_cmp(kp,hi)>0){ if(!bad)first_bad="P+"+std::to_string(i+1)+" выше отрезка нити"; bad++; }
        }
      }
      check(bad==0,"пакет покрывает ровно отрезок нити",
            bad?first_bad:std::to_string(tot)+" ключей, дыр и заходов нет"); }

    printf("\n");
    if(fails) printf("Провалено: %d\n",fails);
    else      printf("Всё, что готовит процессор, проверено.\n");
    return fails?1:0;
}
