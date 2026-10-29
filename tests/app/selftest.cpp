#include "worker_threads.h"
// ============================================================================
//  selftest.cpp — проверки корректности и замер скорости.
// ============================================================================
#include "tests/app/selftest.h"
#include "common.h"
#include "targets.h"
#include "engine.h"
#include "traverse.h"
#include "search.h"
#include "notify.h"
#include "wave_gpu_batch.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <map>
#include <thread>
#include <vector>
#include "tests/app/range_audit.h"
#include "tests/app/strict_range_test.h"
#ifdef PKH_HAVE_CUDA
#include "cuda_scan.h"
#endif

// САМОПРОВЕРКА
// ========================================================

// ----------------------------------------------------------------------------
//  Переходники для 256-битного номера участка.
//
//  Номер стал 256-битным, а тесты гоняют маленькие номера, которые удобнее
//  писать обычным числом. Две коротких функции избавляют от ручной возни в
//  каждой из тридцати проверок и не дают перепутать направление.
// ----------------------------------------------------------------------------
static ChunkIdx CI(uint64_t v) { ChunkIdx c; c.from_u64(v); return c; }
static uint64_t CIU(const ChunkIdx &c) {
    uint64_t v = 0;
    for (int i = 24; i < 32; i++) v = (v << 8) | c.b[i];
    return v;
}

int self_test() {
    int fails = 0;
    struct ZoneSettingRestore {
        int previous = g_cfg.zone_nibbles;
        ~ZoneSettingRestore() { g_cfg.zone_nibbles = previous; }
    } zone_setting_restore;
    g_cfg.zone_nibbles = 3;

    // ПРОВЕРКИ УЗОРА ИДУТ НА ОПОРНОМ РАЗМЕРЕ БЛОКА.
    //
    // Размер блока зонального и волнового режима стал настройкой запуска: с
    // картой он 8 нибблов, без карты 3. Числа 61 разряд, 241 фаза, 3616
    // состояний, 4096 ключей в блоке -- вписаны в проверки ниже прямо, и это
    // правильно: проверка обязана знать ответ заранее, иначе она проверяет
    // формулу против неё самой. Поэтому здесь размер ставится в опорные 3
    // ниббла, а то, что при другом размере всё пересчитывается верно, ловит
    // отдельная проверка в конце.
    const int saved_block_nib = g_skip_low;
    set_block_nibbles(3);
    // %-30s считает байты, а кириллица в UTF-8 занимает по два — столбцы
    // разъезжались. Дополняем по числу символов, а не байтов.
    auto pad = [](const std::string &s, size_t w) {
        size_t chars = 0;
        for (unsigned char c : s) if ((c & 0xC0) != 0x80) chars++;
        return s + std::string(chars < w ? w - chars : 1, ' ');
    };
    auto chk = [&](const std::string &name, const std::string &got, const std::string &exp) {
        bool ok = (got == exp);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << pad(name, 30) << got << "\n";
        if (!ok) { std::cout << "        ожидалось: " << exp << "\n"; fails++; }
    };

    uint8_t h[32];
    sha256((const uint8_t *)"abc", 3, h);
    chk("SHA256(\"abc\")", to_hex(h, 32),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    uint8_t r[20];
    ripemd160((const uint8_t *)"abc", 3, r);
    chk("RIPEMD160(\"abc\")", to_hex(r, 20), "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");

    u256 k; u256_from_u64(k, 1);
    Point p = ec_mul_G(k);
    uint8_t xb[32], pc[33];
    fe_to_be32(xb, p.x);
    pc[0] = (uint8_t)(0x02 | (p.y[0] & 1));
    memcpy(pc + 1, xb, 32);
    chk("PUBLIC KEY (k=1)", to_hex(pc, 33),
        "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798");

    uint8_t hh[20];
    hash160(pc, 33, hh);
    chk("Адрес (k=1) c", addr_from_hash160(hh), "1BgGZ9tcN4rm9KBzDn7KprQz87SZ26SAMH");

    uint8_t pu[65];
    pu[0] = 0x04; fe_to_be32(pu + 1, p.x); fe_to_be32(pu + 33, p.y);
    hash160(pu, 65, hh);
    chk("Адрес (k=1) u", addr_from_hash160(hh), "1EHNa6Q4Jz2uvNExL497mE43ikXhwF6kZm");

    uint8_t kb[32] = {0}; kb[31] = 1;
    chk("WIF (k=1) c", wif_from_key(kb, true),
        "KwDiBf89QgGbjEhKnhXJuH7LrciVrZi3qYjgd9M7rFU73sVHnoWn");

    // головоломка #20 — публичный ключ решённого ключа 0xD2C55
    hex_to_u256("D2C55", k);
    p = ec_mul_G(k);
    fe_to_be32(xb, p.x);
    pc[0] = (uint8_t)(0x02 | (p.y[0] & 1));
    memcpy(pc + 1, xb, 32);
    hash160(pc, 33, hh);
    chk("Головоломка #20 адрес", addr_from_hash160(hh), "1HsMJxNiV7TLxmoF6uJNkydxPFDog4NQum");

    // fe_sqr должно совпадать с fe_mul(a,a) — отдельная реализация легко
    // расходится с общей на переносах, а поймать это потом почти нечем
    {
        bool sq_ok = true;
        std::mt19937_64 rng(12345);
        for (int t = 0; t < 20000; t++) {
            fe a, s1, s2;
            for (int i = 0; i < 4; i++) a[i] = rng();
            fe_reduce_once(a);
            fe_sqr(s1, a);
            fe_mul(s2, a, a);
            if (!fe_eq(s1, s2)) { sq_ok = false; break; }
        }
        std::cout << "  [" << (sq_ok ? "OK " : "FAIL") << "] fe_sqr == fe_mul(a,a)\n";
        if (!sq_ok) fails++;
    }

    // таблица кратных G и шаг пакета
    bool tbl_ok = true;
    for (int i : {0, 1, 7, 100, HALF - 1}) {
        u256 kk; u256_from_u64(kk, (uint64_t)i + 1);
        Point ref = ec_mul_G(kk);
        if (!fe_eq(GT[i].x, ref.x) || !fe_eq(GT[i].y, ref.y)) tbl_ok = false;
    }
    { u256 kk; u256_from_u64(kk, SPAN);
      Point ref = ec_mul_G(kk);
      if (!fe_eq(GSTEP.x, ref.x) || !fe_eq(GSTEP.y, ref.y)) tbl_ok = false; }
    std::cout << "  [" << (tbl_ok ? "OK " : "FAIL") << "] GT[i]==(i+1)G, GSTEP==" << SPAN << "G\n";
    if (!tbl_ok) fails++;

    // симметричный пакет: обе половины должны совпасть с честным k*G
    {
        bool sym_ok = true;
        u256 base; hex_to_u256("9A7C1F00E5", base);
        Point P = ec_mul_G(base);
        fe d[8], iv[8], sc[8];
        for (int i = 0; i < 8; i++) fe_sub(d[i], GT[i].x, P.x);
        fe_inv_batch(iv, d, 8, sc);
        for (int i = 0; i < 8; i++) {
            fe s, t, xp, xm;
            fe_sub(t, GT[i].y, P.y); fe_mul(s, t, iv[i]); fe_sqr(xp, s);
            fe_sub(xp, xp, P.x); fe_sub(xp, xp, GT[i].x);
            fe_add(t, GT[i].y, P.y); fe_mul(s, t, iv[i]); fe_sqr(xm, s);
            fe_sub(xm, xm, P.x); fe_sub(xm, xm, GT[i].x);

            u256 kp, km;
            u256_add_u64(kp, base, (uint64_t)i + 1);
            u256_sub_u64(km, base, (uint64_t)i + 1);
            Point rp = ec_mul_G(kp), rm = ec_mul_G(km);
            if (!fe_eq(xp, rp.x) || !fe_eq(xm, rm.x)) sym_ok = false;
        }
        std::cout << "  [" << (sym_ok ? "OK " : "FAIL") << "] симметричный пакет P+-iG\n";
        if (!sym_ok) fails++;
    }

    // Reject malformed curve points before they enter the target database.
    {
        PubDB testdb;
        const std::string gx = "79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";
        bool ok = !testdb.add_hex("04" + gx + std::string(64, '0'))
               && !testdb.add_hex("02" + std::string(64, 'F'))
               && !testdb.add_hex(std::string(64, 'F'))
               && testdb.items.empty() && testdb.n_comp == 0 && testdb.n_uncomp == 0
               && testdb.add_hex("02" + gx) && testdb.add_hex(gx);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] база: неверные X/Y отвергнуты, валидные 02 и X приняты\n";
        if (!ok) ++fails;
    }

    // User database examples: compressed/uncompressed must preserve Y parity.
    {
        const char* compressed[] = {
            "021c634df6666de0618f214e8b52fc1898d85106fe4475ca025a0f32585e965934",
            "03d6597d465408e6e11264c116dd98b539740e802dc756d7eb88741696e20dfe7d"
        };
        const char* uncompressed[] = {
            "041c634df6666de0618f214e8b52fc1898d85106fe4475ca025a0f32585e965934ca6eb8bc792645b71ff960c1ec5e1523f7cded548158089f8c9a1b0a472545b8",
            "04d6597d465408e6e11264c116dd98b539740e802dc756d7eb88741696e20dfe7d3588695d2e7ad23cbf0aa056d42afada63036d66a1d9b97070dd6bc0c87ceb0d"
        };
        bool ok = true;
        for (int i = 0; i < 2; ++i) {
            Point a, b;
            if (!point_from_pubkey(compressed[i], a) || !point_from_pubkey(uncompressed[i], b)) {
                ok = false; continue;
            }
            ok = ok && fe_eq(a.x, b.x) && fe_eq(a.y, b.y);
            PubDB db;
            ok = ok && db.add_hex(uncompressed[i]); db.build();
            const int parity = (int)(a.y[0] & 1);
            ok = ok && db.maybe(a.x) && db.confirm(a.x, parity) == 1
                && db.confirm(a.x, parity ^ 1) == 2;
            ok = ok && db.add_hex(compressed[i]) && db.add_hex(uncompressed[i]); db.build();
            ok = ok && db.items.size() == 1 && db.confirm(a.x, parity) == 1;
            std::string opposite = compressed[i]; opposite[1] = parity ? '2' : '3';
            ok = ok && db.add_hex(opposite); db.build();
            ok = ok && db.items.size() == 1 && db.confirm(a.x, parity ^ 1) == 1;
        }
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] user 02/03/04 examples: same point, parity and duplicates\n";
        if (!ok) ++fails;
    }

#ifdef PKH_HAVE_CUDA
    if (g_gpu_active) {
        // Exercise the engine's real replay loop at the exact buffer limit.
        // The only actual target (G) is outside the scanned range: every GPU
        // candidate is deliberately false and must be confirmed on the host.
        g_quiet = true;
        g_db = PubDB();
        g_db.add_hex("0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798");
        g_db.build();
        if (!push_targets_to_backend()) return 1;
        uint64_t saturated[BLOOM_BLOCK_WORDS];
        for (auto& word : saturated) word = UINT64_MAX;
        char why[256]{};
        bool ok = cuda_scan_set_targets(nullptr, 9, saturated, 0, why, sizeof(why)) != 0;
        u256 from; u256_from_u64(from, 1000000);
        u256_set(g_range_start, from);
        u256_add_u64(g_range_end, from, 2 * CUDA_MAX_OUT - 1);
        g_count.store(0);
        if (ok) scan_chunk(from, 2 * CUDA_MAX_OUT);
        ok = ok && g_count.load() == 2 * CUDA_MAX_OUT && !g_stop.load();
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] engine: saturated buffer replay, 131072 keys counted once\n";
        if (!ok) ++fails;
    }
