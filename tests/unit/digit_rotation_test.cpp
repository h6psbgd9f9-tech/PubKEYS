#include "digit_rotation.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "minute_speed.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void parse_ok(const std::string& text, double expected) {
    double result = -1.0;
    check(parse_digit_hours(text, result) && result == expected, text.c_str());
}

void parse_bad(const std::string& text) {
    double result = 42.0;
    check(!parse_digit_hours(text, result) && result == 42.0, text.c_str());
}
} // namespace

int main() {
    check(power_after_key(75, '+') == 80, "power increase");
    check(power_after_key(75, '-') == 70, "power decrease");
    check(power_after_key(75, '=') == 80, "power plus alias");
    check(power_after_key(75, '_') == 70, "power minus alias");
    check(power_after_key(100, '+') == 100, "upper power bound");
    check(power_after_key(50, '-') == 50, "lower power bound");
    check(MinuteSpeed::format_status(80, MinuteSpeed::format_rate(3464071335.0)) ==
          L"Мощность: 80% | Скорость: 3 464 071 335 Key/S", "combined power/rate status");
    using namespace std::chrono;
    const DigitRotation::Clock::time_point origin{};

    DigitRotation cycle(4, 7, 6, 7200.0);
    check(cycle.enabled() && cycle.current() == 6, "start at digit 6");
    check(!cycle.due(origin + hours(24)), "timer waits for explicit start");
    cycle.start(origin);
    check(!cycle.due(origin + hours(2) - milliseconds(1)), "two-hour dwell does not expire early");
    check(cycle.due(origin + hours(2)), "two-hour dwell expires at boundary");
    cycle.pause(minutes(15));
    check(!cycle.due(origin + hours(2)), "pause is excluded from working time");
    check(cycle.due(origin + hours(2) + minutes(15)), "pause shifts expiration by its duration");
    cycle.pause(-minutes(30));
    check(!cycle.due(origin + hours(2)), "negative pause cannot shorten dwell");
    const int expected_digits[] = {7, 4, 5, 6, 7};
    for (int digit : expected_digits) {
        check(cycle.advance(false) && cycle.current() == digit, "timed rotation wraps in ascending order");
        check(!cycle.due(origin + hours(24)), "switch waits for next dwell start");
        cycle.start(origin + hours(24));
        check(!cycle.due(origin + hours(25)), "every digit gets a fresh full dwell");
        check(cycle.due(origin + hours(26)), "fresh dwell expires after two hours");
    }

    DigitRotation completed(4, 7, 6, 1.0);
    check(completed.advance(true) && completed.current() == 7, "completion advances 6 to 7");
    check(completed.advance(true) && completed.current() == 4, "completion wraps 7 to 4");
    check(completed.advance(false) && completed.current() == 5, "timed exit keeps 4 eligible");
    check(completed.advance(false) && completed.current() == 4, "rotation skips completed digits 6 and 7");
    check(completed.advance(true) && completed.current() == 5, "completion skips all exhausted digits");
    check(completed.advance(false) && completed.current() == 5, "only unfinished digit remains eligible");
    check(!completed.advance(true), "all completed digits stop rotation");
    check(!completed.advance(false), "exhausted rotation cannot restart completed digits");

    DigitRotation disabled[] = {
        DigitRotation(4, 7, 6, 0.0),
        DigitRotation(4, 7, 6, -1.0),
        DigitRotation(4, 7, -1, 7200.0),
        DigitRotation(6, 6, 6, 7200.0),
        DigitRotation(7, 4, 6, 7200.0),
        DigitRotation(4, 7, 8, 7200.0),
        DigitRotation(-1, 7, 6, 7200.0),
        DigitRotation(4, 16, 6, 7200.0),
        DigitRotation(4, 7, 6, std::numeric_limits<double>::infinity()),
        DigitRotation(4, 7, 6, std::numeric_limits<double>::quiet_NaN())
    };
    for (auto& item : disabled) {
        const int original_digit = item.current();
        item.start(origin);
        check(!item.enabled() && !item.due(origin + hours(24)), "disabled or invalid configuration has no timer");
        check(!item.advance(true) && item.current() == original_digit, "disabled rotation preserves selected digit");
    }

    DigitRotation full_hex(0, 15, 15, 0.25);
    full_hex.start(origin);
    check(!full_hex.due(origin + milliseconds(249)), "fractional seconds do not expire early");
    check(full_hex.due(origin + milliseconds(250)), "fractional-second deadline is respected");
    check(full_hex.advance(true) && full_hex.current() == 0, "HEX digit F wraps to 0");
    for (int digit = 0; digit < 14; ++digit)
        check(full_hex.advance(true) && full_hex.current() == digit + 1, "complete full HEX digit range");
    check(!full_hex.advance(true), "all sixteen HEX digits can be marked complete");

    parse_ok("0", 0.0);
    parse_ok("000.000", 0.0);
    parse_ok("2", 2.0);
    parse_ok(" \t2,5\r\n", 2.5);
    parse_ok(".5", 0.5);
    parse_ok(",5", 0.5);
    parse_ok("2.", 2.0);
    parse_ok("0.0001", 0.0001);
    parse_ok("8760", 8760.0);
    parse_ok("8760,0", 8760.0);
    const char* malformed[] = {
        "", " \t\n", ".", ",", "-1", "-0", "+2", "nan", "NaN", "inf", "Infinity",
        "2h", "2 5", "2\n5", "1e2", "2.5.0", "2,5,0", "2,5.0", "8760.01", "8761",
        "999999999999999999999999999999999999999999999999999999999999999999"
    };
    for (const char* text : malformed) parse_bad(text);
    parse_bad(std::string("2\0hours", 7));
    parse_bad("0." + std::string(400, '0') + "1");

    const double roundtrip_hours[] = {
        0.0, -0.0, 2.0, 2.5, 0.1, 1.0 / 3.0, 1.0 / 3600.0, 1e-5,
        0.12345678901234567, 1234.5678901234567, 8760.0,
        std::nextafter(8760.0, 0.0), 1e-100, 1e-300,
        std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::denorm_min() * 12345.0
    };
    for (double hours_value : roundtrip_hours) {
        const std::string text = format_digit_hours(hours_value);
        check(text.find_first_of("eE") == std::string::npos, "saved duration never uses exponent notation");
        double restored = -1.0;
        check(parse_digit_hours(text, restored) && restored == hours_value,
              "saved duration round-trips without precision loss");
    }
    check(format_digit_hours(2.5) == "2.5", "formatter removes fractional trailing zeros");
    check(format_digit_hours(8760.0) == "8760", "formatter keeps integral trailing zeros");
    check(format_digit_hours(0.0) == "0", "formatter produces compact zero");

    struct DigitInput { const char* text; int expected; };
    const DigitInput valid_digits[] = {
        {"-1", -1}, {" \t-1\r\n", -1}, {"0", 0}, {"1", 1}, {"9", 9},
        {"a", 10}, {"b", 11}, {"c", 12}, {"d", 13}, {"e", 14}, {"f", 15},
        {"A", 10}, {"B", 11}, {"C", 12}, {"D", 13}, {"E", 14}, {"F", 15},
        {"  E\t", 14}, {"0xe", 14}, {"0xE", 14}, {"0Xe", 14}, {"0XF", 15},
        {"0x0", 0}, {"0x9", 9}, {"10", 10}, {"11", 11}, {"12", 12},
        {"13", 13}, {"14", 14}, {"15", 15}
    };
    for (const DigitInput& item : valid_digits) {
        int digit = -2;
        check(parse_top_hex_digit(item.text, digit) && digit == item.expected, item.text);
    }
    const char* invalid_digits[] = {
        "", " \t\n", "-2", "+1", "-0", "00", "01", "09", "16", "99", "ff",
        "0x", "0x10", "0x-1", "0xg", "g", "G", "e1", "14junk", "e f", "0x e",
        "e000000000000000000", "12345678901234567890", "1.0", "1,0"
    };
    for (const char* text : invalid_digits) {
        int digit = -2;
        check(!parse_top_hex_digit(text, digit) && digit == -2, text);
    }
    int embedded_nul_digit = -2;
    check(!parse_top_hex_digit(std::string("e\0f", 3), embedded_nul_digit) && embedded_nul_digit == -2,
          "digit parser rejects embedded NUL and preserves output");
    DigitRotation high_hex_cycle(8, 15, 14, 7200.0);
    const int high_hex_expected[] = {15, 8, 9, 10, 11, 12, 13, 14};
    for (int digit : high_hex_expected)
        check(high_hex_cycle.advance(false) && high_hex_cycle.current() == digit,
              "HEX rotation follows e-f-8-9-a-b-c-d-e");

    if (failures) {
        std::cerr << failures << " digit rotation test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Digit rotation tests passed (timing, pauses, wraparound, exhaustion, input validation).\n";
    return EXIT_SUCCESS;
}

