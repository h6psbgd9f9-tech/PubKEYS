#pragma once
#include "platform.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

// The caller serializes writers and retains this state until success. If the
// rename succeeded but directory sync failed, retry sync without appending twice.
struct FoundWriteState {
    bool replaced = false;
    std::string temporary;
};

inline bool persist_found_record(const std::string &path, const std::string &record,
                                 FoundWriteState &state, std::string &why,
                                 bool (*sync_file)(const std::string &) = plat_fsync_file,
                                 bool (*replace_file)(const std::string &, const std::string &) = plat_replace_file,
                                 bool (*sync_dir)(const std::string &) = plat_fsync_dir) {
    try {
    why.clear();
    if (path.empty() || path.find('\0') != std::string::npos) {
        why = "пустой или недопустимый путь";
        return false;
    }
    const auto target = std::filesystem::u8path(path);
    const auto parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
    const std::string directory = parent.u8string();
    if (state.replaced) {
        if (sync_file(path) && sync_dir(directory)) return true;
        why = "запись заменена, но сброс на диск ещё не подтверждён";
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(parent, error);
    if (error) { why = "не удалось создать каталог результата"; return false; }
    if (state.temporary.empty())
        state.temporary = path + ".pubkeyhunt-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";

    errno = 0;
    FILE *old = plat_fopen(path, "rb");
    if (!old && errno != ENOENT) { why = "не удалось прочитать прежний файл"; return false; }
    std::unique_ptr<FILE, decltype(&std::fclose)> old_guard(old, &std::fclose);
    FILE *out = plat_fopen(state.temporary, "wb");
    if (!out) {
        why = "не удалось открыть временный файл";
        return false;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> out_guard(out, &std::fclose);
    plat_make_file_private(state.temporary); // before writing private material
    bool ok = true;
    if (old) {
        char buffer[65536];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof buffer, old)) != 0) {
            if (std::fwrite(buffer, 1, got, out) != got) { ok = false; break; }
        }
        if (std::ferror(old)) ok = false;
        if (std::fclose(old_guard.release()) != 0) ok = false;
    }
    if (ok && std::fwrite(record.data(), 1, record.size(), out) != record.size()) ok = false;
    if (std::fflush(out) != 0) ok = false;
    if (std::fclose(out_guard.release()) != 0) ok = false;
    if (!ok || !sync_file(state.temporary)) {
        plat_remove_file(state.temporary);
        why = "запись или сброс временного файла не удались";
        return false;
    }
    if (!replace_file(state.temporary, path)) {
        plat_remove_file(state.temporary);
        why = "не удалось заменить файл результата";
        return false;
    }
    state.replaced = true;
    if (!sync_dir(directory)) {
        why = "не удалось сбросить каталог результата";
        return false;
    }
    return true;
    } catch (...) {
        // Preserve replaced=true after a successful rename: retry only the
        // durability confirmation instead of appending the record twice.
        if (!state.replaced && !state.temporary.empty()) {
            try { plat_remove_file(state.temporary); } catch (...) {}
        }
        try { why = "исключение при записи результата; запись будет повторена"; } catch (...) {}
        return false;
    }
}