#endif

    // Search through the real engine: both parities, both serializations,
    // and both prefilters. CUDA builds exercise GPU candidate -> CPU confirm.
    {
        bool ok = true;
        g_quiet = true;
        for (int parity : {0, 1}) for (bool full : {false, true}) for (bool bloom : {false, true}) {
            u256 key; u256_from_u64(key, 1000000);
            Point p = ec_mul_G(key);
            while ((int)(p.y[0] & 1) != parity) {
                u256_add_u64(key, key, 1); p = ec_mul_G(key);
            }
            uint8_t pub[65]{}, kb[32]{};
            pub[0] = full ? 4 : (uint8_t)(2 + parity);
            fe_to_be32(pub + 1, p.x);
            if (full) fe_to_be32(pub + 33, p.y);
            fe_to_be32(kb, key);
            g_db = PubDB();
            ok = ok && g_db.add_hex(to_hex(pub, full ? 65 : 33));
            if (bloom) for (uint64_t i = 1; i <= 8; ++i) {
                u256 decoy; u256_from_u64(decoy, i);
                Point q = ec_mul_G(decoy);
                uint8_t x[32]; fe_to_be32(x, q.x);
                ok = ok && g_db.add_hex(to_hex(x, 32));
            }
            g_db.build();
            if (!push_targets_to_backend()) return 1;
            u256 from; u256_sub_u64(from, key, 100);
            u256_set(g_range_start, from);
            u256_add_u64(g_range_end, from, 4095);
            g_last_found.clear(); g_test_found.clear();
            scan_chunk(from, 4096);
            ok = ok && g_last_found == to_hex(kb, 32) && g_test_found.size() == 1;
        }
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] search 02/03/04: 8 cases, both Y parities, direct and Bloom\n";
        if (!ok) ++fails;
    }

    // сквозной поиск публичного ключа в диапазоне
    {
        g_quiet = true;
        g_db.items.clear();
        g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
        g_db.add_hex(to_hex(pc, 33));     // публичный ключ головоломки #20
        g_db.build();
        if (!push_targets_to_backend()) return 1;

        g_last_found.clear();
        u256 from; hex_to_u256("D0000", from);
        // Диапазон выставляется явно: горячий цикл теперь сверяет находку с
        // [g_range_start, g_range_end], и тест обязан задать его честно.
        u256_set(g_range_start, from);
        u256_add_u64(g_range_end, from, 20000 - 1);
        scan_chunk(from, 20000);
        bool ok = (g_last_found == std::string(59, '0') + "d2c55");
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << pad("сквозной поиск по pubkey", 30)
                  << (g_last_found.empty() ? "(не найдено)" : g_last_found) << "\n";
        if (!ok) fails++;

        // зеркало n-k: тот же X, другая чётность
        g_db.items.clear();
        std::string mirror_pub = std::string(pc[0] == 0x02 ? "03" : "02") + to_hex(xb, 32);
        g_db.add_hex(mirror_pub);
        g_db.build();
        if (!push_targets_to_backend()) return 1;
        g_cfg.want_nk = true;
        g_last_found.clear();
        scan_chunk(from, 20000);
        // The source is inside this narrow interval. Its enabled mirror must
        // still be reported even though n-k lies outside that interval.
        u256 kk; hex_to_u256("D2C55", kk);
        u256 nk; u256_sub(nk, EC_N, kk);
        uint8_t nb[32]; u256_to_be32(nb, nk);
        bool ok2 = g_last_found == to_hex(nb, 32);
        std::cout << "  [" << (ok2 ? "OK " : "FAIL") << "] " << pad("зеркало n-k", 30)
                  << (g_last_found.empty() ? "(не найдено)" : g_last_found) << "\n";
        if (!ok2) fails++;
        g_cfg.want_nk = false;

        // ====================================================================
        //  HEX-МАСКА: КЛЮЧ НА РЕШЁТКЕ С ШАГОМ 16^N
        //
        //  Маска морозит младшие ниббли, и кандидаты идут не подряд, а через
        //  16^N. Пакетное сложение это выдерживает -- прогрессия остаётся
        //  прогрессией, -- но только если шаг проведён ВЕЗДЕ: в таблице
        //  кратных, в сдвиге базы, в границах вырождения и в обратном
        //  пересчёте найденного ключа.
        //
        //  Ошибка в пересчёте не роняет программу и не выдаёт чужой ключ:
        //  неверный ключ просто не подтвердится по базе, и попадание молча
        //  исчезнет. Поэтому тест проверяет, что подложенный ключ НАЙДЕН.
        //  Отсутствие ложных срабатываний здесь не доказывает ничего.
        // ====================================================================
        {
            const int  NFROZ = 5;                       // младших нибблов заморожено
            u256 S; u256_from_u64(S, 1ULL << (4 * NFROZ));   // 16^5 = 2^20
            u256 K0; hex_to_u256("C0FFEE12345", K0);    // начало решётки; хвост 12345 нарочно
                                                        // не нулевой -- нулевой хвост
                                                        // сохранился бы и по случайности
            const uint64_t J = 3000;                    // какой по счёту кандидат

            u256 off; { u128 c = 0;        // off = J*S
                        for (int i = 0; i < 4; i++) {
                            u128 v = (u128)S[i] * J + c;
                            off[i] = (uint64_t)v; c = v >> 64; } }
            u256 KT; u256_add(KT, K0, off);             // искомый ключ

            Point tp = ec_mul_G(KT);
            uint8_t txb[32], tc[33];
            fe_to_be32(txb, tp.x);
            tc[0] = (uint8_t)(0x02 | (tp.y[0] & 1));
            memcpy(tc + 1, txb, 32);

            g_db.items.clear();
            g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
            g_db.add_hex(to_hex(tc, 33));
            g_db.build();
            if (!push_targets_to_backend()) return 1;

            const uint64_t CNT = 4096;                  // кандидатов на решётке
            u256 span_all; { u128 c = 0;
                        for (int i = 0; i < 4; i++) {
                            u128 v = (u128)S[i] * CNT + c;
                            span_all[i] = (uint64_t)v; c = v >> 64; } }
            u256_set(g_range_start, K0);
            u256_add(g_range_end, K0, span_all);

            uint8_t ktb[32]; u256_to_be32(ktb, KT);
            const std::string want = to_hex(ktb, 32);

            // 1) С ЕДИНИЧНЫМ шагом тот же вызов НЕ должен находить ключ:
            //    он лежит в 3000*2^20 отсюда. Это страховка от теста, который
            //    проходит по случайности.
            g_last_found.clear();
            scan_chunk(K0, CNT);
            const bool miss_with_step1 = (g_last_found != want);

            // 2) С шагом 16^5 -- обязан найтись.
            set_scan_step(S);
            g_last_found.clear();
            scan_chunk(K0, CNT);
            const bool hit_with_step = (g_last_found == want);

            u256 one; u256_from_u64(one, 1);
            set_scan_step(one);                          // вернуть как было

            const bool ok3 = miss_with_step1 && hit_with_step;
            std::cout << "  [" << (ok3 ? "OK " : "FAIL") << "] "
                      << pad("маска: шаг 16^5, ключ найден", 30)
                      << (ok3 ? "кандидат 3000 из 4096 найден, шаг 1 его не видит"
                              : (!hit_with_step
                                 ? "НЕ НАЙДЕН: " + (g_last_found.empty()
                                        ? std::string("(пусто)") : g_last_found)
                                 : "шаг ни на что не влияет"))
                      << "\n";
            if (!ok3) fails++;

            // ================================================================
            //  ВЫРОЖДЕННЫЙ ПАКЕТ ПРИ БОЛЬШОМ ШАГЕ
            //
            //  Пакет неприменим, когда центр ближе к нулю (или к n), чем его
            //  собственная ширина: среди разностей абсцисс появляется нуль.
            //  Ширина эта -- SPAN*S, а проверка сравнивала центр с константой
            //  SPAN. При шаге 1 разницы нет, при 16^N разница в 16^N раз:
            //  вырожденный пакет уходил бы по быстрому пути и считал мусор,
            //  ничего об этом не сообщая.
            //
            //  Здесь центр нарочно мал: 513*S против ширины 1024*S.
            // ================================================================
            {
                u256 KS; u256_set(KS, S);              // старт = S, центр = 513*S
                const uint64_t JD = 100;
                u256 od; { u128 c = 0;
                           for (int i = 0; i < 4; i++) {
                               u128 v = (u128)S[i] * JD + c;
                               od[i] = (uint64_t)v; c = v >> 64; } }
                u256 KD; u256_add(KD, KS, od);

                Point dp = ec_mul_G(KD);
                uint8_t dxb[32], dc[33];
                fe_to_be32(dxb, dp.x);
                dc[0] = (uint8_t)(0x02 | (dp.y[0] & 1));
                memcpy(dc + 1, dxb, 32);

                g_db.items.clear();
                g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
                g_db.add_hex(to_hex(dc, 33));
                g_db.build();
                if (!push_targets_to_backend()) return 1;

                u256 sp2; { u128 c = 0;
                            for (int i = 0; i < 4; i++) {
                                u128 v = (u128)S[i] * 4096ULL + c;
                                sp2[i] = (uint64_t)v; c = v >> 64; } }
                u256_set(g_range_start, KS);
                u256_add(g_range_end, KS, sp2);

                uint8_t kdb[32]; u256_to_be32(kdb, KD);
                const std::string want_d = to_hex(kdb, 32);

                set_scan_step(S);
                g_last_found.clear();
                scan_chunk(KS, 4096);
                const bool ok4 = (g_last_found == want_d);
                u256 one2; u256_from_u64(one2, 1);
                set_scan_step(one2);

                std::cout << "  [" << (ok4 ? "OK " : "FAIL") << "] "
                          << pad("маска: вырожденный пакет у нуля", 30)
                          << (ok4 ? "центр 513*S < ширины 1024*S — ключ найден"
                                  : "ПОТЕРЯН: " + (g_last_found.empty()
                                        ? std::string("(пусто)") : g_last_found))
                          << "\n";
                    if (!ok4) fails++;
            }

            // ================================================================
            //  ОБРАЗЕЦ «ПРОВЕРКИ» ОБЯЗАН ЛЕЖАТЬ НА РЕШЁТКЕ
            //
            //  Ключ из этого блока человек берёт с экрана и сверяет чужим
            //  инструментом. Смещение внутри участка -- номер КАНДИДАТА;
            //  прибавленное к ключу напрямую, оно сдвигает младшие ниббли,
            //  то есть ровно те, которые маска объявила неизменными. Ключ
            //  при этом остаётся настоящим и сходится при любой сторонней
            //  проверке -- но принадлежит он не тому множеству, которое
            //  программа перебирает. Проверка успокаивает, ничего не
            //  подтверждая; это хуже, чем её отсутствие.
            //
            //  То же самое уже было со строкой Baza в волновом режиме:
            //  считается одно, показывается другое.
            // ================================================================
            {
                set_scan_step(S);
                u256_set(g_range_start, K0);
                u256_add(g_range_end, K0, span_all);
                g_db.items.clear();
                g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
                g_db.add_hex(to_hex(tc, 33));
                g_db.build();
                if (!push_targets_to_backend()) return 1;

                scan_chunk(K0, 512);

                u256 smp; bool have;
                { std::lock_guard<std::mutex> lk(g_mtx_sample);
                  have = g_sample_valid;
                  if (have) u256_set(smp, g_sample_key); }

                // Замороженная часть образца обязана совпасть с K0 до знака.
                uint8_t sb[32], k0b[32];
                if (have) u256_to_be32(sb, smp);
                u256_to_be32(k0b, K0);
                const std::string s_hex  = have ? to_hex(sb, 32) : std::string();
                const std::string k0_hex = to_hex(k0b, 32);
                const bool tail_same = have &&
                    s_hex.substr(64 - NFROZ) == k0_hex.substr(64 - NFROZ);
                const bool moved = have && u256_cmp(smp, K0) != 0;

                u256 one3; u256_from_u64(one3, 1);
                set_scan_step(one3);

                const bool ok5 = tail_same && moved;
                std::cout << "  [" << (ok5 ? "OK " : "FAIL") << "] "
                          << pad("маска: образец на решётке", 30)
                          << (!have ? std::string("образца нет")
                              : !tail_same
                                ? "хвост уехал: ждали ..." + k0_hex.substr(64 - NFROZ)
                                  + ", вышло ..." + s_hex.substr(64 - NFROZ)
                              : !moved ? std::string("образец совпал с началом участка")
                              : "хвост ..." + s_hex.substr(64 - NFROZ) + " сохранён")
                          << "\n";
                if (!ok5) fails++;
            }

            // ================================================================
            //  ХВОСТ УЧАСТКА: count НЕ КРАТЕН ПАКЕТУ
            //
            //  Участок из count кандидатов разбирается на целые пакеты по
            //  1024 плюс остаток поштучно. Остаток отсчитывался в КЛЮЧАХ, а
            //  не в шагах, и при маске попадал мимо решётки: ключ из хвоста
            //  не проверялся вовсе, молча.
            //
            //  Прежние проверки маски этого не видели, потому что все шли с
            //  count 4096 и 512 -- остатка не возникало ни разу. Здесь
            //  count = SPAN + 5 нарочно: цель кладём в сам хвост.
            //
            //  В своей математике рядом стоял верный вариант -- то есть два
            //  backend'а одним вызовом сканировали разное. Тест обязан идти
            //  на обоих.
            // ================================================================
            {
                const uint64_t TAIL_N = 5;
                const uint64_t CNT2 = (uint64_t)SPAN + TAIL_N;
                const uint64_t JT = (uint64_t)SPAN + 2;      // внутри хвоста

                u256 ot; { u128 c = 0;
                           for (int i = 0; i < 4; i++) {
                               u128 v = (u128)S[i] * JT + c;
                               ot[i] = (uint64_t)v; c = v >> 64; } }
                u256 KTail; u256_add(KTail, K0, ot);

                Point tp2 = ec_mul_G(KTail);
                uint8_t xb2[32], c2[33];
                fe_to_be32(xb2, tp2.x);
                c2[0] = (uint8_t)(0x02 | (tp2.y[0] & 1));
                memcpy(c2 + 1, xb2, 32);

                g_db.items.clear();
                g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
                g_db.add_hex(to_hex(c2, 33));
                g_db.build();
                if (!push_targets_to_backend()) return 1;

                u256 sp3; { u128 c = 0;
                            for (int i = 0; i < 4; i++) {
                                u128 v = (u128)S[i] * (CNT2 + 1) + c;
                                sp3[i] = (uint64_t)v; c = v >> 64; } }
                u256_set(g_range_start, K0);
                u256_add(g_range_end, K0, sp3);

                uint8_t ktb2[32]; u256_to_be32(ktb2, KTail);
                const std::string want2 = to_hex(ktb2, 32);

                set_scan_step(S);
                g_last_found.clear();
                scan_chunk(K0, CNT2);
                const bool ok6 = (g_last_found == want2);
                u256 one4; u256_from_u64(one4, 1);
                set_scan_step(one4);

                std::cout << "  [" << (ok6 ? "OK " : "FAIL") << "] "
                          << pad("маска: хвост участка (count%SPAN)", 30)
                          << (ok6 ? "кандидат " + std::to_string(JT) + " из "
                                    + std::to_string(CNT2) + " найден в хвосте"
                                  : "ПОТЕРЯН: ждали ..." + want2.substr(48)
                                    + ", нашли " + (g_last_found.empty()
                                        ? std::string("(ничего)")
                                        : "..." + g_last_found.substr(48)))
                          << "\n";
                if (!ok6) fails++;
            }
        }
        g_quiet = false;
    }

    // ---- ВЫРОЖДЕННЫЙ ПАКЕТ У НИЖНЕГО КРАЯ ---------------------------------
    // Центр пакета ближе к нулю, чем размер пакета: среди разностей абсцисс
    // есть нулевая, пакетная формула неприменима. В scan_secp.c этот случай
    // разобран, в своей математике нулевая разность подменялась единицей --
    // абсциссы выходили мусором, и ключи такого пакета молча НЕ проверялись.
    // Ключи 1..2500 накрывают ровно эту зону.
    {
        g_quiet = true;
        g_test_found.clear();
        g_db.items.clear();
        g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
        const uint64_t LO = 1, N = 2500;
        for (uint64_t v = LO; v < LO + N; v++) {
            u256 kk; u256_from_u64(kk, v);
            Point pp = ec_mul_G(kk);
            uint8_t bx[32], bc[33];
            fe_to_be32(bx, pp.x);
            bc[0] = (uint8_t)(0x02 | (pp.y[0] & 1));
            memcpy(bc + 1, bx, 32);
            g_db.add_hex(to_hex(bc, 33));
        }
        g_db.build();
        if (!push_targets_to_backend()) return 1;

        u256 from; u256_from_u64(from, LO);
        u256_from_u64(g_range_start, LO);
        u256_from_u64(g_range_end, LO + N - 1);
        scan_chunk(from, N);

        uint64_t miss = 0, first = 0;
        for (uint64_t v = LO; v < LO + N; v++) {
            u256 kk; u256_from_u64(kk, v);
            uint8_t bk[32]; u256_to_be32(bk, kk);
            if (!g_test_found.count(to_hex(bk, 32))) { if (!miss) first = v; miss++; }
        }
        bool ok = (miss == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << pad("вырожденный пакет [1, 2500]", 30);
        if (ok) std::cout << "все найдены\n";
        else    std::cout << "пропущено " << miss << ", первый " << first << "\n";
        if (!ok) fails++;
        g_quiet = false;
        g_test_found.clear();
    }

    // ---- ПОКРЫТИЕ ----------------------------------------------------------
    // Самая ценная проверка. В базу кладутся публичные ключи ВСЕХ ключей
    // небольшого отрезка, и отрезок просматривается боевым кодом. Если хоть
    // один ключ пропущен — он не найдётся, и тест это увидит.
    // Ловит: пропуски в пакете, ошибку шага, промах на границах, лишние или
    // недостающие ключи в хвосте. Ровно тот класс ошибок, который иначе
    // никак не проявляется: программа не падает, она просто молчит.
    {
        g_quiet = true;
        g_test_found.clear();
        g_db.items.clear();
        g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;

        // Мишени ставятся ШИРЕ участка: 1024 ключа до него и 2048 после.
        // Участок при этом заказывается ровно [A, A+CNT). Если движок
        // проверит хоть один ключ вне задания -- лишняя мишень найдётся, и
        // тест это увидит. Без этого «покрытие» отвечало только на вопрос
        // «находит ли движок то, что должен», но не на вопрос «не трогает ли
        // он то, чего не должен».
        const uint64_t A = 0x30000, CNT = 3000;
        const uint64_t GUARD_LO = 1024, GUARD_HI = 2048;
        for (uint64_t v = A - GUARD_LO; v < A + CNT + GUARD_HI; v++) {
            u256 kk; u256_from_u64(kk, v);
            Point pp = ec_mul_G(kk);
            uint8_t bx[32], bc[33];
            fe_to_be32(bx, pp.x);
            bc[0] = (uint8_t)(0x02 | (pp.y[0] & 1));
            memcpy(bc + 1, bx, 32);
            g_db.add_hex(to_hex(bc, 33));
        }
        g_db.build();
        if (!push_targets_to_backend()) return 1;

        u256 from; u256_from_u64(from, A);
        // ДИАПАЗОН НАРОЧНО ШИРЕ УЧАСТКА.
        //
        // Иначе сеть безопасности в secp_hit/slow_confirm отсекла бы лишние
        // ключи сама, и тест проверял бы сеть, а не нарезку. Здесь проверяется
        // именно нарезка: движку разрешено сообщать о ключах из полосы
        // охраны -- если он их тронет, значит вышел за заказанные CNT.
        u256_from_u64(g_range_start, A - GUARD_LO);
        u256_from_u64(g_range_end, A + CNT + GUARD_HI - 1);
        scan_chunk(from, CNT);

        auto seen = [&](uint64_t v) {
            u256 kk; u256_from_u64(kk, v);
            uint8_t bk[32]; u256_to_be32(bk, kk);
            return g_test_found.count(to_hex(bk, 32)) != 0;
        };

        uint64_t missing = 0, first_missing = 0;
        for (uint64_t v = A; v < A + CNT; v++)
            if (!seen(v)) { if (!missing) first_missing = v; missing++; }

        // CNT = 3000 нарочно НЕ делится на пакет: 3000 = 2*1024 + 952. Раньше
        // хвост дочитывался целым пакетом, и ключи от A+3000 и дальше
        // проверялись, хотя заказаны не были.
        uint64_t extra = 0, first_extra = 0;
        for (uint64_t v = A - GUARD_LO; v < A; v++)
            if (seen(v)) { if (!extra) first_extra = v; extra++; }
        for (uint64_t v = A + CNT; v < A + CNT + GUARD_HI; v++)
            if (seen(v)) { if (!extra) first_extra = v; extra++; }

        bool ok = (missing == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << pad("покрытие: 3000 ключей", 30);
        if (ok) std::cout << "все найдены\n";
        else    std::cout << "пропущено " << missing << ", первый 0x"
                          << std::hex << first_missing << std::dec << "\n";
        if (!ok) fails++;

        bool ok_ex = (extra == 0);
        std::cout << "  [" << (ok_ex ? "OK " : "FAIL") << "] " << pad("ничего лишнего за участком", 30);
        if (ok_ex) std::cout << "проверено ровно " << CNT << " ключей\n";
        else       std::cout << "тронуто лишних " << extra << ", первый 0x"
                             << std::hex << first_extra << std::dec << "\n";
        if (!ok_ex) fails++;

        // СЧЁТЧИК ПРОВЕРЕННЫХ.
        //
        // g_count должен совпасть с заказанным числом ключей ровно. Раньше в
        // своей математике он рос на SPAN = 1025 за пакет, то есть быстрее
        // фактического задания, и ошибка копилась линейно -- «проверено
        // ключей», скорость, процент и ETA становились недостоверны.
        // СЕТЬ ПО ДИАПАЗОНУ -- отдельно от нарезки.
        //
        // Просматриваем заведомо больше, чем разрешено, и проверяем, что за
        // границы [g_range_start, g_range_end] не ушло ни одной находки.
        // Это второй, независимый рубеж: даже если нарезка когда-нибудь снова
        // сломается, ключ вне диапазона не попадёт в Found.txt.
        g_test_found.clear();
        u256_from_u64(g_range_start, A);
        u256_from_u64(g_range_end, A + CNT - 1);
        { u256 wide; u256_from_u64(wide, A - GUARD_LO);
          scan_slow_range(wide, GUARD_LO + CNT + GUARD_HI); }
        uint64_t leaked = 0;
        for (uint64_t v = A - GUARD_LO; v < A; v++)                 if (seen(v)) leaked++;
        for (uint64_t v = A + CNT; v < A + CNT + GUARD_HI; v++)     if (seen(v)) leaked++;
        uint64_t inside = 0;
        for (uint64_t v = A; v < A + CNT; v++) if (seen(v)) inside++;
        bool ok_net = (leaked == 0 && inside == CNT);
        std::cout << "  [" << (ok_net ? "OK " : "FAIL") << "] " << pad("сеть: только свой диапазон", 30)
                  << "своих " << inside << " из " << CNT << ", чужих " << leaked << "\n";
        if (!ok_net) fails++;

        g_count.store(0);
        g_test_found.clear();
        u256_from_u64(g_range_start, A);
        u256_from_u64(g_range_end, A + CNT - 1);
        scan_chunk(from, CNT);
        uint64_t cnt = g_count.load();
        bool ok_cnt = (cnt == CNT);
        std::cout << "  [" << (ok_cnt ? "OK " : "FAIL") << "] " << pad("g_count == заказано", 30)
                  << cnt << " из " << CNT << "\n";
        if (!ok_cnt) fails++;
        g_quiet = false;
        g_test_found.clear();
    }

    // ---- ОБЕ ФОРМЫ ПУБЛИЧНОГО КЛЮЧА --------------------------------------
    // Сжатый 02/03||X и несжатый 04||X||Y — это один и тот же ключ, у них
    // общая координата X. Поэтому сравнение по X ловит обе формы одним
    // действием, и ни одна не требует лишней работы. Проверяем, что база
    // действительно принимает все три записи и сводит их к одной цели, и
    // что ключ находится, какой формой его ни задай.
    {
        u256 kk; hex_to_u256("D2C55", kk);
        Point pp = ec_mul_G(kk);
        uint8_t bx[32], by[32];
        fe_to_be32(bx, pp.x);
        fe_to_be32(by, pp.y);
        std::string sx = to_hex(bx, 32);
        std::string comp = std::string((pp.y[0] & 1) ? "03" : "02") + sx;
        std::string uncomp = "04" + sx + to_hex(by, 32);

        bool all_ok = true;
        const char *names[3] = { "сжатая", "несжатая", "только X" };
        std::string forms[3] = { comp, uncomp, sx };
        for (int i = 0; i < 3; i++) {
            g_quiet = true;
            g_test_found.clear();
            g_db.items.clear();
            g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
            if (!g_db.add_hex(forms[i])) { all_ok = false; break; }
            g_db.build();
            if (!push_targets_to_backend()) return 1;
            u256 from; hex_to_u256("D0000", from);
            u256_set(g_range_start, from);
            u256_add_u64(g_range_end, from, 20000 - 1);
            scan_chunk(from, 20000);
            if (g_last_found != std::string(59, '0') + "d2c55") {
                all_ok = false;
                std::cout << "        форма " << names[i] << ": не найдено\n";
            }
            g_quiet = false;
        }

        // все три записи вместе должны дать ровно одну цель
        g_db.items.clear();
        g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
        for (int i = 0; i < 3; i++) g_db.add_hex(forms[i]);
        g_db.build();
        bool merged = (g_db.items.size() == 1);
        if (!merged) all_ok = false;

        std::cout << "  [" << (all_ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("обе формы ключа", 30)
                  << "сжатая, несжатая и голый X -> "
                  << g_db.items.size() << " цель\n";
        if (!all_ok) fails++;
        g_test_found.clear();
    }

    // ---- НАРЕЗКА НА УЧАСТКИ ------------------------------------------------
    // Участки должны укладывать диапазон встык: без дыр и без нахлёстов.
    // Проверяется той же функцией chunk_bounds, которой пользуется рабочий цикл.
    {
        g_cfg.range_start_bit = 10;
        g_cfg.range_end_bit   = 14;
        g_cfg.chunk_bits      = 6;
        compute_range();

        std::vector<char> hit(1 << 14, 0);
        uint64_t idx = 0, guard = 0;
        for (; idx < g_total_chunks && guard < 100000; idx++, guard++) {
            u256 st; uint64_t cnt;
            if (!chunk_bounds(CI(idx), st, cnt)) break;
            for (uint64_t j = 0; j < cnt; j++) {
                uint64_t key = st[0] + j;
                if (key < hit.size()) {
                    if (hit[key]) { hit[key] = 2; }      // нахлёст
                    else hit[key] = 1;
                }
            }
        }
        uint64_t gaps = 0, overlaps = 0;
        for (uint64_t v = 1024; v <= 16383; v++) {
            if (hit[v] == 0) gaps++;
            if (hit[v] == 2) overlaps++;
        }
        bool ok = (gaps == 0 && overlaps == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << pad("нарезка на участки", 30)
                  << g_total_chunks << " участков, дыр " << gaps
                  << ", нахлёстов " << overlaps << "\n";
        if (!ok) fails++;
    }

    // ---- KXE: перестановка ---------------------------------------------
    // Обход обязан пройти каждый блок ровно один раз. Сеть Фейстеля
    // обратима по построению, но cycle-walking легко испортить так, что
    // часть значений начнёт повторяться, а часть выпадет. Проверяем прямо:
    // прогоняем все номера и смотрим, что получилась именно перестановка.
    {
        bool ok = true;
        // МАЛЫЕ И НЕЧЁТНЫЕ ДОМЕНЫ.
        //
        // В kxe_permute число бит Фейстеля делится на половины динамически
        // (wl/wr меняются местами), и именно на маленьких и нечётных по битам
        // доменах это место ломается легче всего: 2, 3, 5, 7 -- домены, где
        // половина шире самого домена, а cycle walking может зациклиться.
        // Раньше проверялись только 1000, 4096 и 65537 -- то есть ни одного
        // такого случая. Перебор здесь исчерпывающий: домены крошечные.
        for (uint64_t total : {2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, 8ULL, 9ULL,
                               15ULL, 16ULL, 17ULL, 31ULL, 32ULL, 33ULL,
                               63ULL, 64ULL, 65ULL, 127ULL, 128ULL, 129ULL,
                               255ULL, 256ULL, 257ULL,
                               1000ULL, 4096ULL, 65537ULL}) {
            std::vector<char> hit(total, 0);
            uint64_t dup = 0, out_of_range = 0;
            for (uint64_t i = 0; i < total; i++) {
                uint64_t y = kxe_permute(i, total, 0xA5A5C3C3ULL);
                if (y >= total) { out_of_range++; continue; }
                if (hit[y]) dup++;
                hit[y] = 1;
            }
            uint64_t missing = 0;
            for (uint64_t i = 0; i < total; i++) if (!hit[i]) missing++;
            if (dup || missing || out_of_range) {
                ok = false;
                std::cout << "        total=" << total << ": повторов " << dup
                          << ", пропущено " << missing
                          << ", вне диапазона " << out_of_range << "\n";
            }
        }
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("KXE: перестановка без повторов", 30)
                  << (ok ? "26 доменов, от 2 до 65537 — все" : "") << "\n";
        if (!ok) fails++;
    }

    // ---- KXE на диапазоне ШИРЕ 2^64 блоков -------------------------------
    // Раньше домен перестановки был uint64_t, и такой диапазон делал KXE
    // недоступным целиком (тихая подмена на случайный режим). Полный
    // перебор здесь невозможен физически (блоков ~2^190), поэтому
    // проверяется то, что можно: размер домена вычислен верно (больше 64
    // бит), и на выборке далеко отстоящих друг от друга счётчиков
    // результаты не повторяются и не выходят за total_blocks.
    {
        g_cfg.range_start_bit = 0; g_cfg.range_end_bit = 200; g_cfg.chunk_bits = 10;
        compute_range();
        compute_top_nibble();

        // total_blocks256 обязан быть шире 64 бит -- то есть занимать
        // байты за пределами последних восьми (индексы 0..23 из 32).
        bool wide_enough = false;
        for (int i = 0; i < 24; i++) if (g_total_blocks256[i]) { wide_enough = true; break; }

        std::set<std::string> seen;
        std::map<std::string, uint64_t> first_seen;
        bool dup = false, over = false;
        std::mt19937_64 rng_local(1);
        // Счётчики далеко друг от друга, не только 0..N подряд -- ловит
        // ошибки, которые задели бы только маленькие или только большие x.
        for (uint64_t base : {0ULL, 10000ULL, 1000000ULL, (1ULL<<40), (1ULL<<60)}) {
            for (uint64_t d = 0; d < 2000; d++) {
                u256 k; uint64_t n; int z;
                g_cfg.mode = TRAV_KXE;
                uint64_t ctr = base + d;
                bool got = next_chunk(CI(ctr), k, n, z, rng_local);
                if (!got) continue;   // счётчик за total_blocks -- нормально для больших base
                uint8_t b[32]; u256_to_be32(b, k);
                std::string key((char*)b, 32);
                if (!seen.insert(key).second) {
                    if (!dup) std::cout << "  [DEBUG] коллизия: counter=" << ctr
                                        << " и counter=" << first_seen[key] << "\n";
                    dup = true;
                } else {
                    first_seen[key] = ctr;
                }
            }
        }
        bool ok = wide_enough && !dup && !over;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("KXE на диапазоне > 2^64 блоков", 30)
                  << (ok ? "домен расширен, повторов нет" : "") << "\n";
        if (!ok) fails++;
        g_cfg.mode = TRAV_SEQ;
    }


    // Смысл KXE в том, что это тот же обход, только в другом порядке.
    // Если множества разойдутся, «полное покрытие» станет неправдой.
    {
        g_cfg.range_start_bit = 10; g_cfg.range_end_bit = 14; g_cfg.chunk_bits = 6;
        compute_range();
        compute_top_nibble();
        std::vector<char> seq(1 << 14, 0), kxe(1 << 14, 0);
        std::mt19937_64 rng(1);
        for (uint64_t i = 0; i < g_total_chunks; i++) {
            u256 k; uint64_t n; int z;
            g_cfg.mode = TRAV_SEQ;
            if (next_chunk(CI(i), k, n, z, rng)) for (uint64_t j = 0; j < n; j++) seq[k[0]+j] = 1;
            g_cfg.mode = TRAV_KXE;
            if (next_chunk(CI(i), k, n, z, rng)) for (uint64_t j = 0; j < n; j++) kxe[k[0]+j] = 1;
        }
        bool ok = (seq == kxe);
        uint64_t cnt = 0;
        for (char c : kxe) if (c) cnt++;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("KXE == детерминированный", 30)
                  << "то же множество, " << cnt << " ключей\n";
        if (!ok) fails++;
        g_cfg.mode = TRAV_SEQ;
    }

    // ---- НИ ОДИН РЕЖИМ НЕ ВЫХОДИТ ЗА ДИАПАЗОН ----------------------------
    //
    // Проверка покрытия отвечает на вопрос «трогает ли движок лишнее внутри
    // участка». Здесь другой вопрос: не выходит ли за диапазон САМ УЧАСТОК,
    // ещё до горячего цикла. Это разные рубежи, и ловятся они разными
    // тестами — прошлый разбор закрыл первый и оставил второй.
    //
    // Конфигурации подобраны так, чтобы промах был виден. Диапазон УЖЕ
    // участка ловит случайный и зональный режимы: там длина ставилась
    // постоянной. Участок шире 2^32 ловит KXE: там остаток сравнивался
    // только по младшим 32 битам.
    {
        struct { int a, b, cb, mode; const char *name; } cs[] = {
            { 19, 20, 22, TRAV_RANDOM, "случайный"          },
            { 19, 20, 22, TRAV_ZONE,   "зональный"          },
            { 33, 37, 36, TRAV_KXE,    "KXE"                },
            { 33, 37, 36, TRAV_SEQ,    "детерминированный"  },
        };
        int saved_mode = g_cfg.mode, saved_cb = g_cfg.chunk_bits;
        bool all_ok = true;
        uint64_t worst = 0;
        const char *worst_name = "";
        for (auto &c : cs) {
            g_cfg.range_start_bit = c.a; g_cfg.range_end_bit = c.b;
            g_cfg.chunk_bits = c.cb; g_cfg.mode = c.mode; g_cfg.det_hex = -1;
            sanitize_config(g_cfg);
            compute_range();
            compute_top_nibble();
            zone_set_seed(20260813ULL);
            std::mt19937_64 rng(99);
            for (uint64_t i = 0; i < 4096; i++) {
                u256 k; uint64_t n = 0; int z;
                if (!next_chunk(CI(i), k, n, z, rng)) break;
                if (n == 0) continue;                 // блок целиком вне — законно
                u256 last; u256_add_u64(last, k, n - 1);
                bool bad = u256_cmp(k, g_range_start) < 0
                        || u256_cmp(last, g_range_end) > 0
                        || u256_cmp(last, k) < 0;     // перенос за 2^256
                if (bad) {
                    all_ok = false;
                    u256 d; u256_sub(d, last, g_range_end);
                    if (!d[3] && !d[2] && !d[1] && d[0] > worst) { worst = d[0]; worst_name = c.name; }
                }
            }
        }
        std::cout << "  [" << (all_ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("участок не выходит за диапазон", 30);
        if (all_ok) std::cout << "все 4 режима\n";
        else        std::cout << "хуже всего " << worst_name << ": лишних "
                              << worst << " ключей\n";
        if (!all_ok) fails++;
        g_cfg.mode = saved_mode; g_cfg.chunk_bits = saved_cb;
    }

    // A zone step changes exactly one digit above its aligned low block.
    // Check the actual previous/current scalars, not the walk's own masks.
    {
        const int saved_mode = g_cfg.mode;
        g_cfg.range_start_bit = 252; g_cfg.range_end_bit = 256;
        g_cfg.chunk_bits = 22; g_cfg.mode = TRAV_ZONE; g_cfg.det_hex = -1;
        g_cfg.zone_nibbles = 3;
        compute_range(); zone_set_seed(20260813ULL);
        const int N = 20000;
        long bad_step = 0, pairs = 0, values[16]{};
        uint64_t positions = 0;
        u256 previous{};
        for (int i = 0; i < N; ++i) {
            u256 key; ZoneInfo info;
            zone_block_key(CI((uint64_t)i), key, info);
            if (!info.valid || info.count != 4096 || info.skip != 3
                || (info.raw[0] & 4095) || u256_cmp(key, g_range_start) < 0) ++bad_step;
            u256 last; u256_add_u64(last, key, info.count ? info.count - 1 : 0);
            if (u256_cmp(last, g_range_end) > 0 || u256_cmp(last, key) < 0) ++bad_step;
            uint8_t current[32], prior[32];
            u256_to_be32(current, info.raw); u256_to_be32(prior, info.base);
            values[current[30] >> 4]++;
            if (i) {
                int changes = 0, changed = -1;
                for (int pos = 0; pos < 64; ++pos) {
                    const int shift = (pos & 1) ? 0 : 4;
                    if (((current[pos / 2] >> shift) & 15) != ((prior[pos / 2] >> shift) & 15)) {
                        ++changes; changed = pos;
                    }
                }
                if (changes != 1 || changed != info.pos || 63 - changed < info.skip
                    || u256_cmp(info.base, previous) != 0) ++bad_step;
                if (changed >= 0) positions |= UINT64_C(1) << (63 - changed);
                ++pairs;
            }
            u256_set(previous, info.raw);
        }
        long worst = 0;
        for (long count : values) if (count > worst) worst = count;
        const bool step_ok = bad_step == 0 && pairs == N - 1;
        const bool spread_ok = (positions >> 3) == ((UINT64_C(1) << 61) - 1)
                            && worst * 16 <= N * 3;
        std::cout << "  [" << (step_ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("зонный: одна замена выше блока", 30)
                  << pairs << " переходов, расхождений " << bad_step << "\n";
        if (!step_ok) ++fails;
        std::cout << "  [" << (spread_ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("зонный: позиции и символы меняются", 30)
                  << "61 позиция; самое частое значение " << (100.0 * worst / N) << "%\n";
        if (!spread_ok) ++fails;
        zone_set_seed(20260813ULL);
        u256 replay; ZoneInfo replay_info;
        zone_block_key(CI(N - 1), replay, replay_info);
        const bool replay_ok = replay_info.valid && u256_cmp(replay_info.raw, previous) == 0;
        std::cout << "  [" << (replay_ok ? "OK " : "FAIL") << "] зонный: повтор того же зерна и шага\n";
        if (!replay_ok) ++fails;
        g_cfg.mode = saved_mode;
    }

    // ---- зональный: ключи внутри диапазона и разряд на месте -------------
    // Покрытия здесь не бывает по замыслу, но выходить за границы диапазона
    // режим не имеет права — иначе часть работы уходит впустую.
    {
        g_cfg.range_start_bit = 39; g_cfg.range_end_bit = 40;
        g_cfg.det_hex = -1;
        compute_range();
        compute_top_nibble();
        zone_set_seed(12345);
        uint64_t out = 0, seen_zones = 0;
        std::vector<char> zones(64, 0);
        for (uint64_t i = 0; i < 5000; i++) {
            u256 k; ZoneInfo zi;
            zone_block_key(CI(i), k, zi);
            if (!zi.valid || u256_cmp(k, g_range_start) < 0 || u256_cmp(k, g_range_end) > 0) out++;
            if (zi.zone >= 0 && zi.zone < 64 && !zones[zi.zone]) { zones[zi.zone] = 1; seen_zones++; }
        }
        bool ok = (out == 0) && (g_zone_count == 0 || seen_zones > 1);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("зональный: в границах", 30)
                  << "вне диапазона " << out << ", зон задействовано " << seen_zones << "\n";
        if (!ok) fails++;
        g_cfg.det_hex = -1;
    }

    // ---- старший разряд держится ВО ВСЕХ ЧЕТЫРЁХ РЕЖИМАХ ------------------
    //
    // Прежний тест спрашивал только зональный режим -- и потому не замечал,
    // что в детерминированном и в KXE настройка не действовала вовсе.
    // Проверять надо каждый режим: разряд задаётся один раз, а обходов
    // четыре, и любой из них может пройти мимо ограничения.
    //
    // Заодно проверяется и сам порядок вызовов: det_hex обязан быть выставлен
    // ДО compute_range(), потому что теперь он сужает границы, а не правит
    // готовые ключи.
    {
        const int modes[4] = { TRAV_RANDOM, TRAV_SEQ, TRAV_ZONE, TRAV_KXE };
        const char *names[4] = { "случайный", "детерм.", "зональный", "KXE" };
        int save_mode = g_cfg.mode;
        uint64_t total_bad = 0;
        std::string worst;

        for (int m = 0; m < 4; m++) {
            g_cfg.range_start_bit = 39; g_cfg.range_end_bit = 40;
            g_cfg.chunk_bits = 12;
            g_cfg.mode = modes[m];
            g_cfg.det_hex = -1;
            compute_range();                       // чтобы узнать g_top_vmin/vmax
            g_cfg.det_hex = (g_top_vmin + g_top_vmax) / 2;
            compute_range();                       // теперь диапазон сужается

            std::mt19937_64 rng(777 + m);
            uint64_t bad = 0, checked = 0;
            for (uint64_t i = 0; i < 500; i++) {
                u256 k; uint64_t n = 0; int z = -1;
                if (!next_chunk(CI(i), k, n, z, rng)) break;
                if (n == 0) continue;              // блок вне диапазона -- не в счёт
                checked++;
                uint8_t kb[32]; u256_to_be32(kb, k);
                int byte = g_top_ni / 2;
                int got = (g_top_ni % 2 == 0) ? (kb[byte] >> 4) : (kb[byte] & 0xF);
                if (got != g_cfg.det_hex) bad++;
                // и последний ключ участка обязан остаться в том же разряде
                u256 last; u256_add_u64(last, k, n - 1);
                u256_to_be32(kb, last);
                got = (g_top_ni % 2 == 0) ? (kb[byte] >> 4) : (kb[byte] & 0xF);
                if (got != g_cfg.det_hex) bad++;
            }
            if (bad || checked == 0) {
                total_bad += bad ? bad : 1;
                if (!worst.empty()) worst += ", ";
                worst += std::string(names[m]) + ": " +
                         (checked ? std::to_string(bad) : std::string("нет участков"));
            }
        }
        bool ok = (total_bad == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("старший разряд: все 4 режима", 30)
                  << (ok ? "нарушений 0" : worst) << "\n";
        if (!ok) fails++;
        g_cfg.det_hex = -1;
        g_cfg.mode = save_mode;
        compute_range();
    }

    // границы диапазона: [2^x, 2^y) должно давать ровно 2^x .. 2^y-1
    {
        g_cfg.range_start_bit = 19; g_cfg.range_end_bit = 20; g_cfg.chunk_bits = 16;
        compute_range();
        uint8_t s[32], e[32];
        u256_to_be32(s, g_range_start);
        u256_to_be32(e, g_range_end);
        bool ok = (to_hex(s, 32) == std::string(59, '0') + "80000") &&
                  (to_hex(e, 32) == std::string(59, '0') + "fffff");
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] " << pad("диапазон [2^19, 2^20)", 30)
                  << "0x" << to_hex(s, 32).substr(59) << " .. 0x" << to_hex(e, 32).substr(59) << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  ЧЕКПОИНТ НЕ ИМЕЕТ ПРАВА ПРОПУСТИТЬ НЕЗАВЕРШЁННЫЙ УЧАСТОК
    //
    //  Разбор указал на самое опасное место: позиция бралась вычитанием числа
    //  потоков из счётчика ВЫДАЧИ. Участки выдаются по порядку, но кончаются
    //  вразнобой, и отступ на фиксированное число ничего не доказывает.
    //
    //  Воспроизводим ровно тот расклад из разбора: поток 0 забрал участок 50 и
    //  застрял, соседи закрыли 51..107. Старая схема сохранила бы 100 --
    //  ВЫШЕ незакрытого пятидесятого, и после восстановления участок 50 не
    //  достался бы никому.
    // ========================================================================
    {
        checkpoint_reset(g_search, ChunkIdx{});
        for (uint64_t i = 0; i < 50; i++) chunk_done(g_search, CI(i));      // 0..49 закрыты
        for (uint64_t i = 51; i <= 107; i++) chunk_done(g_search, CI(i));   // 51..107 закрыты
        // 50 всё ещё в работе
        uint64_t saved = CIU(checkpoint_seq(g_search));
        bool ok1 = (saved == 50);

        chunk_done(g_search, CI(50));                                       // застрявший закрылся
        uint64_t after = CIU(checkpoint_seq(g_search));
        bool ok2 = (after == 108);                            // знак прыгнул через всю цепочку

        bool ok = ok1 && ok2;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("чекпоинт: дыра невозможна", 30)
                  << "при незакрытом 50-м сохранено " << saved
                  << " (старая схема дала бы 100), после закрытия " << after << "\n";
        if (!ok) fails++;
    }

    // Тот же учёт под настоящими потоками: участки закрываются в случайном
    // порядке, и водяной знак обязан остаться не выше самого младшего
    // незакрытого в любой момент времени.
    {
        checkpoint_reset(g_search, ChunkIdx{});
        const int N = 2000;
        std::vector<uint64_t> order(N);
        for (int i = 0; i < N; i++) order[i] = (uint64_t)i;
        std::mt19937_64 rng(4242);
        std::shuffle(order.begin(), order.end(), rng);

        std::set<uint64_t> closed;
        bool violated = false;
        for (int i = 0; i < N; i++) {
            chunk_done(g_search, CI(order[i]));
            closed.insert(order[i]);
            // наименьший НЕзакрытый на этот момент
            uint64_t low = 0;
            while (closed.count(low)) low++;
            if (CIU(checkpoint_seq(g_search)) > low) { violated = true; break; }
        }
        bool ok = !violated && CIU(checkpoint_seq(g_search)) == (uint64_t)N;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("чекпоинт: закрытие вразнобой", 30)
                  << N << " участков, знак ни разу не обогнал незакрытый\n";
        if (!ok) fails++;
        checkpoint_reset(g_search, ChunkIdx{});
    }

    // ========================================================================
    //  ЗОНАЛЬНЫЙ РЕЖИМ: МЛАДШИЕ ТРИ РАЗРЯДА ДЕЙСТВИТЕЛЬНО ПЕРЕБИРАЮТСЯ
    //
    //  В журнале каждая запись зонального режима выглядит так:
    //
    //      CURR : ...a5c8[0]000
    //      ПОСЛ.: ...a5c8[0]fff
    //
    //  и хвост 000 у CURR читается как «младшие три разряда стоят на месте».
    //  На самом деле CURR -- ПЕРВЫЙ ключ блока, а блок и есть эти три
    //  разряда: 4096 значений от 000 до fff. Но поверить в это на слово
    //  нельзя, а перепутать выравнивание базы с отсутствием перебора --
    //  можно, и такая ошибка однажды здесь уже была.
    //
    //  Поэтому проверяем не глазами, а движком: кладём в базу целей ВСЕ 4096
    //  ключей блока, прогоняем блок и требуем, чтобы нашлись все до одного.
    //  Если бы младшие разряды не перебирались, нашёлся бы ровно один.
    // ========================================================================
    {
        g_quiet = true;
        g_test_found.clear();
        g_db.items.clear();
        g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;

        g_cfg.range_start_bit = 30; g_cfg.range_end_bit = 40;
        g_cfg.chunk_bits = 12; g_cfg.det_hex = -1;
        g_cfg.mode = TRAV_ZONE;
        compute_range();
        compute_top_nibble();
        zone_set_seed(777);

        u256 k0; ZoneInfo zi;
        zone_block_key(CI(5), k0, zi);              // какой-нибудь блок из середины

        // Цели: все 4096 ключей блока.
        for (uint64_t i = 0; i < 4096; i++) {
            u256 kk; u256_add_u64(kk, k0, i);
            Point pp = ec_mul_G(kk);
            uint8_t bx[32], bc[33];
            fe_to_be32(bx, pp.x);
            bc[0] = (uint8_t)(0x02 | (pp.y[0] & 1));
            memcpy(bc + 1, bx, 32);
            g_db.add_hex(to_hex(bc, 33));
        }
        g_db.build();
        if (!push_targets_to_backend()) return 1;

        scan_chunk(k0, 4096);

        uint64_t found = 0, first_missing = ~0ULL;
        for (uint64_t i = 0; i < 4096; i++) {
            u256 kk; u256_add_u64(kk, k0, i);
            uint8_t bk[32]; u256_to_be32(bk, kk);
            if (g_test_found.count(to_hex(bk, 32))) found++;
            else if (first_missing == ~0ULL) first_missing = i;
        }

        // The aligned zone block must scan all 4096 keys, including the
        // last low-HEX combination, and report their original private keys.
        bool ok = (found == 4096);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad("зонный: все ключи блока", 30)
                  << "найдено " << found << " из 4096";
        if (found != 4096) std::cout << ", первый пропуск на смещении " << first_missing;
        std::cout << "\n";
        if (!ok) fails++;

        // ОБРАЗЕЦ ДЛЯ БЛОКА «ПРОВЕРКА» ОБЯЗАН ЛЕЖАТЬ ВНУТРИ УЧАСТКА.
        //
        // Этот ключ человек берёт с экрана и сверяет сторонним инструментом,
        // убеждаясь, что программа считает верно. Если образец окажется за
        // пределами участка, проверка сойдётся -- ключ настоящий, -- но
        // покажет она не то: этот ключ программа не проверяла. Ложное
        // успокоение хуже отсутствия проверки, поэтому границы закрепляем.
        {
            u256 smp;
            bool have;
            {
                std::lock_guard<std::mutex> lk(g_mtx_sample);
                have = g_sample_valid;
                if (have) u256_set(smp, g_sample_key);
            }
            u256 hi; u256_add_u64(hi, k0, 4095);
            bool inside = have && u256_cmp(smp, k0) >= 0 && u256_cmp(smp, hi) <= 0;
            // И он НЕ должен совпадать с началом: круглый ключ ничего не
            // доказывает про остальные 4095, ради этого образец и смещают.
            bool not_start = have && u256_cmp(smp, k0) != 0;
            const uint64_t off = have ? sample_offset(k0, 4096) : 0;
            // И тот же ключ обязан получаться у журнала: он печатает строку
            // ВНУТРИ той же функцией. Разойдись они -- экран и журнал снова
            // показывали бы разные ключи одного блока.
            u256 from_log; u256_add_u64(from_log, k0, sample_offset(k0, 4096));
            bool same_as_log = have && u256_cmp(smp, from_log) == 0;

            bool ok2 = inside && not_start && same_as_log;
            std::cout << "  [" << (ok2 ? "OK " : "FAIL") << "] "
                      << pad("образец ПРОВЕРКИ внутри участка", 30)
                      << (have ? ("смещение " + std::to_string(off) + " из 4096")
                               : std::string("образца нет"))
                      << (inside ? "" : ", ЗА ПРЕДЕЛАМИ УЧАСТКА")
                      << (not_start ? "" : ", совпал с началом")
                      << (same_as_log ? ", совпадает с журналом" : ", РАСХОДИТСЯ С ЖУРНАЛОМ") << "\n";
            if (!ok2) fails++;
        }

        g_db.items.clear();
        g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
        g_test_found.clear();
        g_quiet = false;
        g_cfg.mode = TRAV_SEQ;
        compute_range();
    }

    // ========================================================================
    //  ВОЛНОВОЙ РЕЖИМ
    //
    //  Ключ считается ПРЯМО по номеру состояния, без прокрутки волны — иначе
    //  выдача участка стоила бы тысячу шагов. Формула короткая и потому
    //  соблазнительно правдоподобная: ошибку в множителе или в границе фазы
    //  глазами не увидеть, а проявится она тем, что часть узора молча
    //  пропущена.
    //
    //  Поэтому сверяем с честной прокруткой: идём по волне шаг за шагом,
    //  как рисуют от руки, и сравниваем с тем, что даёт формула.
    // ========================================================================
    {
        g_cfg.range_start_bit = 0; g_cfg.range_end_bit = 256;
        g_cfg.chunk_bits = 12; g_cfg.det_hex = -1;
        g_cfg.mode = TRAV_WAVE;
        g_cfg.wave_start = "884222fa45078bdf89a87ac693652bf51d520fa576c317425b1e52806148fb14";
        compute_range();

        // Честная прокрутка: k младших разрядов получают +1 на каждом шаге,
        // f переходит в 0 БЕЗ переноса; последний ключ фазы -- база следующей.
        uint8_t walk[32];
        {
            u256 st; hex_to_u256(g_cfg.wave_start.c_str(), st);
            u256_to_be32(walk, st);
            walk[31] = 0;
            walk[30] &= 0xf0; // blockfloor: low three digits belong to the scan
        }
        // Волна начинается с разряда 3: младшие три перебирает сам блок.
        const int SKIP = 3;
        // Разряды считаем от МЛАДШЕГО: i = SKIP -- самый младший рабочий,
        // i = 63 -- самый старший. Фронт от старшего конца занимает
        // верхние width разрядов.
        auto bump = [&](uint8_t k[32], int width, bool high, int t) {
            const int lo = high ? (64 - width) : SKIP;
            const int hi = high ? 64 : (SKIP + width);
            for (int i = lo; i < hi; i++) {
                int pos = 63 - i, byte = pos / 2;
                int cur = (pos % 2 == 0) ? (k[byte] >> 4) : (k[byte] & 0xF);
                int nv = (cur + t) & 0xF;
                if (pos % 2 == 0) k[byte] = (uint8_t)((k[byte] & 0x0F) | (nv << 4));
                else              k[byte] = (uint8_t)((k[byte] & 0xF0) | nv);
            }
        };

        // ПОРЯДОК ФАЗ СТРОИТСЯ ЗДЕСЬ ЗАНОВО, А НЕ БЕРЁТСЯ ИЗ ПРОГРАММЫ.
        //
        // В traverse_modes.cpp накопления лежат в таблице, которую строит сам
        // модуль. Сверять таблицу с собой же бессмысленно, поэтому здесь
        // последовательность (ширина, с какого конца) выписана независимо, по
        // словесному описанию челнока: 1..61 от младшего, 60..1 от младшего,
        // 1..61 от старшего, 60..1 от старшего.
        struct Ph { int w; bool high; };
        std::vector<Ph> phases;
        // Замкнутый круг В->А->В. Сужаясь, окно прижимается к ТОМУ краю, до
        // которого дошло, а не к тому, откуда вышло: иначе фронт не
        // возвращается, а пятится хвостом вперёд.
        for (int w = 1;  w <= 61; w++) phases.push_back({w, false});  // растёт от младшего
        for (int w = 60; w >= 1;  w--) phases.push_back({w, true });  // жмётся к старшему
        for (int w = 2;  w <= 61; w++) phases.push_back({w, true });  // растёт от старшего
        for (int w = 60; w >= 1;  w--) phases.push_back({w, false});  // жмётся к младшему

        uint64_t bad = 0, checked = 0;
        std::string first_bad;
        uint64_t s = 1;                       // состояние 0 -- сам старт
        for (size_t q = 0; q < phases.size(); q++) {
            const int  w    = phases[q].w;
            const bool high = phases[q].high;
            for (int t = 1; t <= 15; t++, s++) {
                uint8_t want[32]; memcpy(want, walk, 32);
                bump(want, w, high, t);

                ChunkIdx idx; idx.from_u64(s);
                u256 got; WaveInfo wi;
                wave_block_key(idx, got, wi);
                uint8_t gb[32]; u256_to_be32(gb, got);

                checked++;
                if (memcmp(gb, want, 32) != 0) {
                    if (!bad) first_bad = "фаза " + std::to_string(q + 1) +
                                          " (ширина " + std::to_string(w) +
                                          (high ? ", от старшего" : ", от младшего") +
                                          ") шаг " + std::to_string(t);
                    bad++;
                }
                if (wi.phase != (int)q + 1 || wi.step != t ||
                    wi.width != w || wi.high != high) {
                    if (!bad) first_bad = "не та фаза/ширина на s=" + std::to_string(s);
                    bad++;
                }
            }
            bump(walk, w, high, 15);          // последний ключ фазы -> база следующей
        }

        bool ok = (bad == 0) && (checked == 241 * 15);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("модель челнока = прокрутка", 30)
                  << checked << " состояний"
                  << (ok ? ", расхождений 0" : ", первое расхождение: " + first_bad) << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  ВОЛНА НЕ ПОВТОРЯЕТСЯ И ВОВРЕМЯ БЕРЁТ НОВЫЙ СТАРТ
    //
    //  Каждая фаза делает 15 шагов, а не 16: шестнадцатый вернул бы разряды
    //  туда, откуда они вышли. И когда волна дошла до самого старшего
    //  разряда и он отработал свои 15 -- дальше крутить нечего, нужен новый
    //  старт, иначе узор пойдёт по второму кругу.
    //
    //  С челноком и зеркалом ходов стало четыре, состояний на старт 3631.
    //  Проверяем оба утверждения разом: все они дают РАЗНЫЕ ключи, а
    //  состояние 3631 -- уже другой старт.
    //
    //  Повторов не должно быть и МЕЖДУ ходами: обратный ход идёт по тем же
    //  ширинам, что и прямой, и если бы накопления не уходили вперёд, фаза
    //  62 совпала бы с фазой 60. Здесь это и ловится.
    // ========================================================================
    {
        g_cfg.range_start_bit = 0; g_cfg.range_end_bit = 256;
        g_cfg.chunk_bits = 12; g_cfg.det_hex = -1;
        g_cfg.mode = TRAV_WAVE;
        g_cfg.wave_start.clear();          // случайный старт: так работает вживую
        compute_range();
        zone_set_seed(20260815);

        const uint64_t PER = 1 + 241 * 15;  // 3616
        std::set<std::string> seen;
        uint64_t dup = 0;
        int last_phase = -1;
        for (uint64_t s = 0; s < PER; s++) {
            ChunkIdx idx; idx.from_u64(s);
            u256 k; WaveInfo wi;
            wave_block_key(idx, k, wi);
            uint8_t b[32]; u256_to_be32(b, k);
            if (!seen.insert(to_hex(b, 32)).second) dup++;
            if (s + 1 == PER) last_phase = wi.phase;
        }

        // Состояние PER обязано быть НОВЫМ стартом.
        ChunkIdx i0; i0.from_u64(0);
        ChunkIdx iN; iN.from_u64(PER);
        u256 k0, kN; WaveInfo w0, wN;
        wave_block_key(i0, k0, w0);
        wave_block_key(iN, kN, wN);

        bool fresh = (wN.start_no == w0.start_no + 1) && (wN.phase == 0) &&
                     (u256_cmp(k0, kN) != 0);
        // Повторов быть НЕ ДОЛЖНО ни одного.
        //
        // Две прежние схемы их давали: 16 -- когда ход 3 доходил до полной
        // ширины и повторял ход 1, и 15 -- когда обратный ход начинался с
        // той же ширины 1, какой кончился предыдущий. Обе нашлись не
        // рассуждением, а прокруткой всех состояний. У нынешней схемы ноль,
        // и проверять надо именно ноль: любое послабление здесь спрячет
        // третью такую же.
        bool ok = (dup == 0) && (seen.size() == PER) && fresh &&
                  (last_phase == 241);

        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("модель челнока: 3616 состояний", 30)
                  << "разных ключей " << seen.size() << " из " << PER
                  << ", последняя фаза " << last_phase
                  << (fresh ? ", дальше новый старт" : ", НОВЫЙ СТАРТ НЕ ВЗЯТ") << "\n";
        if (!ok) fails++;
    }

    // Волна обязана возвращаться: 16 шагов -- и разряд там же, откуда начал.
    // Плюс проверка, что переноса в соседний разряд НЕТ: при переносе
    // возврата не случилось бы.
    {
        g_cfg.mode = TRAV_WAVE;
        g_cfg.wave_start = "00000000000000000000000000000000000000000000000000000000000000ff";
        compute_range();

        // Фаза 1, шаг 15 от старта ...ff даёт ...fe (f + 15 = e по модулю 16).
        // При переносе было бы ...0e с единицей в соседнем разряде.
        // Волна начинается с разряда 3, то есть с байта 30 (старшая половина).
        ChunkIdx i15; i15.from_u64(15);
        u256 k15; WaveInfo w15;
        wave_block_key(i15, k15, w15);
        uint8_t b[32]; u256_to_be32(b, k15);
        // Старт ...00ff: разряд 3 это второй знак от конца байта 30, он был 0.
        // 0 + 15 = f, а соседние разряды не тронуты -- значит переноса нет.
        bool no_carry = (b[30] == 0xf0) && (b[31] == 0x00);

        // Полный оборот: 16 шагов вернули бы разряд на место, но шагов в фазе
        // 15, поэтому проверяем сумму: старт + 15 + 1 = старт.
        bool ok = no_carry;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("модель челнока: f -> 0", 30)
                  << "старт ...00ff, шаг 15 дал ..." << to_hex(b + 30, 2)
                  << (no_carry ? " — соседний разряд не тронут"
                               : " — ПЕРЕНОС ПРОСОЧИЛСЯ") << "\n";
        if (!ok) fails++;

        g_cfg.wave_start.clear();
        g_cfg.mode = TRAV_SEQ;
        compute_range();
    }

    // ========================================================================
    //  ПРОИЗВОДСТВЕННЫЕ ВОЛНА/ЗОНЫ: ПОЛНОЕ ПОКРЫТИЕ И ЦЕЛЫЕ УЗОРЫ.
    //  Проверяем next_chunk/restored_shape_block, а не legacy-переходники.
    // ========================================================================
    {
        const Config saved_config = g_cfg;
        const int saved_block = g_skip_low;
        const uint64_t saved_seed = wave_get_seed();
        bool coverage_ok = true, pattern_ok = true, fixed_ok = true;
        uint64_t accepted = 0, checked_states = 0, visits = 0;
        auto total64 = [&](uint64_t& total) {
            total = 0;
            for (int i=0;i<24;++i) if(g_total_blocks256[i]) return false;
            for (int i=24;i<32;++i) total=(total<<8)|g_total_blocks256[i];
            return total>0;
        };
        auto exact_small_union = [&]() {
            uint64_t total=0;
            if(!total64(total)||total>UINT64_C(1048576))return false;
            std::vector<std::pair<uint64_t,uint64_t>> windows;
            std::mt19937_64 rng(7137);
            bool ok=traversal_full_coverage()&&mode_has_checkpoint();
            for(uint64_t i=0;i<total;++i) {
                u256 key{};uint64_t count=0;int phase=-1;
                if(!next_chunk(CI(i),key,count,phase,rng)){ok=false;break;}
                if(!count)continue;
                u256 last;u256_add_u64(last,key,count-1);
                ok=ok&&!key[1]&&!key[2]&&!key[3]&&!last[1]&&!last[2]&&!last[3]
                    &&u256_cmp(key,g_range_start)>=0&&u256_cmp(last,g_range_end)<=0
                    &&u256_cmp(last,key)>=0&&count<=4096;
                windows.emplace_back(key[0],last[0]);
            }
            std::sort(windows.begin(),windows.end());
            uint64_t next=g_range_start[0];
            for(const auto& window:windows){ok=ok&&window.first==next;next=window.second+1;}
            ok=ok&&!windows.empty()&&next==g_range_end[0]+1;
            u256 unused{};uint64_t count=9;int phase=-1;
            ok=ok&&!next_chunk(CI(total),unused,count,phase,rng)&&count==0;
            accepted+=windows.size();return ok;
        };
        for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
            g_cfg.mode=mode;g_cfg.wave_start.clear();g_cfg.mask_hex.clear();g_cfg.mask_froz=0;
            g_cfg.range_start_bit=12;g_cfg.range_end_bit=20;g_cfg.det_hex=-1;
            g_cfg.zone_nibbles=g_cfg.wave_nibbles=3;
            set_block_nibbles(3);wave_set_seed(UINT64_C(7137));compute_range();
            coverage_ok=exact_small_union()&&coverage_ok;
        }
        // Independent archive formula for each complete base visit. The
        // positions are local to each bit bucket; there is no global quantum.
        for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
            g_cfg.mode=mode;g_cfg.wave_start.clear();g_cfg.range_start_bit=139;
            g_cfg.range_end_bit=256;g_cfg.det_hex=-1;compute_range();
            uint64_t ordinal=0;
            std::set<std::string> seen;
            for(unsigned round=0;round<2;++round)for(unsigned bits=140;bits<=256;++bits) {
                const unsigned top=64U-(bits+3U)/4U;
                const unsigned dimensions=mode==TRAV_WAVE?61U-top:60U-top;
                const unsigned states=1U+15U*dimensions;
                u256 low{},high{};
                low[(bits-1U)/64U]=UINT64_C(1)<<((bits-1U)%64U);
                if(bits==256)u256_sub_u64(high,EC_N,1);
                else {high[bits/64U]=UINT64_C(1)<<(bits%64U);u256_sub_u64(high,high,1);}
                uint8_t prior_center[32]{};
                for(unsigned within=0;within<states;++within,++ordinal) {
                    u256 got{};uint64_t count=0;int phase=-1;WaveInfo wave{};ZoneInfo zone{};
                    if(!restored_shape_block(CI(ordinal),got,count,phase,
                            mode==TRAV_WAVE?&wave:nullptr,mode==TRAV_ZONE?&zone:nullptr)) {
                        pattern_ok=false;continue;
                    }
                    uint8_t center[32],want[32];
                    u256_to_be32(center,mode==TRAV_WAVE?wave.start:zone.base);
                    if(!within){memcpy(prior_center,center,32);++visits;pattern_ok=pattern_ok&&count>0;}
                    pattern_ok=pattern_ok&&std::memcmp(prior_center,center,32)==0;
                    const unsigned p=within?(within-1U)/15U+1U:0U;
                    const unsigned step=within?(within-1U)%15U+1U:0U;
                    pattern_ok=pattern_ok&&phase==(int)p;
                    memcpy(want,center,32);
                    auto digit_add=[&](unsigned position,unsigned add) {
                        const unsigned shift=position%2U?0U:4U;
                        const unsigned value=(((unsigned)want[position/2]>>shift)+add)&15U;
                        want[position/2]=(uint8_t)((want[position/2]&~(15U<<shift))|(value<<shift));
                    };
                    if(p&&mode==TRAV_ZONE)digit_add(top+p,step);
                    if(p&&mode==TRAV_WAVE)
                        for(unsigned j=0;j<p;++j)digit_add(60U-j,15U*(p-1U-j)+step);
                    u256 expected,last;fe_from_be32(expected,want);u256_add_u64(last,expected,4095);
                    uint64_t expected_count=0;
                    if(u256_cmp(expected,high)<=0&&(u256_cmp(last,low)>=0||u256_cmp(last,expected)<0)) {
                        if(u256_cmp(expected,low)<0)u256_set(expected,low);
                        if(u256_cmp(last,high)>0||u256_cmp(last,expected)<0)u256_set(last,high);
                        u256 width;u256_sub(width,last,expected);expected_count=width[0]+1;
                    }
                    pattern_ok=pattern_ok&&count==expected_count&&(!count||u256_cmp(got,expected)==0);
                    if(count){uint8_t actual[32];u256_to_be32(actual,got);pattern_ok=pattern_ok&&seen.insert(to_hex(actual,32)).second;}
                    ++checked_states;
                }
            }
        }
        // The fixed start keeps the exact archive prefix, then continues with
        // its complement. The end of 916 states is not the end of the search.
        g_cfg.mode=TRAV_WAVE;g_cfg.range_start_bit=0;g_cfg.range_end_bit=256;
        u256 anchor;u256_from_u64(anchor,UINT64_C(0x123000));uint8_t walk[32];u256_to_be32(walk,anchor);
        g_cfg.wave_start=to_hex(walk,32);compute_range();
        const ChunkIdx prefix_end=CI(916);
        fixed_ok=fixed_ok&&traversal_full_coverage()&&mode_has_checkpoint()
            &&std::memcmp(g_total_blocks256,prefix_end.b,32)>0;
        std::mt19937_64 rng(2);uint64_t ordinal=0;
        auto check_walk=[&](int expected_phase) {
            u256 expected;fe_from_be32(expected,walk);uint64_t expected_count=4096;
            if(!(expected[0]|expected[1]|expected[2]|expected[3])){u256_from_u64(expected,1);expected_count=4095;}
            u256 got{};uint64_t count=0;int phase=-1;
            const bool issued=next_chunk(CI(ordinal++),got,count,phase,rng);
            fixed_ok=fixed_ok&&issued&&count==expected_count&&phase==expected_phase&&u256_cmp(got,expected)==0;
        };
        check_walk(0);
        for(int width=1;width<=3;++width)for(int step=1;step<=15;++step) {
            for(int nibble=3;nibble<3+width;++nibble) {
                const int position=63-nibble,byte=position/2,shift=position%2?0:4;
                const unsigned value=(((unsigned)walk[byte]>>shift)+1U)&15U;
                walk[byte]=(uint8_t)((walk[byte]&~(15U<<shift))|(value<<shift));
            }
            check_walk(width);
        }
        std::set<std::string> fixed_seen;
        for(uint64_t i=0;i<916;++i) {
            u256 got{};uint64_t count=0;int phase=-1;
            if(!next_chunk(CI(i),got,count,phase,rng)){fixed_ok=false;break;}
            if(count){uint8_t bytes[32];u256_to_be32(bytes,got);fixed_ok=fixed_ok&&fixed_seen.insert(to_hex(bytes,32)).second;}
        }
        bool complement=false;
        for(uint64_t i=916;i<916+4096;++i) {
            u256 got{};uint64_t count=0;int phase=-1;
            if(!next_chunk(CI(i),got,count,phase,rng)){fixed_ok=false;break;}
            if(count){uint8_t bytes[32];u256_to_be32(bytes,got);fixed_ok=fixed_ok&&fixed_seen.insert(to_hex(bytes,32)).second;complement=true;break;}
        }
        fixed_ok=fixed_ok&&complement;
        for(uint64_t start:{UINT64_C(15),UINT64_C(0x1234)}) {
            g_cfg.range_start_bit=0;g_cfg.range_end_bit=16;u256_from_u64(anchor,start);
            u256_to_be32(walk,anchor);g_cfg.wave_start=to_hex(walk,32);compute_range();
            u256 first{};uint64_t count=0;int phase=-1;
            fixed_ok=fixed_ok&&next_chunk(CI(0),first,count,phase,rng)&&u256_cmp(first,anchor)==0;
            fixed_ok=exact_small_union()&&fixed_ok;
        }
        auto show=[&](bool ok,const char* label,const std::string& detail) {
            std::cout<<"  ["<<(ok?"OK ":"FAIL")<<"] "<<pad_utf8(label,30)<<detail<<"\n";
            if(!ok)++fails;
        };
        show(coverage_ok,"волна/зоны: полный рабочий обход",std::to_string(accepted)+" окон; точное объединение, без пропусков и повторов");
        show(pattern_ok,"волна/зоны: целые узоры по битам",std::to_string(visits)+" посещений, "+std::to_string(checked_states)+" состояний по независимой формуле");
        show(fixed_ok,"фиксированная волна и дополнение","точный старт, архивный префикс, продолжение и полное малое покрытие");
        g_cfg=saved_config;wave_set_seed(saved_seed);set_block_nibbles(saved_block);compute_range();
    }

    // ========================================================================
    //  ДВА КОНТЕКСТА НЕ МЕШАЮТ ДРУГ ДРУГУ
    //
    //  Ради этого состояние и собирали в SearchContext. Пока оно жило
    //  десятком отдельных глобальных переменных, такую проверку нельзя было
    //  написать вовсе: любой второй поиск затирал бы первый.
    //
    //  Здесь два независимых контекста ведут свой учёт участков вперемешку.
    //  Требование простое: водяной знак каждого зависит только от его
    //  собственных закрытых участков.
    // ========================================================================
    {
        SearchContext a, b;

        // В первом закрываем 0..9, во втором только 0..2 -- и чередуем,
        // чтобы промах по контексту проявился сразу.
        for (uint64_t i = 0; i < 10; i++) {
            chunk_done(a, CI(i));
            if (i < 3) chunk_done(b, CI(i));
        }
        uint64_t pa = CIU(checkpoint_seq(a));
        uint64_t pb = CIU(checkpoint_seq(b));

        // Сброс одного не должен трогать другой.
        a.reset_progress();
        uint64_t pa2 = CIU(checkpoint_seq(a));
        uint64_t pb2 = CIU(checkpoint_seq(b));

        // И счётчики ключей у них тоже свои.
        a.count.fetch_add(1000);
        b.count.fetch_add(7);

        bool ok = (pa == 10) && (pb == 3) && (pa2 == 0) && (pb2 == 3) &&
                  (a.count.load() == 1000) && (b.count.load() == 7) &&
                  (&a.range_start[0] != &b.range_start[0]);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("два контекста независимы", 30)
                  << "знаки " << pa << " и " << pb
                  << ", после сброса первого " << pa2 << " и " << pb2 << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  УЧЁТ УЧАСТКОВ ПОД НАСТОЯЩИМИ ПОТОКАМИ
    //
    //  Прежние две проверки водяного знака шли в один поток и потому
    //  проверяли только схему, а не её работу под гонкой. Настоящий путь --
    //  fetch_add на общем счётчике из N потоков и chunk_done вразнобой,
    //  причём часть участков БРОСАЕТСЯ недосчитанной (так ведёт себя
    //  остановка по Ctrl+C посреди участка).
    //
    //  Требование одно и то же на всём протяжении: сохранённая позиция не
    //  должна оказаться выше любого незакрытого участка. Проверяем это не в
    //  конце, а на каждом шаге, из отдельного наблюдателя -- ошибка гонки
    //  живёт мгновение и в итоговом состоянии не видна.
    // ========================================================================
    for (int threads : {2, 4, 8, 32}) {
        checkpoint_reset(g_search, ChunkIdx{});
        const uint64_t TOTAL = 4000;
        std::atomic<uint64_t> seq{0};
        std::atomic<bool> stop{false};
        std::mutex mtx;
        std::set<uint64_t> closed;          // что реально закрыто
        std::atomic<bool> violated{false};
        std::atomic<uint64_t> abandoned{0};

        auto worker = [&](int id) {
            std::mt19937_64 rng(1234 + id);
            for (;;) {
                uint64_t idx = seq.fetch_add(1);
                if (idx >= TOTAL) return;
                // Разброс длительностей -- то самое, из-за чего отступ на
                // число потоков и не работал.
                if ((rng() & 15) == 0) std::this_thread::sleep_for(
                        std::chrono::microseconds(200 + (rng() % 800)));
                // Каждый двадцатый участок бросаем недосчитанным.
                if ((rng() % 20) == 0) { abandoned++; continue; }
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    closed.insert(idx);
                }
                chunk_done(g_search, CI(idx));
            }
        };

        // Наблюдатель: сравнивает знак с истинным «наименьшим незакрытым».
        //
        // ОБА ЗНАЧЕНИЯ БЕРУТСЯ ПОД ОДНИМ ЗАМКОМ.
        //
        // Первая версия читала множество закрытых под замком, отпускала его и
        // только потом спрашивала позицию. За этот промежуток соседние потоки
        // успевали закрыть ещё десяток участков, знак законно уходил вперёд
        // -- и проверка объявляла нарушение на восьми и тридцати двух потоках,
        // хотя нарушения не было. Сравнивать состояния, снятые в разные
        // моменты, бессмысленно: это ловит не ошибку, а собственную гонку.
        //
        // Под общим замком порядок гарантирован: рабочий поток сначала
        // вписывает участок в closed и лишь затем зовёт chunk_done, поэтому
        // всё, что успел учесть знак, уже лежит в closed.
        std::thread watcher([&]{
            while (!stop.load()) {
                std::lock_guard<std::mutex> lk(mtx);
                uint64_t low = 0;
                while (closed.count(low)) low++;
                if (CIU(checkpoint_seq(g_search)) > low) violated = true;
                std::this_thread::yield();
            }
        });

        std::vector<std::thread> pool;
        for (int i = 0; i < threads; i++) pool.emplace_back(worker, i);
        for (auto &t : pool) t.join();
        stop = true;
        watcher.join();

        // И итоговая проверка: ниже знака не должно быть ни одного брошенного.
        uint64_t saved = CIU(checkpoint_seq(g_search));
        uint64_t holes = 0;
        for (uint64_t i = 0; i < saved; i++) if (!closed.count(i)) holes++;

        bool ok = !violated.load() && holes == 0;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("чекпоинт: " + std::to_string(threads) + " потоков, срывы", 30)
                  << "позиция " << saved << ", брошено " << abandoned.load()
                  << ", незакрытых ниже позиции " << holes
                  << (violated.load() ? ", ЗНАК ОБГОНЯЛ" : "") << "\n";
        if (!ok) fails++;
        checkpoint_reset(g_search, ChunkIdx{});
    }

    // ========================================================================
    //  У КАЖДОЙ НАСТРОЙКИ — СВОЙ ЧЕКПОИНТ
    //
    //  Позиция участка осмысленна только вместе с тем, от чего она
    //  отсчитывается: режим, границы, размер участка и старший разряд.
    //  Совпадение имён у разных настроек означало бы, что одна затирает
    //  накопленное другой, а продолжение начинается с чужого места.
    //
    //  Разряд попал в этот список последним и не сразу: пока он правил уже
    //  готовые ключи, отрезок от него не зависел. С тех пор как он сужает
    //  границы, настройки с разными разрядами — это разные отрезки.
    // ========================================================================
    {
        // Four finite modes persist checkpoints. Random and hybrid remain
        // disabled; shape mapping has a separate versioned namespace.
        struct Cfg { int mode, a, b, chunk, det; const char *wave;
                     const char *mask; int froz; };
        const Cfg cfgs[] = {
            { TRAV_SEQ, 39, 40, 22, -1, "", "", 0 },
            { TRAV_KXE, 39, 40, 22, -1, "", "", 0 },   // другой режим
            { TRAV_SEQ, 38, 40, 22, -1, "", "", 0 },   // другое начало
            { TRAV_SEQ, 39, 41, 22, -1, "", "", 0 },   // другой конец
            { TRAV_SEQ, 39, 40, 20, -1, "", "", 0 },   // другой размер участка
            { TRAV_SEQ, 39, 40, 22,  8, "", "", 0 },   // тот же диапазон, разряд 8
            { TRAV_SEQ, 39, 40, 22,  9, "", "", 0 },   // тот же диапазон, разряд 9
            { TRAV_WAVE, 39, 40, 22, -1, "", "", 0 },  // волна от случайных точек
            { TRAV_WAVE, 39, 40, 22, -1,
              "0000000000000000000000000000000000000000000000000000008000000000", "", 0 },
            { TRAV_WAVE, 39, 40, 22, -1,        // ДРУГАЯ гипотеза, всё прочее то же
              "000000000000000000000000000000000000000000000000000000ff00000000", "", 0 },
            { TRAV_WAVE, 39, 41, 22, -1,        // та же гипотеза, другой диапазон
              "0000000000000000000000000000000000000000000000000000008000000000", "", 0 },
            { TRAV_ZONE, 39, 40, 22, -1, "", "", 0 },
            { TRAV_HYBRID, 39, 40, 22, -1, "", "", 0 },
            { TRAV_RANDOM, 39, 40, 22, -1, "", "", 0 },
            // МАСКА -- тоже «от чего считаем»: две гипотезы о хвосте ключа
            // при одном режиме, интервале и участке обязаны разойтись файлами.
            { TRAV_SEQ, 0, 256, 22, -1, "",
              "c69fa3a59299e3253d2b081a735c0f79d38b766a44fe36ed91f0d1f56905bd9b", 40 },
            { TRAV_SEQ, 0, 256, 22, -1, "",     // ДРУГОЙ ключ, всё прочее то же
              "c69fa3a59299e3253d2b081a735c0f79d38b766a44fe36ed91f0d1f56905bd9c", 40 },
            { TRAV_SEQ, 0, 256, 22, -1, "",     // тот же ключ, другая граница маски
              "c69fa3a59299e3253d2b081a735c0f79d38b766a44fe36ed91f0d1f56905bd9b", 39 },
        };
        const int N = (int)(sizeof(cfgs) / sizeof(cfgs[0]));
        int save_mode = g_cfg.mode;
        std::string save_wave = g_cfg.wave_start;
        std::string save_mask = g_cfg.mask_hex; int save_froz = g_cfg.mask_froz;
        std::set<std::string> names;
        std::string clash;
        int enabled = 0, disabled = 0;
        bool policy_ok = true;
        for (int i = 0; i < N; i++) {
            g_cfg.mode = cfgs[i].mode;
            g_cfg.range_start_bit = cfgs[i].a;
            g_cfg.range_end_bit   = cfgs[i].b;
            g_cfg.chunk_bits      = cfgs[i].chunk;
            g_cfg.det_hex         = cfgs[i].det;
            g_cfg.wave_start      = cfgs[i].wave;
            g_cfg.mask_hex        = cfgs[i].mask;
            g_cfg.mask_froz       = cfgs[i].froz;
            compute_range();
            std::string p = checkpoint_path();
            if (cfgs[i].mode == TRAV_RANDOM || cfgs[i].mode == TRAV_HYBRID) {
                ++disabled;
                policy_ok = policy_ok && !mode_has_checkpoint() && p.empty();
                continue;
            }
            ++enabled;
            policy_ok = policy_ok && mode_has_checkpoint() && !p.empty();
            if (!names.insert(p).second && clash.empty()) clash = p;
        }
        g_cfg.wave_start = save_wave;
        g_cfg.mask_hex = save_mask; g_cfg.mask_froz = save_froz;
        bool ok = policy_ok && clash.empty() && (int)names.size() == enabled && enabled + disabled == N;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("чекпоинт: у каждой настройки свой", 30)
                  << (ok ? std::to_string(enabled) + " настроек SEQ/KXE -> разные файлы; "
                           + std::to_string(disabled) + " без чекпоинта"
                         : !policy_ok ? "нарушена политика чекпоинтов" : "СОВПАЛИ на " + clash) << "\n";
        if (!ok) fails++;
        g_cfg.mode = save_mode;
        g_cfg.det_hex = -1;
        compute_range();
    }

    // ========================================================================
    //  DISABLED MODES: NO CHECKPOINT I/O OR IMPLICIT SEED RESTORATION.
    //  Seed determinism is checked separately through the explicit setter.
    // ========================================================================
    {
        const Config saved_config = g_cfg;
        const int saved_block = g_skip_low;
        const uint64_t save_seed = wave_get_seed();
        g_cfg.mode = TRAV_WAVE; g_cfg.wave_start.clear();
        g_cfg.wave_nibbles=4;
        g_cfg.range_start_bit = 39; g_cfg.range_end_bit = 40;
        set_block_nibbles(g_cfg.wave_nibbles);compute_range();compute_total_blocks256();

        auto keys_now = [&]() {
            std::vector<std::string> v;
            std::mt19937_64 rng(1);
            for (uint64_t i = 900; i < 920; i++) {
                ChunkIdx c; c.from_u64(i);
                u256 k; uint64_t count=0;int zone=-1;
                if(!next_chunk(c,k,count,zone,rng))return std::vector<std::string>{};
                uint8_t b[32]; u256_to_be32(b, k); v.push_back(to_hex(b, 32));
            }
            return v;
        };

        wave_set_seed(0x1234567890ABCDEFULL);
        const auto before=keys_now();wave_set_seed(0xFEDCBA0987654321ULL);
        const auto other=keys_now();wave_set_seed(0x1234567890ABCDEFULL);
        const auto after=keys_now();
        const bool deterministic=before.size()==20&&before==after&&before!=other;
        std::cout << "  [" << (deterministic ? "OK " : "FAIL") << "] "
                  << pad_utf8("волновой: явное зерно воспроизводимо",30)
                  << (deterministic?"20/20 блоков совпали; другое зерно меняет порядок":"ошибка воспроизведения")<<"\n";
        if(!deterministic)++fails;
        using Files=std::map<std::string,std::pair<uintmax_t,std::filesystem::file_time_type>>;
        auto checkpoint_files=[](){Files files;
            for(const auto& item:std::filesystem::directory_iterator(std::filesystem::current_path())) {
                const auto name=item.path().filename().u8string();
                if(item.is_regular_file()&&name.rfind("checkpoint",0)==0)
                    files.emplace(name,std::make_pair(item.file_size(),item.last_write_time()));
            }return files;
        };
        bool policy_ok=true;
        for(int mode:{TRAV_WAVE,TRAV_ZONE}) {
            g_cfg.mode=mode;
            const std::string prefix=std::string("checkpoint_")+(mode==TRAV_WAVE?"wave":"zone")+"_balanced_v1_";
            policy_ok=policy_ok&&mode_has_checkpoint()&&checkpoint_path().rfind(prefix,0)==0;
        }
        for(int mode:{TRAV_HYBRID,TRAV_RANDOM}) {
            g_cfg.mode=mode;const auto files_before=checkpoint_files();
            const auto seed_before=wave_get_seed();ChunkIdx got;got.from_u64(907);const auto unchanged=got;
            save_checkpoint();const bool read_ok=load_checkpoint(got);
            policy_ok=policy_ok&&!mode_has_checkpoint()&&checkpoint_path().empty()&&!read_ok
                &&got==unchanged&&wave_get_seed()==seed_before&&checkpoint_files()==files_before;
        }
        std::cout<<"  ["<<(policy_ok?"OK ":"FAIL")<<"] "
                 <<pad_utf8("чекпоинты: четыре конечных режима",30)
                 <<(policy_ok?"волна/зоны: новый формат; гибрид/rnd: без файлов":"нарушена политика")<<"\n";
        if(!policy_ok)++fails;
        wave_set_seed(save_seed);
        g_cfg=saved_config;set_block_nibbles(saved_block);compute_range();compute_total_blocks256();
    }

    // ========================================================================
    // Confirmed shaped progress counts only the consecutive completed prefix.
    // This uses a local context and creates no checkpoint or result files.
    {
        SearchContext local;
        u256 zero{};ChunkIdx position;
        checkpoint_reset(local,CI(0),zero);
        chunk_done(local,CI(1),UINT64_C(7));
        u256 checked{};
        bool ok=checkpoint_snapshot(local,position,checked)&&CIU(position)==0&&u256_cmp(checked,zero)==0;
        chunk_done(local,CI(0),UINT64_C(5));
        ok=ok&&checkpoint_snapshot(local,position,checked)&&CIU(position)==2
            &&checked[0]==12&&!checked[1]&&!checked[2]&&!checked[3];
        chunk_done(local,CI(2),UINT64_C(0));
        chunk_done(local,CI(0),UINT64_C(5));
        ok=ok&&checkpoint_snapshot(local,position,checked)&&CIU(position)==3&&checked[0]==12;
        checkpoint_reset(local,CI(0),zero);
        chunk_done(local,CI(0),UINT64_MAX);
        chunk_done(local,CI(1),UINT64_MAX);
        ok=ok&&checkpoint_snapshot(local,position,checked)&&CIU(position)==2
            &&checked[0]==UINT64_MAX-1&&checked[1]==1&&!checked[2]&&!checked[3];
        std::cout<<"  ["<<(ok?"OK ":"FAIL")<<"] "
                 <<pad_utf8("чекпоинт: подтверждённый вес префикса",30)
                 <<(ok?"дыра, пустой, повтор и сумма больше 2^64":"ошибка позиции/счётчика")<<"\n";
        if(!ok)++fails;

        WaveGpuCursor cursor;WaveGpuBatch batch;bool claimed=false;
        auto next=[&](ChunkIdx& id,u256 key,uint64_t& count){
            if(claimed)return false;claimed=true;id=CI(7);u256_from_u64(key,32);count=10;return true;
        };
        cursor.fill(batch,1,4,next);
        bool weights=batch.completed.empty()&&batch.completed_counts.empty();
        cursor.fill(batch,1,4,next);
        weights=weights&&batch.completed.empty()&&batch.completed_counts.empty();
        cursor.fill(batch,1,4,next);
        weights=weights&&batch.completed.size()==1&&batch.completed_counts.size()==1
            &&batch.completed[0]==CI(7)&&batch.completed_counts[0]==10;
        std::cout<<"  ["<<(weights?"OK ":"FAIL")<<"] "
                 <<pad_utf8("чекпоинт: большой блок завершён один раз",30)
                 <<(weights?"вес только после последней части":"преждевременный вес")<<"\n";
        if(!weights)++fails;
    }

    // ========================================================================
    //  ВОЛНА: НОМЕР СТАРТА НЕ УСЕКАЕТСЯ
    //
    //  Номер участка делится на 916; частное -- это номер старта, и оно
    //  256-битное. Раньше от него брались младшие 64 бита, то есть два
    //  участка, отличающиеся ровно на 916*2^64, получали ОДИН И ТОТ ЖЕ старт
    //  и дальше повторяли друг друга состояние в состояние.
    //
    //  Дотуда не дойти: это 2^85.8 ключей. Но «недостижимо» -- довод о том,
    //  сколько ошибка проживёт незамеченной, а не о том, ошибка ли это;
    //  правильный вариант здесь стоит ровно столько же.
    // ========================================================================
    {
        int save_mode = g_cfg.mode; std::string save_wave = g_cfg.wave_start;
        g_cfg.mode = TRAV_WAVE; g_cfg.wave_start.clear();
        g_cfg.range_start_bit = 0; g_cfg.range_end_bit = 256; compute_range();

        ChunkIdx a, b;
        a.from_hex_str("0000000000000000000000000000000000000000000000000000000000000000");   // старт 0
        b.from_hex_str("0000000000000000000000000000000000000000000003940000000000000000");   // старт 2^64 -- младшие 64 бита те же
        u256 ka, kb; WaveInfo wa, wb;
        wave_block_key(a, ka, wa);
        wave_block_key(b, kb, wb);
        const bool ok = (u256_cmp(ka, kb) != 0);
        uint8_t ba[32]; u256_to_be32(ba, ka);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("волновой: номер старта не усечён", 30)
                  << (ok ? "участки 0 и 916*2^64 дали разные старты"
                         : "СОВПАЛИ: частное усечено до 64 бит")
                  << "\n";
        if (!ok) fails++;
        g_cfg.mode = save_mode; g_cfg.wave_start = save_wave; compute_range();
    }

    // ========================================================================
    //  МАСКА: НАРЕЗКА УЧАСТКОВ = ЧЕСТНЫЙ ПЕРЕБОР РЕШЁТКИ
    //
    //  Участок при маске -- это 2^chunk КАНДИДАТОВ, а не отрезок длиной
    //  2^chunk ключей. Если нарезать в ключах, участки лягут на пустоту между
    //  узлами решётки: программа отчитается о пройденных участках, не
    //  проверив почти ничего.
    //
    //  Сверяем поштучно со списком, построенным напрямую: K + j*16^N.
    // ========================================================================
    {
        const int  sm = g_cfg.mode, sf = g_cfg.mask_froz, sc = g_cfg.chunk_bits;
        const int  sa = g_cfg.range_start_bit, sb2 = g_cfg.range_end_bit;
        const std::string smh = g_cfg.mask_hex;

        g_cfg.mode = TRAV_SEQ;
        g_cfg.mask_hex  = std::string(62, '0') + "5a";   // хвост 0x5a
        g_cfg.mask_froz = 2;                              // шаг 16^2 = 256
        g_cfg.chunk_bits = 10;                            // 1024 кандидата/участок
        g_cfg.range_start_bit = 0; g_cfg.range_end_bit = 20;
        compute_range();

        // Эталон: все ключи с хвостом 5a внутри [1, 2^20)
        std::vector<std::string> want;
        for (uint64_t v = 0x5a; v < (1ULL << 20); v += 256) {
            u256 k; u256_from_u64(k, v);
            uint8_t b[32]; u256_to_be32(b, k); want.push_back(to_hex(b, 32));
        }

        std::vector<std::string> got;
        bool walked = true;
        for (uint64_t ci = 0; ci < 64; ci++) {
            ChunkIdx idx; idx.from_u64(ci);
            u256 st; uint64_t cnt = 0;
            if (!chunk_bounds(idx, st, cnt)) break;       // участки кончились
            for (uint64_t t = 0; t < cnt; t++) {
                u256 k, off;
                { u128 c = 0;
                  for (int q = 0; q < 4; q++) {
                      u128 v = (u128)g_mask_step[q] * t + c;
                      off[q] = (uint64_t)v; c = v >> 64; } }
                u256_add(k, st, off);
                uint8_t b[32]; u256_to_be32(b, k); got.push_back(to_hex(b, 32));
            }
            if (got.size() > want.size() + 16) { walked = false; break; }
        }

        const bool ok = walked && (got == want);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("маска: нарезка = честный перебор", 30)
                  << (ok ? std::to_string(want.size()) + " кандидатов, порядок совпал"
                         : (got.size() == want.size()
                            ? [&]{ size_t d = 0;
                                   while (d < got.size() && got[d] == want[d]) d++;
                                   return "число совпало, КЛЮЧИ разошлись с "
                                          + std::to_string(d) + "-го: ждали ..."
                                          + (d < want.size() ? want[d].substr(48) : std::string("-"))
                                          + ", вышло ..."
                                          + (d < got.size() ? got[d].substr(48) : std::string("-")); }()
                            : "ждали " + std::to_string(want.size())
                              + " кандидатов, получили " + std::to_string(got.size())))
                  << "\n";
        if (!ok) fails++;

        // ====================================================================
        //  МАСКА: КРАЯ ПЕРЕСЕЧЕНИЯ С ИНТЕРВАЛОМ
        //
        //  Маска и границы задаются порознь и легко расходятся. Здесь три
        //  случая, каждый из которых раньше было нечем отличить от рабочего:
        //  ключ ниже дна, ключ выше потолка, весь ключ заморожен.
        // ====================================================================
        struct Edge { const char *name; int a, b; std::string key; int froz; uint64_t want; };
        const std::string lowk  = std::string(62, '0') + "5a";                 // 0x5a
        const std::string highk = "f" + std::string(63, '0');                  // ~2^255
        const Edge edges[] = {
            // ключ ниже дна: первый кандидат обязан подняться внутрь
            { "ключ ниже дна",   8, 12, lowk,  2, (4096 - 256) / 256 + 0 },
            // ключ выше потолка: пусто
            { "ключ выше верха", 0, 20, highk, 2, 0 },
            // заморожен весь ключ: ровно один кандидат
            { "заморожен весь",  0, 20, lowk, 64, 1 },
        };
        bool edge_ok = true; std::string edge_bad;
        for (const Edge &e : edges) {
            g_cfg.mask_hex = e.key; g_cfg.mask_froz = e.froz;
            g_cfg.range_start_bit = e.a; g_cfg.range_end_bit = e.b;
            compute_range();
            uint64_t n = (g_mask_total[1] | g_mask_total[2] | g_mask_total[3])
                         ? ~0ULL : g_mask_total[0];
            if (n != e.want) {
                edge_ok = false;
                edge_bad = std::string(e.name) + ": ждали " + std::to_string(e.want)
                         + ", вышло " + std::to_string(n);
                break;
            }
            // Первый кандидат обязан лежать в интервале и на решётке.
            if (n) {
                if (u256_cmp(g_mask_first, g_range_start) < 0 ||
                    u256_cmp(g_mask_first, g_range_end) > 0) {
                    edge_ok = false; edge_bad = std::string(e.name) + ": первый вне интервала";
                    break;
                }
            }
        }
        std::cout << "  [" << (edge_ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("маска: края пересечения", 30)
                  << (edge_ok ? "ниже дна / выше верха / весь заморожен — все верны"
                              : edge_bad)
                  << "\n";
        if (!edge_ok) fails++;

        g_cfg.mode = sm; g_cfg.mask_hex = smh; g_cfg.mask_froz = sf;
        g_cfg.chunk_bits = sc;
        g_cfg.range_start_bit = sa; g_cfg.range_end_bit = sb2;
        compute_range();
    }



    // ========================================================================
    //  ХЕШИ НА ГРАНИЦАХ БЛОКА
    //
    //  SHA-256 и RIPEMD-160 обрабатывают по 64 байта, и дополнение сообщения
    //  переезжает в новый блок ровно на длинах 56 и 120. Это классическое
    //  место ошибок, и случайные длины его почти не задевают: попасть в 55 или
    //  56 из тысячи возможных -- дело случая.
    //
    //  Эталоны посчитаны сторонним средством (hashlib), а не этой программой.
    //
    //  Оговорка, стоящая упоминания: когда я подбирал эти значения впервые,
    //  одно вписал по памяти и получил «ошибку» там, где её не было. Правой
    //  оказалась программа. Поэтому все числа ниже -- из независимого счёта.
    // ========================================================================
    {
        struct V { int len; const char *want; };
        const V sha[] = {
            {    0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
            {    1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb" },
            {   55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
            {   56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
            {   63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
            {   64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
            {   65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
            {  119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb" },
            {  120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c" },
            {  127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a" },
            {  128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
        };
        int bad = 0; std::string first;
        for (const V &v : sha) {
            std::vector<uint8_t> m((size_t)v.len, 'a');
            uint8_t out[32];
            sha256(m.data(), m.size(), out);
            if (to_hex(out, 32) != v.want) {
                if (!bad) first = "длина " + std::to_string(v.len);
                bad++;
            }
        }
        const bool ok = (bad == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad("SHA-256 на границах блока", 30)
                  << (ok ? std::to_string(sizeof(sha)/sizeof(sha[0])) + " длин, включая 55/56 и 119/120"
                         : std::to_string(bad) + " расхождений, первое: " + first)
                  << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  ТОЖДЕСТВА КРИВОЙ НА ВЫРОЖДЕННЫХ СЛУЧАЯХ
    //
    //  Бесконечность, удвоение, порядок группы -- места, где сложение точек
    //  ведёт себя иначе, чем в общем случае. Случайные точки туда не попадают
    //  никогда: вероятность выбрать P и -P наугад равна нулю.
    // ========================================================================
    {
        Point G = ec_G();
        Point inf; inf.inf = true;

        auto eqp = [](const Point &a, const Point &b) {
            if (a.inf || b.inf) return a.inf && b.inf;
            return fe_eq(a.x, b.x) && fe_eq(a.y, b.y);
        };

        int bad = 0; std::string first;
        auto want = [&](bool cond, const char *what) {
            if (!cond) { if (!bad) first = what; bad++; }
        };

        { Point n = G; fe_neg(n.y, G.y); want(ec_add(G, n).inf, "P + (-P) != бесконечность"); }
        want(eqp(ec_add(G, inf), G), "P + бесконечность != P");
        want(eqp(ec_add(inf, G), G), "бесконечность + P != P");
        { u256 k; u256_from_u64(k, 2); want(eqp(ec_add(G, G), ec_mul_G(k)), "G+G != 2G"); }
        { u256 k; u256_from_u64(k, 1); want(eqp(ec_mul_G(k), G), "1*G != G"); }
        { u256 k, one; u256_from_u64(one, 1); u256_sub(k, EC_N, one);
          Point ng = G; fe_neg(ng.y, G.y);
          want(eqp(ec_mul_G(k), ng), "(n-1)*G != -G"); }
        { u256 k; memcpy(k, EC_N, 32); want(ec_mul_G(k).inf, "n*G != бесконечность"); }
        { u256 k; u256_zero(k);        want(ec_mul_G(k).inf, "0*G != бесконечность"); }

        // Сложение обязано быть согласовано с умножением: (a+b)*G == a*G + b*G.
        // Это ловит ошибку в любой из двух веток сразу.
        {
            std::mt19937_64 rng(20260821);
            for (int t = 0; t < 100; t++) {
                u256 a, b, s;
                for (int i = 0; i < 4; i++) { a[i] = rng(); b[i] = rng(); }
                a[3] &= 0xFFFF; b[3] &= 0xFFFF;          // заведомо меньше n
                u256_add(s, a, b);
                if (!eqp(ec_mul_G(s), ec_add(ec_mul_G(a), ec_mul_G(b))))
                    want(false, "(a+b)*G != a*G + b*G");
            }
        }

        const bool ok = (bad == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad("кривая: вырожденные случаи", 30)
                  << (ok ? "бесконечность, удвоение, n*G, 100 пар"
                         : std::to_string(bad) + " нарушений, первое: " + first)
                  << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  KXE: ВЗАИМНАЯ ОДНОЗНАЧНОСТЬ НА КРАЕВЫХ РАЗМЕРАХ
    //
    //  Весь смысл KXE в том, что каждый блок достаётся ровно один раз. Держится
    //  это на обратимости перестановки, а она -- на прокрутке (cycle-walking),
    //  которая ломается на вырожденных размерах: 1, 2, степени двойки и их
    //  соседи. Обычный тест берёт один большой размер и этих случаев не видит.
    // ========================================================================
    {
        const uint64_t sizes[] = { 1,2,3,4,5,7,8,9,15,16,17,31,32,33,
                                   63,64,65,127,128,129,255,256,257,
                                   1023,1024,1025,4095,4096,4097 };
        int bad = 0; std::string first;
        for (uint64_t n : sizes) {
            for (uint64_t seed : { 1ULL, 12345ULL, 0xDEADBEEFULL }) {
                std::set<uint64_t> seen;
                bool out_of_range = false;
                for (uint64_t i = 0; i < n; i++) {
                    const uint64_t v = kxe_permute(i, n, seed);
                    if (v >= n) { out_of_range = true; break; }
                    seen.insert(v);
                }
                if (out_of_range || seen.size() != n) {
                    if (!bad) first = "N=" + std::to_string(n) +
                                      (out_of_range ? ": значение вне области"
                                                    : ": есть повторы");
                    bad++;
                }
            }
        }
        const bool ok = (bad == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad("KXE: биекция на краевых размерах", 30)
                  << (ok ? std::to_string(sizeof(sizes)/sizeof(sizes[0])) +
                           " размеров x 3 зерна, повторов нет"
                         : std::to_string(bad) + " нарушений, первое: " + first)
                  << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  ПОЛЕ НА КРАЯХ, А НЕ ТОЛЬКО НА СЛУЧАЙНЫХ ЗНАЧЕНИЯХ
    //
    //  Тест «своё поле против libsecp256k1» брал случайные числа -- и пропустил
    //  настоящую ошибку: fe_mul терял перенос за 2^128 и давал неверный
    //  результат для операндов, близких к p. У самого верха поля неверными были
    //  ВСЕ произведения, при двух старших лимбах из единиц -- половина, а на
    //  случайных значениях -- ни одного. Случайная выборка такую область не
    //  находит: попасть в неё случайно означает угадать 64 бита.
    //
    //  Нашлось при сверке с реализацией для видеокарты, где то же умножение
    //  написано заново, и подтверждено независимым счётом.
    //
    //  Проверяем тождествами, верными для любого поля: их можно утверждать без
    //  второй реализации, и они не зависят от того, чем считали.
    // ========================================================================
    {
        const fe edges[] = {
            {0, 0, 0, 0},                                        // ноль
            {1, 0, 0, 0},                                        // единица
            {0xFFFFFFFEFFFFFC2EULL, ~0ULL, ~0ULL, ~0ULL},        // p-1
            {0xFFFFFFFEFFFFFC2DULL, ~0ULL, ~0ULL, ~0ULL},        // p-2
            {0x1000003D0ULL, 0, 0, 0},                           // 2^32+976
            {0x1000003D1ULL, 0, 0, 0},                           // 2^32+977
            {0, 0, 0, 0x8000000000000000ULL},                    // 2^255
            {~0ULL, ~0ULL, ~0ULL, 0x7FFFFFFFFFFFFFFFULL},        // 2^255-1
            {~0ULL, ~0ULL, 0, 0},                                // 2^128-1
        };
        const int NE = (int)(sizeof(edges) / sizeof(edges[0]));
        int bad = 0;
        std::string first;

        for (int i = 0; i < NE; i++) {
            // (p-1)*x == -x, то есть (p-1)*x + x == 0.
            fe t, s;
            fe_mul(t, edges[2], edges[i]);
            fe_add(s, t, edges[i]);
            if (!fe_is_zero(s) && !bad) { first = "(p-1)*e" + std::to_string(i) + " != -e"; }
            if (!fe_is_zero(s)) bad++;

            // x*1 == x
            fe_mul(t, edges[i], edges[1]);
            if (!fe_eq(t, edges[i])) { if (!bad) first = "e" + std::to_string(i) + "*1 != e"; bad++; }

            // x*x == sqr(x): две разные схемы обязаны сойтись
            fe m, q;
            fe_mul(m, edges[i], edges[i]);
            fe_sqr(q, edges[i]);
            if (!fe_eq(m, q)) { if (!bad) first = "e" + std::to_string(i) + ": mul != sqr"; bad++; }

            // x != 0  =>  x * x^-1 == 1
            if (!fe_is_zero(edges[i])) {
                fe inv, pr, one; fe_one(one);
                fe_inv(inv, edges[i]);
                fe_mul(pr, edges[i], inv);
                if (!fe_eq(pr, one)) { if (!bad) first = "e" + std::to_string(i) + ": x*x^-1 != 1"; bad++; }
            }
        }

        // И все пары на распределительный закон: a*(b+c) == a*b + a*c.
        for (int i = 0; i < NE; i++)
            for (int j = 0; j < NE; j++)
                for (int k = 0; k < NE; k++) {
                    fe bc, l, ab, ac, rr;
                    fe_add(bc, edges[j], edges[k]);
                    fe_mul(l, edges[i], bc);
                    fe_mul(ab, edges[i], edges[j]);
                    fe_mul(ac, edges[i], edges[k]);
                    fe_add(rr, ab, ac);
                    if (!fe_eq(l, rr)) {
                        if (!bad) first = "распределительный на e" + std::to_string(i)
                                        + ",e" + std::to_string(j) + ",e" + std::to_string(k);
                        bad++;
                    }
                }

        const bool ok = (bad == 0);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad("поле на краях (не случайных)", 30)
                  << (ok ? std::to_string(NE) + " краёв, тождества сошлись"
                         : std::to_string(bad) + " нарушений, первое: " + first)
                  << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  ЦЕПОЧКА ПРИВАТНЫЙ -> secp256k1 -> ПУБЛИЧНЫЙ -> БАЗА ВО ВСЕХ ПЯТИ РЕЖИМАХ
    //
    //  Режим решает только ОДНО: какие ключи брать и в каком порядке. Дальше
    //  все пять идут в один и тот же scan_chunk, где и происходит вся суть --
    //  k*G, сравнение X с базой, подтверждение. Так устроено в коде, но
    //  «так устроено» -- не проверка: ровно этим рассуждением легко
    //  пропустить, что какой-то режим отдаёт ключ, до которого перебор не
    //  доходит, или отдаёт длину участка, не отвечающую этому ключу.
    //
    //  Поэтому здесь для КАЖДОГО режима берётся тот самый участок, который он
    //  выдаёт первым, из него выбирается ключ, его публичный ключ кладётся в
    //  базу -- и проверяется, что поиск находит его и называет ИСХОДНЫЙ
    //  приватный ключ. Совпасть случайно тут нечему.
    // ========================================================================
    {
        const int  sm = g_cfg.mode, sc = g_cfg.chunk_bits;
        const int  sa = g_cfg.range_start_bit, sb2 = g_cfg.range_end_bit;
        const std::string sw = g_cfg.wave_start;
        const bool sq = g_quiet;
        g_quiet = true;

        struct M { int mode; const char *name; };
        const M modes[] = {
            { TRAV_SEQ,    "детерминированный" },
            { TRAV_KXE,    "KXE"               },
            { TRAV_RANDOM, "случайный"         },
            { TRAV_ZONE,   "зональный"         },
            { TRAV_WAVE,   "волновой"          },
        };

        int done = 0;
        std::string bad;
        for (const M &m : modes) {
            g_cfg.mode = m.mode;
            g_cfg.chunk_bits = 12;               // мелкий участок: тест быстрый
            g_cfg.range_start_bit = 20; g_cfg.range_end_bit = 24;
            g_cfg.wave_start.clear();
            compute_range();
            // Зёрна фиксируем: иначе зональный и волновой не воспроизводятся,
            // и «первый участок» окажется каждый раз другим.
            zone_set_seed(777000777ULL);
            wave_set_seed(555000555ULL);

            // Тот самый участок, который режим выдаёт первым.
            ChunkIdx idx; idx.from_u64(0);
            u256 k0; uint64_t cnt = 0; int zone = -1;
            std::mt19937_64 rng(42);
            bool issued = next_chunk(idx, k0, cnt, zone, rng);
            // Sparse ownership can skip an out-of-range ordinal. Use the first
            // actual shape block, while keeping every other mode's old fixture.
            if (unique_blocks_mode()) {
                for (uint64_t candidate = 1; issued && cnt == 0 && candidate < 65536; ++candidate)
                    issued = next_chunk(CI(candidate), k0, cnt, zone, rng);
            }
            if (!issued || cnt == 0) {
                bad = std::string(m.name) + ": режим не выдал участок";
                break;
            }

            // ДЛИНУ УЧАСТКА СВЕРЯЕМ, А НЕ ПРИНИМАЕМ НА ВЕРУ.
            //
            // Первая версия теста брала цель как k0 + cnt/2, то есть выводила
            // её из того самого числа, которое проверяет. Мутация «участок
            // волны вдвое короче» прошла тест насквозь: цель послушно
            // переехала в середину укороченного участка и нашлась.
            //
            // Здесь длина сверяется с chunk_keys() -- той величиной, на
            // которую опирается показ чекпоинта. Разойдись эти два места --
            // и получится ровно та ошибка, из-за которой «Ключей» в меню
            // врало в 1024 раза.
            {
                u256 left; u256_sub(left, g_range_end, k0);
                uint64_t want_cnt = chunk_keys();
                if (left[3] == 0 && left[2] == 0 && left[1] == 0 &&
                    left[0] != ~0ULL && left[0] + 1 < want_cnt)
                    want_cnt = left[0] + 1;
                if (cnt != want_cnt) {
                    bad = std::string(m.name) + ": длина участка " + std::to_string(cnt)
                        + ", а chunk_keys() обещает " + std::to_string(want_cnt);
                    break;
                }
            }

            // Цель -- ПОСЛЕДНИЙ ключ участка. Начало у многих режимов
            // выровнено, и совпадение на нём доказывало бы меньше; а конец
            // ловит ещё и ошибку на единицу в длине.
            u256 target; u256_add_u64(target, k0, cnt - 1);

            Point tp = ec_mul_G(target);
            if (tp.inf) { bad = std::string(m.name) + ": k*G дало бесконечность"; break; }
            uint8_t xb[32], c33[33];
            fe_to_be32(xb, tp.x);
            c33[0] = (uint8_t)(0x02 | (tp.y[0] & 1));
            memcpy(c33 + 1, xb, 32);

            g_db.items.clear();
            g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
            g_db.add_hex(to_hex(c33, 33));
            g_db.build();
            if (!push_targets_to_backend()) return 1;

            uint8_t tb[32]; u256_to_be32(tb, target);
            const std::string want = to_hex(tb, 32);

            g_last_found.clear();
            scan_chunk(k0, cnt);
            if (g_last_found != want) {
                bad = std::string(m.name) + ": ждали ..." + want.substr(52)
                    + ", нашли " + (g_last_found.empty() ? std::string("(ничего)")
                                                         : "..." + g_last_found.substr(52));
                break;
            }
            done++;
        }

        const bool ok = (done == 5);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad("цепочка ключ->pubkey->база", 30)
                  << (ok ? "все 5 режимов: подложенный ключ найден"
                         : bad)
                  << "\n";
        if (!ok) fails++;

        g_quiet = sq;
        g_cfg.mode = sm; g_cfg.chunk_bits = sc; g_cfg.wave_start = sw;
        g_cfg.range_start_bit = sa; g_cfg.range_end_bit = sb2;
        compute_range();
    }

    // ========================================================================
    //  СЛУЧАЙНЫЙ РЕЖИМ: РАВНОМЕРНОСТЬ НА ПРОИЗВОЛЬНОМ ДИАПАЗОНЕ
    //
    //  Прежнее приведение резало только старший ненулевой лимб ширины, а
    //  младшие оставляло как есть.
    //
    //  ПОЧЕМУ ЭТО НЕ ЛОВИЛОСЬ ГОДАМИ. Диапазон задаётся парой [2^a, 2^b), а
    //  у такой ширины -- 2^b - 2^a - 1 -- все биты ниже a-го выставлены в
    //  единицу. Младшие лимбы получаются полными, и старая схема на них
    //  случайно совпадает с правильным ответом. Первая версия ЭТОГО теста
    //  брала [2^128, 2^130) и была пройдена старым кодом без единого
    //  замечания -- то есть не проверяла ничего.
    //
    //  Поэтому границы ставятся здесь напрямую, а не через пару степеней
    //  двойки: ширина 3*2^128 с нулевыми младшими лимбами. Старая схема
    //  приводит старший лимб по модулю 4, младшие оставляет случайными и
    //  выдаёт значения вплоть до 4*2^128 -- то есть четверть участков
    //  оказывается за концом диапазона. Такие ширины возникают не
    //  умозрительно: любое сужение границ (например, по старшему разряду)
    //  даёт произвольный отрезок, а не степень двойки.
    // ========================================================================
    {
        g_cfg.chunk_bits = 20;
        g_cfg.mode = TRAV_RANDOM;
        g_cfg.det_hex = -1;
        g_cfg.range_start_bit = 128; g_cfg.range_end_bit = 130;
        compute_range();
        // ширина ровно 3*2^128, начало смещено -- ни то, ни другое не
        // выражается парой степеней двойки
        u256_zero(g_range_start); g_range_start[2] = 1; g_range_start[0] = 12345;
        u256_set(g_range_end, g_range_start);
        g_range_end[2] += 3;

        std::mt19937_64 rng(20260814);
        const int N = 20000;
        int outside = 0, empty = 0;
        // Делим диапазон на 8 частей по старшим битам ширины и считаем
        // попадания: у равномерной выборки все восемь примерно равны.
        int bucket[8] = {0,0,0,0,0,0,0,0};
        u256 span; u256_sub(span, g_range_end, g_range_start);

        for (int i = 0; i < N; i++) {
            u256 k; uint64_t n = 0; int z = -1;
            next_chunk(CI((uint64_t)i), k, n, z, rng);
            if (u256_cmp(k, g_range_start) < 0 || u256_cmp(k, g_range_end) > 0) outside++;
            if (n == 0) empty++;
            // Номер восьмушки. Точность long double здесь избыточна: нас
            // интересует перекос в разы, а не третий знак после запятой.
            u256 off; u256_sub(off, k, g_range_start);
            long double frac = u256_to_ld(off) / u256_to_ld(span);
            int b = (int)(frac * 8.0L);
            if (b < 0) b = 0;
            if (b > 7) b = 7;
            bucket[b]++;
        }
        int lo = N, hi = 0;
        for (int i = 0; i < 8; i++) { if (bucket[i] < lo) lo = bucket[i]; if (bucket[i] > hi) hi = bucket[i]; }
        // Ждём по N/8 = 2500 на восьмушку; допускаем щедрый разброс, ловим
        // именно перекос в разы, а не статистический шум.
        bool ok = (outside == 0) && (empty == 0) && (lo > N / 8 / 2) && (hi < N / 8 * 2);
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("случайный: ширина 3*2^128", 30)
                  << "за краем " << outside << ", пустых " << empty
                  << ", восьмушки от " << lo << " до " << hi << " (ждём ~" << N/8 << ")\n";
        if (!ok) fails++;
        compute_range();      // вернуть границы, посчитанные обычным путём
    }

    // ========================================================================
    //  ГРАНИЦЫ 2^64, 2^128, 2^192 И n
    //
    //  Разбор верно заметил: почти все проверки жили на маленьких диапазонах,
    //  где uint64_t и u256 не расходятся. А расходятся они ровно на этих
    //  четырёх границах -- там, где перенос уходит из лимба в лимб. Гоняем у
    //  каждой границы все четыре режима и требуем одного: ни один ключ не
    //  выходит за диапазон, ни один участок не пуст без причины.
    // ========================================================================
    {
        struct B { int lo, hi; const char *name; };
        const B bs[] = {
            {  63,  65, "2^64"  },
            { 127, 129, "2^128" },
            { 191, 193, "2^192" },
            { 254, 256, "n"     },   // верх обрезается по порядку кривой
        };
        const int modes[4] = { TRAV_RANDOM, TRAV_SEQ, TRAV_ZONE, TRAV_KXE };
        const char *mnames[4] = { "случ.", "детерм.", "зон.", "KXE" };

        for (const B &b : bs) {
            int save_mode = g_cfg.mode;
            std::string bad;
            for (int m = 0; m < 4; m++) {
                g_cfg.range_start_bit = b.lo;
                g_cfg.range_end_bit   = b.hi;
                g_cfg.chunk_bits      = 20;
                g_cfg.mode            = modes[m];
                g_cfg.det_hex         = -1;
                compute_range();
                compute_top_nibble();
                zone_set_seed(99);

                std::mt19937_64 rng(1000 + m);
                int outside = 0, over = 0, got = 0;
                for (uint64_t i = 0; i < 3000; i++) {
                    u256 k; uint64_t n = 0; int z = -1;
                    if (!next_chunk(CI(i), k, n, z, rng)) break;
                    if (n == 0) continue;
                    got++;
                    if (u256_cmp(k, g_range_start) < 0) outside++;
                    u256 last; u256_add_u64(last, k, n - 1);
                    if (u256_cmp(last, g_range_end) > 0) over++;
                    // ключ обязан остаться годным: не ноль и меньше n
                    if (u256_cmp(last, EC_N) >= 0) over++;
                }
                if (outside || over || got == 0) {
                    if (!bad.empty()) bad += "; ";
                    bad += std::string(mnames[m]) + " ниже " + std::to_string(outside) +
                           ", выше " + std::to_string(over) +
                           (got ? "" : ", участков нет");
                }
            }
            bool ok = bad.empty();
            std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                      << pad_utf8(std::string("граница ") + b.name + ": 4 режима", 30)
                      << (ok ? "за диапазон не вышли" : bad) << "\n";
            if (!ok) fails++;
            g_cfg.mode = save_mode;
        }
    }

    // Полное покрытие у самой границы 2^64: диапазон узкий, но лежит верхом
    // на переносе между лимбами. Нарезка обязана закрыть его без дыр и
    // нахлёстов -- это тот же тест, что и раньше, но в опасном месте.
    {
        g_cfg.range_start_bit = 64; g_cfg.range_end_bit = 64 + 1;
        g_cfg.chunk_bits = 20;
        g_cfg.mode = TRAV_SEQ;
        g_cfg.det_hex = -1;
        compute_range();

        // ждём: 2^64 .. 2^65-1, то есть ровно 2^64 ключей
        u256 want_start; u256_zero(want_start); want_start[1] = 1;          // 2^64
        u256 width; u256_sub(width, g_range_end, g_range_start);
        bool bounds_ok = (u256_cmp(g_range_start, want_start) == 0) &&
                         (width[0] == ~0ULL && width[1] == 0 && width[2] == 0 && width[3] == 0);

        // покрытие первых участков: конец предыдущего + 1 == начало следующего
        int holes = 0, overlaps = 0;
        u256 prev_end; u256_zero(prev_end);
        for (uint64_t i = 0; i < 500; i++) {
            u256 k; uint64_t n = 0;
            if (!chunk_bounds(CI(i), k, n) || n == 0) break;
            if (i > 0) {
                u256 expect; u256_add_u64(expect, prev_end, 1);
                int c = u256_cmp(k, expect);
                if (c > 0) holes++;
                if (c < 0) overlaps++;
            }
            u256_add_u64(prev_end, k, n - 1);
        }
        bool ok = bounds_ok && holes == 0 && overlaps == 0;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("нарезка ровно на 2^64", 30)
                  << "границы " << (bounds_ok ? "верны" : "НЕВЕРНЫ")
                  << ", дыр " << holes << ", нахлёстов " << overlaps << "\n";
        if (!ok) fails++;
    }

    // ========================================================================
    //  НОМЕР УЧАСТКА ЗА ПРЕДЕЛОМ 2^64
    //
    //  Здесь стояла проверка обратного: что программа ЗНАЕТ о своей
    //  неспособности пройти больше 2^64 участков. Знание было честным, но
    //  ограничение осталось ограничением, и теперь его нет: счётчик, домен
    //  перестановки и учёт завершённости — всё 256-битное.
    //
    //  Проверяем то, что раньше было недостижимо: номера ЗА 2^64 дают
    //  правильные, различные участки, и KXE на них работает.
    // ========================================================================
    {
        g_cfg.range_start_bit = 0; g_cfg.range_end_bit = 256;
        g_cfg.chunk_bits = 10; g_cfg.det_hex = -1;
        g_cfg.mode = TRAV_SEQ;
        compute_range();

        // idx = 2^64 и 2^64+1 — первые номера, которых прежний счётчик
        // назвать не мог вовсе.
        ChunkIdx big;  big.from_u64(0); big.b[23] = 1;          // 2^64
        ChunkIdx big1 = big; big1.inc();

        u256 s0, s1; uint64_t c0 = 0, c1 = 0;
        bool got0 = chunk_bounds(big,  s0, c0);
        bool got1 = chunk_bounds(big1, s1, c1);

        // Начала должны отличаться ровно на размер участка.
        u256 diff; u256_sub(diff, s1, s0);
        bool step_ok = got0 && got1 &&
                       diff[0] == (1ULL << g_cfg.chunk_bits) &&
                       diff[1] == 0 && diff[2] == 0 && diff[3] == 0;

        // И само значение: start + 2^64 * 2^10 = start + 2^74.
        u256 want; u256_set(want, g_range_start);
        want[1] += (1ULL << (74 - 64));
        bool val_ok = got0 && u256_cmp(s0, want) == 0;

        // KXE на таком же номере обязан выдать участок, а не отказ.
        g_cfg.mode = TRAV_KXE;
        compute_range();
        u256 kk; uint64_t kn = 0; int kz = -1;
        std::mt19937_64 krng(1);
        bool kxe_ok = next_chunk(big, kk, kn, kz, krng) && kn > 0;

        bool ok = step_ok && val_ok && kxe_ok;
        std::cout << "  [" << (ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("участок за 2^64 достижим", 30)
                  << (ok ? "номера 2^64 и 2^64+1 дают верные участки, KXE работает"
                         : std::string("шаг ") + (step_ok ? "ок" : "НЕВЕРЕН") +
                           ", начало " + (val_ok ? "ок" : "НЕВЕРНО") +
                           ", KXE " + (kxe_ok ? "ок" : "ОТКАЗАЛ")) << "\n";
        if (!ok) fails++;
        g_cfg.mode = TRAV_SEQ;
    }

    // КРУПНЫЙ БЛОК: ТЕ ЖЕ ДВА РЕЖИМА ПРИ ДРУГОМ РАЗМЕРЕ.
    //
    // С видеокартой блок зон и волны -- 8 нибблов, 2^32 ключа. Проверяется не
    // «функция не упала», а три утверждения, каждое из которых при ошибке в
    // пересчёте сломалось бы молча:
    //
    //   1. ZONE_BLOCK отвечает числу нибблов -- иначе участок и узор считают
    //      разный размер блока, и часть ключей выпадет между ними;
    //   2. начало каждого блока выровнено (младшие 8 нибблов нулевые) -- иначе
    //      блоки наезжают друг на друга, а «зона» перестаёт быть зоной;
    //   3. все состояния одного старта волны РАЗНЫЕ -- ровно то, что
    //      проверяется выше для 3616 состояний при трёх нибблах. Если бы
    //      таблицы накоплений остались от прежнего размера, повторы пошли бы
    //      сразу и обход крутился бы на месте.
    {
        std::cout << "\n  --- крупный блок (как с видеокартой) ---\n";
        const int NIB = 8;
        set_block_nibbles(NIB);
        zone_set_seed(1); wave_set_seed(1);

        const bool size_ok = (ZONE_BLOCK == (1ULL << (4 * NIB)));
        std::cout << "  [" << (size_ok ? "OK " : "FAIL") << "] "
                  << pad_utf8("блок 8 нибблов = 2^32 ключей", 30)
                  << "ZONE_BLOCK = " << ZONE_BLOCK << "\n";
        if (!size_ok) fails++;

        auto low_nibbles_zero = [&](const u256 k) {
            uint8_t b[32]; u256_to_be32(b, k);
            for (int n = 0; n < NIB; n++) {
                const int pos = 63 - n, by = pos / 2;
                const int v = (pos % 2 == 0) ? (b[by] >> 4) : (b[by] & 0xF);
                if (v) return false;
            }
            return true;
        };

        g_cfg.zone_nibbles = NIB;
        int bad_zone = 0, bad_wave = 0;
        for (uint64_t i = 0; i < 2000; ++i) {
            ZoneInfo zi; u256 kz; zone_block_key(CI(i), kz, zi);
            if (!zi.valid || !low_nibbles_zero(zi.raw)
                || zi.block_keys != (UINT64_C(1) << (4 * NIB))) ++bad_zone;
            WaveInfo wi; u256 kw; wave_block_key(CI(i), kw, wi);
            if (!low_nibbles_zero(kw)) ++bad_wave;
        }
        std::cout << "  [" << (bad_zone == 0 ? "OK " : "FAIL") << "] "
                  << pad_utf8("зона: блок выровнен по 2^32", 30)
                  << "2000 блоков, неверных " << bad_zone << "\n";
        if (bad_zone) ++fails;
        g_cfg.zone_nibbles = 3;
        std::cout << "  [" << (bad_wave == 0 ? "OK " : "FAIL") << "] "
                  << pad_utf8("волна: блок выровнен по 2^32", 30)
                  << "2000 блоков, невыровненных " << bad_wave << "\n";
        if (bad_wave) fails++;

        const uint64_t PER = wave_states_per_start();
        std::set<std::string> seen;
        for (uint64_t st = 0; st < PER; st++) {
            WaveInfo wi; u256 k; wave_block_key(CI(st), k, wi);
            uint8_t b[32]; u256_to_be32(b, k);
            seen.insert(to_hex(b, 32));
        }
        const bool uniq = (seen.size() == PER);
        std::cout << "  [" << (uniq ? "OK " : "FAIL") << "] "
                  << pad_utf8("волна: состояния старта различны", 30)
                  << seen.size() << " различных из " << PER << "\n";
        if (!uniq) fails++;

        set_block_nibbles(saved_block_nib);      // возвращаем как было
    }

    fails += strict_range_self_test();

    std::cout << "\n" << (fails ? "ЕСТЬ ОШИБКИ" : "Все тесты пройдены.") << "\n";
    return fails;
}

// ========================================================
// ЗАМЕР СКОРОСТИ
// Один поток, база из одного ключа, которого в диапазоне нет.
// Меряется ровно горячий цикл.
// ========================================================
int bench(int threads, int seconds, uint64_t ntargets) {
    threads = bounded_worker_threads(threads);
    uint64_t batch_keys = UINT64_C(1) << 20;
#ifdef PKH_HAVE_CUDA
    if (g_gpu_active) {
        if (threads != 1) std::cout << "  CUDA benchmark: используется один управляющий поток.\n";
        threads = 1;
        batch_keys = UINT64_C(1) << 28;
    }
#endif
    g_db.items.clear();
    g_db.n_comp = g_db.n_uncomp = g_db.n_xonly = 0;
    g_db.add_hex("02" + std::string(63, 'a') + "b");   // заведомо отсутствующий X
    // ЗАМЕР ДОЛЖЕН МЕРИТЬ ТУ КОНФИГУРАЦИЮ, В КОТОРОЙ РАБОТАЮТ.
    //
    // С одной целью предфильтр идёт коротким путём: вычитание и проверка на
    // нуль, целиком в регистрах, ни одного обращения в память на ключ. На
    // боевой базе в миллионы ключей путь другой -- фильтр Блума размером с
    // мегабайты, который в кэш не помещается, и промах прячется конвейером
    // предвыборки лишь отчасти. Разница между этими двумя числами -- это
    // ровно то, что --bench скрывал: у пользователя 1.6 млн целей давали
    // 32.05 Mkey/s при замере в 37.17.
    //
    // Цели строятся арифметически (на кривой им лежать незачем -- сравнение
    // идёт по X), но кладутся в ту же базу и через тот же build(), поэтому и
    // размер фильтра, и раскладка по блокам получаются боевые.
    if (ntargets > 1) {
        g_db.items.reserve((size_t)ntargets + 1);
        uint64_t st = 0x9E3779B97F4A7C15ULL;
        for (uint64_t i = 0; i < ntargets; i++) {
            PubEntry e{};
            for (int w = 0; w < 4; w++) {
                st += 0x9E3779B97F4A7C15ULL;
                uint64_t z = st;
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                e.x[w] = z ^ (z >> 31);
            }
            e.par = 1;
            g_db.items.push_back(e);
        }
    }
    g_db.build();
    // Замер идёт по всему допустимому диапазону: сеть по [g_range_start,
    // g_range_end] не должна вмешиваться в измерение скорости.
    u256_from_u64(g_range_start, 1);
    u256_sub_u64(g_range_end, EC_N, 1);
    if (!push_targets_to_backend()) return 1;
    g_quiet = true;
    g_count.store(0);

    std::atomic<bool> stop{false};
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    ThreadJoinScope workers_guard(pool, [&] { stop.store(true); set_stop(); });
    for (int t = 0; t < threads; t++) {
        pool.emplace_back([&, t]() {
            u256 k;
            hex_to_u256("1000000000000000", k);
            k[0] += (uint64_t)t << 40;
            while (!stop.load(std::memory_order_relaxed)) {
                scan_chunk(k, batch_keys);
                k[0] += batch_keys;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop.store(true);
    set_stop();
    for (auto &th : pool) th.join();
    g_stop.store(false); g_stop_flag = 0;

    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint64_t n = g_count.load();
    std::cout << "  Потоков: " << threads
              << " | целей: " << fmt_num(g_db.items.size())
              << " | ключей: " << fmt_num(n)
              << " | время: " << el << " c\n"
              << "  СКОРОСТЬ: " << (n / el / 1e6) << " Mkey/s"
              << "  (" << (n / el / 1e6 / threads) << " Mkey/s на поток)\n";
    return 0;
}

// ========================================================
