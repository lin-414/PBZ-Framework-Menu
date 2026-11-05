// M1: the single atomic store writer lives in its own translation unit so
// framework_test can exercise the real config/schema persistence path without
// linking DllMain, the overlay or the game-window plumbing (Test host stubs the
// paths/logging in tests/framework_test_stubs.cpp).
#include "common.h"

namespace pbzfm {

bool WriteFileAtomic(const std::wstring& path, const std::string& utf8) {
    // M1: write beside the target, flush through the OS cache, then rename over
    // it. A crash mid-write now leaves the old store intact instead of a
    // truncated/empty file, and the single code path keeps config and schema
    // stores from diverging.
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("ERROR: cannot create temp store %ls err=%lu", tmp.c_str(), GetLastError());
        return false;
    }
    bool ok = true;
    size_t written = 0;
    while (written < utf8.size()) {
        size_t left = utf8.size() - written;
        DWORD ask = (DWORD)(left > 0x7FFFFFFFu ? 0x7FFFFFFFu : left);
        DWORD chunk = 0;
        if (!WriteFile(h, utf8.data() + written, ask, &chunk, nullptr) || chunk == 0) { ok = false; break; }
        written += chunk;
    }
    if (ok && !FlushFileBuffers(h)) ok = false;   // flushed: the rename alone guarantees nothing
    CloseHandle(h);
    if (ok) {
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ok = false;
            Log("ERROR: atomic replace %ls failed err=%lu", path.c_str(), GetLastError());
        }
    } else {
        Log("ERROR: write %ls failed err=%lu (temp %ls kept out of the way)",
            path.c_str(), GetLastError(), tmp.c_str());
    }
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

} // namespace pbzfm
