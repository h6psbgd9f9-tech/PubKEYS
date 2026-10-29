#pragma once

#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>

// Accept the displayed HEX digits, plus legacy decimal 10..15 and -1 (all).
// Failure leaves digit unchanged; range-specific validation belongs to caller.
inline bool parse_top_hex_digit(const std::string& input, int& digit) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    const std::string text = input.substr(begin, end - begin);
    if (text == "-1") {
        digit = -1;
        return true;
    }
    if (text.size() == 2 && text[0] == '1' && text[1] >= '0' && text[1] <= '5') {
        digit = 10 + text[1] - '0';
        return true;
    }
    std::size_t position = 0;
    if (text.size() == 3 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) position = 2;
    else if (text.size() != 1) return false;
    const char ch = text[position];
    if (ch >= '0' && ch <= '9') digit = ch - '0';
    else if (ch >= 'a' && ch <= 'f') digit = 10 + ch - 'a';
    else if (ch >= 'A' && ch <= 'F') digit = 10 + ch - 'A';
    else return false;
    return true;
}

// Save the exact chosen double in the decimal notation accepted below, even
// when a small value would normally be printed using a scientific exponent.
inline std::string format_digit_hours(double hours) {
    if (!std::isfinite(hours) || hours < 0.0) return std::string();
    if (hours == 0.0) return "0";
    const int leading_fractional_zeros = hours < 1.0
        ? static_cast<int>(std::ceil(-std::log10(hours))) : 0;
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::fixed
           << std::setprecision(std::numeric_limits<double>::max_digits10 + leading_fractional_zeros)
           << hours;
    std::string number = stream.str();
    while (!number.empty() && number.back() == '0') number.pop_back();
    if (!number.empty() && number.back() == '.') number.pop_back();
    return number;
}

// Parse hours independently of the process locale. Accept ordinary decimal
// notation with either decimal separator; failure leaves hours unchanged.
inline bool parse_digit_hours(const std::string& input, double& hours) {
    std::size_t begin = 0;
    std::size_t end = input.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(input[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) --end;
    if (begin == end) return false;

    std::string number = input.substr(begin, end - begin);
    bool saw_digit = false;
    bool saw_separator = false;
    for (char& ch : number) {
        if (ch >= '0' && ch <= '9') {
            saw_digit = true;
        } else if ((ch == '.' || ch == ',') && !saw_separator) {
            ch = '.';
            saw_separator = true;
        } else {
            return false;
        }
    }
    if (!saw_digit) return false;

    std::istringstream stream(number);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (stream.fail() || !stream.eof() || !std::isfinite(value) || value < 0.0 || value > 8760.0)
        return false;
    // Do not silently turn a positive but unrepresentable decimal into "off".
    if (value == 0.0 && number.find_first_of("123456789") != std::string::npos) return false;
    hours = value;
    return true;
}

// Host-side timer for cycling an inclusive range of leading HEX digits.
// The caller starts a fresh dwell after switching ranges, so initialization
// and checkpoint I/O do not consume the next digit's search time.
class DigitRotation {
public:
    using Clock = std::chrono::steady_clock;

    DigitRotation(int first, int last, int current, double seconds)
        : first_(first), last_(last), current_(current), seconds_(seconds),
          enabled_(first >= 0 && last <= 15 && first < last &&
                   current >= first && current <= last &&
                   std::isfinite(seconds) && seconds > 0.0) {}

    bool enabled() const { return enabled_; }
    int current() const { return current_; }

    void start(Clock::time_point now) {
        started_at_ = now;
        started_ = true;
    }

    void pause(Clock::duration duration) {
        if (started_ && duration > Clock::duration::zero()) started_at_ += duration;
    }

    bool due(Clock::time_point now) const {
        return enabled_ && started_ &&
               std::chrono::duration<double>(now - started_at_).count() >= seconds_;
    }

    // A fully searched digit stays excluded for this process. A timed switch
    // leaves it eligible for another turn. Return false once none remain.
    bool advance(bool completed) {
        if (!enabled_) return false;
        if (completed) completed_ |= static_cast<std::uint16_t>(1u << current_);
        int candidate = current_;
        for (int remaining = last_ - first_ + 1; remaining > 0; --remaining) {
            candidate = candidate == last_ ? first_ : candidate + 1;
            if ((completed_ & (1u << candidate)) == 0) {
                current_ = candidate;
                started_ = false;
                return true;
            }
        }
        started_ = false;
        return false;
    }

private:
    int first_;
    int last_;
    int current_;
    double seconds_;
    bool enabled_;
    bool started_ = false;
    std::uint16_t completed_ = 0;
    Clock::time_point started_at_{};
};

