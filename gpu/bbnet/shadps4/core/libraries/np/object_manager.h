// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <mutex>
#include <ranges>

template <typename T, size_t N, int INVALID_OBJECT_ID_ERROR, int OBJECT_NOT_FOUND_ERROR,
          int MAX_OBJECTS_ERROR>
struct ObjectManager {
    s32 GetObject(int objectId, T** out) {
        std::scoped_lock lk{mutex};
        if (objectId < 1 || objectId > N) {
            return INVALID_OBJECT_ID_ERROR;
        }
        auto obj = objects[objectId - 1];
        if (!obj) {
            return OBJECT_NOT_FOUND_ERROR;
        }
        *out = obj;
        return ORBIS_OK;
    }

    template <typename... Args>
    s32 CreateObject(Args&&... args) {
        std::scoped_lock lk{mutex};

        // bbport: round-robin from the slot after the last one handed out, so an id just freed
        // is not handed out again at once. GUNDAM VERSUS deletes a finished TUS request a second
        // time after creating the next one; with the freed id reused, that deleted the new
        // request, which it then polled for ever ("LOGGING IN"). On a PS4 ids are not reused
        // at once either.
        for (size_t i = 0; i < N; ++i) {
            const size_t at = (next + i) % N;
            if (!objects[at]) {
                objects[at] = new T{args...};
                next = (at + 1) % N;
                return static_cast<s32>(at + 1);
            }
        }

        return MAX_OBJECTS_ERROR;
    }

    s32 DeleteObject(int objectId) {
        std::scoped_lock lk{mutex};

        if (objectId < 1 || objectId > N) {
            return INVALID_OBJECT_ID_ERROR;
        }
        auto obj = objects[objectId - 1];
        if (!obj) {
            return OBJECT_NOT_FOUND_ERROR;
        }

        delete obj;
        objects[objectId - 1] = nullptr;

        return ORBIS_OK;
    }

private:
    std::mutex mutex;
    std::array<T*, N> objects = {nullptr};
    size_t next = 0;
};