#include "config_files.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace skeetsdk::config_files
{
    namespace
    {
        constexpr std::uintptr_t store_path_address = 0x434798CCu;    // UNICODE_STRING: the store's NT path
        constexpr std::uint8_t store_marker = 0xAF;
        constexpr std::uint8_t field_string = 0x01;
        constexpr std::uint8_t field_byte = 0x03;
        constexpr std::uint8_t no_key = 0x1B;
        constexpr std::uint32_t config_signature = 0xDEC00D60u;
        constexpr wchar_t keys_file_name[] = L"keys.txt";

        // The Config tab (see config_scripts) and its list reload, which the Save button runs after saving.
        constexpr std::uintptr_t config_tab_vtable_address = 0x4346F6DCu;
        constexpr std::uintptr_t menu_pointer_address = 0x43476924u;
        constexpr std::size_t menu_tabs_begin = 0x54;
        constexpr std::size_t menu_tabs_end = 0x58;
        constexpr std::uintptr_t refresh_list_address = 0x4335C747u; // __thiscall(config tab)
        // sub esp, 44h / push ebx / push ebp / xor eax, eax
        constexpr std::array<std::uint8_t, 7> refresh_list_prologue{ 0x83, 0xEC, 0x44, 0x53, 0x55, 0x33, 0xC0 };
        using RefreshListFn = void(__thiscall*)(void*);

        struct NtString // UNICODE_STRING
        {
            std::uint16_t length;
            std::uint16_t maximum;
            wchar_t* buffer;
        };

        struct Config
        {
            std::string data;
            std::uint8_t key = no_key;
            bool operator==(const Config&) const = default;
        };
        using Configs = std::map<std::string, Config>; // name (UTF-8) -> config

        struct Stamp
        {
            bool exists = false;
            std::uint64_t size = 0;
            std::uint64_t written = 0;
            bool operator==(const Stamp&) const = default;
        };
        using Snapshot = std::map<std::wstring, Stamp>;

        struct State
        {
            bool started = false;
            std::wstring store;    // DOS path of the store
            std::wstring folder;   // ...\skeet\configs
            Stamp store_stamp;
            Snapshot folder_snapshot;
            std::set<std::string> mirrored; // names whose files this wrote
            HANDLE notification = INVALID_HANDLE_VALUE;
        };
        State state;

        bool IsReadable(const void* address, std::size_t size) noexcept
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) == 0) return false;
            if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
            const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            return reinterpret_cast<std::uintptr_t>(address) + size <= end;
        }

        // --- files ---------------------------------------------------------------------------------------

        Stamp StampOf(const std::wstring& path) noexcept
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)
                || (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
            return { true, (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime };
        }

        std::optional<std::string> ReadWholeFile(const std::wstring& path)
        {
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return std::nullopt;
            std::string bytes;
            LARGE_INTEGER size{};
            bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart < 256ll * 1024 * 1024;
            if (ok)
            {
                bytes.resize(static_cast<std::size_t>(size.QuadPart));
                DWORD done = 0;
                ok = bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) && done == bytes.size());
            }
            CloseHandle(file);
            if (!ok) return std::nullopt;
            return bytes;
        }

        // Through a temporary file, so a reader never sees half a file.
        bool WriteWholeFile(const std::wstring& path, std::string_view bytes)
        {
            const std::wstring temporary = path + L".tmp";
            const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            DWORD done = 0;
            const bool ok = bytes.empty() || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr) && done == bytes.size());
            CloseHandle(file);
            if (ok && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            DeleteFileW(temporary.c_str());
            return false;
        }

        bool WriteIfDifferent(const std::wstring& path, std::string_view bytes)
        {
            const std::optional<std::string> current = ReadWholeFile(path);
            if (current.has_value() && *current == bytes) return true;
            return WriteWholeFile(path, bytes);
        }

        std::wstring Widen(std::string_view utf8)
        {
            if (utf8.empty()) return {};
            const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
            if (length > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
            return wide;
        }

        std::string Narrow(std::wstring_view wide)
        {
            if (wide.empty()) return {};
            const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
            if (length > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
            return utf8;
        }

        // A config name as a file name: characters Windows does not allow (and % itself, a trailing dot or space,
        // and the first letter of a device name such as CON) become %XX.
        std::wstring FileNameOf(const std::string& name)
        {
            const std::wstring wide = Widen(name);
            std::wstring stem;
            for (std::size_t i = 0; i < wide.size(); ++i)
            {
                const wchar_t c = wide[i];
                const bool last = i + 1 == wide.size();
                if (c < 0x20 || std::wcschr(L"\\/:*?\"<>|%", c) != nullptr || (last && (c == L'.' || c == L' ')))
                {
                    wchar_t escaped[8]{};
                    std::swprintf(escaped, 8, L"%%%02X", static_cast<unsigned>(c) & 0xFF);
                    stem += escaped;
                }
                else
                {
                    stem += c;
                }
            }
            static constexpr const wchar_t* devices[] = { L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3",
                L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
                L"LPT6", L"LPT7", L"LPT8", L"LPT9" };
            for (const wchar_t* device : devices)
                if (_wcsicmp(stem.c_str(), device) == 0)
                {
                    wchar_t escaped[8]{};
                    std::swprintf(escaped, 8, L"%%%02X", static_cast<unsigned>(stem[0]) & 0xFF);
                    stem = escaped + stem.substr(1);
                    break;
                }
            return stem + L".cfg";
        }

        std::optional<std::string> NameOf(std::wstring_view file_name)
        {
            if (file_name.size() <= 4 || _wcsicmp(std::wstring(file_name.substr(file_name.size() - 4)).c_str(), L".cfg") != 0)
                return std::nullopt;
            const std::wstring_view stem = file_name.substr(0, file_name.size() - 4);
            std::wstring wide;
            for (std::size_t i = 0; i < stem.size(); ++i)
            {
                if (stem[i] == L'%' && i + 2 < stem.size() && std::iswxdigit(stem[i + 1]) && std::iswxdigit(stem[i + 2]))
                {
                    wide += static_cast<wchar_t>(std::wcstoul(std::wstring(stem.substr(i + 1, 2)).c_str(), nullptr, 16));
                    i += 2;
                }
                else
                {
                    wide += stem[i];
                }
            }
            if (wide.empty()) return std::nullopt;
            return Narrow(wide);
        }

        // --- the store -------------------------------------------------------------------------------------

        bool ParseStore(std::string_view bytes, Configs& configs)
        {
            configs.clear();
            if (bytes.empty() || static_cast<std::uint8_t>(bytes[0]) != store_marker) return false;
            std::size_t at = 1;
            auto read_string = [&](std::string& out) {
                if (at + 5 > bytes.size() || static_cast<std::uint8_t>(bytes[at]) != field_string) return false;
                std::uint32_t length = 0;
                std::memcpy(&length, bytes.data() + at + 1, sizeof(length));
                if (length > bytes.size() - at - 5) return false;
                out.assign(bytes.substr(at + 5, length));
                at += 5 + length;
                return true;
            };
            while (at < bytes.size())
            {
                std::string name;
                Config config;
                if (!read_string(name) || !read_string(config.data)) return false;
                if (at + 2 > bytes.size() || static_cast<std::uint8_t>(bytes[at]) != field_byte) return false;
                config.key = static_cast<std::uint8_t>(bytes[at + 1]);
                at += 2;
                configs.insert_or_assign(std::move(name), std::move(config));
            }
            return true;
        }

        std::string BuildStore(const Configs& configs)
        {
            std::string bytes(1, static_cast<char>(store_marker));
            auto write_string = [&](std::string_view text) {
                bytes += static_cast<char>(field_string);
                const auto length = static_cast<std::uint32_t>(text.size());
                bytes.append(reinterpret_cast<const char*>(&length), sizeof(length));
                bytes += text;
            };
            for (const auto& [name, config] : configs)
            {
                write_string(name);
                write_string(config.data);
                bytes += static_cast<char>(field_byte);
                bytes += static_cast<char>(config.key);
            }
            return bytes;
        }

        // --- the folder ------------------------------------------------------------------------------------

        Snapshot SnapshotOf(const std::wstring& folder)
        {
            Snapshot snapshot;
            WIN32_FIND_DATAW found{};
            const HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
            if (search == INVALID_HANDLE_VALUE) return snapshot;
            do
            {
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                snapshot[found.cFileName] = { true, (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow,
                    (static_cast<std::uint64_t>(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime };
            } while (FindNextFileW(search, &found));
            FindClose(search);
            return snapshot;
        }

        // keys.txt: "<key slot>\t<name>" per line, for configs with a key.
        std::map<std::string, std::uint8_t> ReadKeys(const std::wstring& folder)
        {
            std::map<std::string, std::uint8_t> keys;
            const std::optional<std::string> text = ReadWholeFile(folder + L"\\" + keys_file_name);
            if (!text.has_value()) return keys;
            std::size_t start = 0;
            while (start < text->size())
            {
                std::size_t end = text->find('\n', start);
                if (end == std::string::npos) end = text->size();
                std::string line = text->substr(start, end - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const std::size_t tab = line.find('\t');
                if (tab != std::string::npos && tab > 0)
                {
                    char* stop = nullptr;
                    const unsigned long key = std::strtoul(line.substr(0, tab).c_str(), &stop, 10);
                    if (stop != nullptr && *stop == '\0' && key <= 0xFF) keys[line.substr(tab + 1)] = static_cast<std::uint8_t>(key);
                }
                start = end + 1;
            }
            return keys;
        }

        Configs LoadFolder(const std::wstring& folder)
        {
            Configs configs;
            const std::map<std::string, std::uint8_t> keys = ReadKeys(folder);
            for (const auto& [file_name, stamp] : SnapshotOf(folder))
            {
                const std::optional<std::string> name = NameOf(file_name);
                if (!name.has_value()) continue;
                std::optional<std::string> data = ReadWholeFile(folder + L"\\" + file_name);
                std::uint32_t signature = 0;
                if (!data.has_value() || data->size() < sizeof(signature)) continue;
                std::memcpy(&signature, data->data(), sizeof(signature));
                if (signature != config_signature) continue;
                Config config{ std::move(*data), no_key };
                if (const auto key = keys.find(*name); key != keys.end()) config.key = key->second;
                configs.insert_or_assign(*name, std::move(config));
            }
            return configs;
        }

        // Writes the store's configs as files. Removes only the files of configs this wrote before and the store
        // no longer has, never a file someone put there.
        void WriteFolder(const Configs& configs)
        {
            std::string keys;
            for (const auto& [name, config] : configs)
            {
                WriteIfDifferent(state.folder + L"\\" + FileNameOf(name), config.data);
                if (config.key != no_key) keys += std::to_string(config.key) + "\t" + name + "\r\n";
            }
            for (const std::string& name : state.mirrored)
                if (configs.find(name) == configs.end()) DeleteFileW((state.folder + L"\\" + FileNameOf(name)).c_str());
            const std::wstring keys_path = state.folder + L"\\" + keys_file_name;
            if (keys.empty()) DeleteFileW(keys_path.c_str());
            else WriteIfDifferent(keys_path, keys);
            state.mirrored.clear();
            for (const auto& [name, config] : configs) state.mirrored.insert(name);
        }

        void RefreshConfigList() noexcept
        {
            const auto* menu_slot = reinterpret_cast<std::uint8_t* const*>(menu_pointer_address);
            if (!IsReadable(menu_slot, sizeof(void*)) || *menu_slot == nullptr) return;
            if (!IsReadable(reinterpret_cast<const void*>(refresh_list_address), refresh_list_prologue.size())
                || std::memcmp(reinterpret_cast<const void*>(refresh_list_address), refresh_list_prologue.data(),
                    refresh_list_prologue.size()) != 0) return;
            std::uint8_t* menu = *menu_slot;
            if (!IsReadable(menu + menu_tabs_begin, 2 * sizeof(void*))) return;
            void** first = *reinterpret_cast<void***>(menu + menu_tabs_begin);
            void** last = *reinterpret_cast<void***>(menu + menu_tabs_end);
            if (first == nullptr || last < first || !IsReadable(first, (last - first) * sizeof(void*))) return;
            for (void** tab = first; tab != last; ++tab)
            {
                if (*tab == nullptr || !IsReadable(*tab, sizeof(void*))
                    || *static_cast<const std::uintptr_t*>(*tab) != config_tab_vtable_address) continue;
                reinterpret_cast<RefreshListFn>(refresh_list_address)(*tab);
                return;
            }
        }

        void Watch() noexcept
        {
            if (state.notification != INVALID_HANDLE_VALUE) FindCloseChangeNotification(state.notification);
            state.notification = FindFirstChangeNotificationW(state.folder.c_str(), FALSE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        }

        void StoreFromFolder(const Configs& configs)
        {
            WriteWholeFile(state.store, BuildStore(configs));
            state.store_stamp = StampOf(state.store);
            state.mirrored.clear();
            for (const auto& [name, config] : configs) state.mirrored.insert(name);
            RefreshConfigList();
        }

        // The store's DOS path, once the cheat has set it up ("\??\c:\...\csgo\cfg\<id>.cfg").
        std::wstring StorePath()
        {
            const auto* path = reinterpret_cast<const NtString*>(store_path_address);
            if (!IsReadable(path, sizeof(*path)) || path->buffer == nullptr || path->length == 0
                || !IsReadable(path->buffer, path->length)) return {};
            std::wstring store(path->buffer, path->length / sizeof(wchar_t));
            if (store.rfind(L"\\??\\", 0) == 0) store.erase(0, 4);
            return store;
        }

        void Start()
        {
            state.store = StorePath();
            if (state.store.empty()) return;
            wchar_t executable[MAX_PATH]{};
            const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) return;
            std::wstring folder(executable, length);
            folder.resize(folder.find_last_of(L'\\'));
            folder += L"\\skeet";
            CreateDirectoryW(folder.c_str(), nullptr);
            folder += L"\\configs";
            if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
            state.folder = folder;

            // The store as it was before any of this, once.
            const std::wstring backup = state.store + L".skeetsdk-backup";
            if (StampOf(state.store).exists && !StampOf(backup).exists) CopyFileW(state.store.c_str(), backup.c_str(), TRUE);

            // Both ways at the start: configs saved while this was not running are in the store only, configs
            // copied in meanwhile in the folder only. The folder wins when both have a name.
            Configs from_store;
            if (const std::optional<std::string> bytes = ReadWholeFile(state.store); bytes.has_value())
                ParseStore(*bytes, from_store);
            Configs configs = from_store;
            for (auto& [name, config] : LoadFolder(state.folder)) configs.insert_or_assign(name, std::move(config));
            WriteFolder(configs);
            if (configs != from_store) StoreFromFolder(configs);
            state.store_stamp = StampOf(state.store);
            state.folder_snapshot = SnapshotOf(state.folder);
            Watch();
            state.started = true;
        }
    }

    void Tick() noexcept
    {
        try
        {
            if (!state.started)
            {
                Start();
                return;
            }

            // The cheat saved or deleted a config.
            const Stamp store_now = StampOf(state.store);
            if (store_now != state.store_stamp)
            {
                state.store_stamp = store_now;
                Configs configs;
                if (store_now.exists)
                {
                    const std::optional<std::string> bytes = ReadWholeFile(state.store);
                    if (!bytes.has_value() || !ParseStore(*bytes, configs)) return; // being written, or damaged
                }
                WriteFolder(configs);
                state.folder_snapshot = SnapshotOf(state.folder);
                return;
            }

            // Someone changed the folder.
            if (state.notification == INVALID_HANDLE_VALUE || WaitForSingleObject(state.notification, 0) != WAIT_OBJECT_0) return;
            FindNextChangeNotification(state.notification);
            Snapshot snapshot = SnapshotOf(state.folder);
            if (snapshot == state.folder_snapshot) return; // our own writes
            state.folder_snapshot = std::move(snapshot);
            StoreFromFolder(LoadFolder(state.folder));
        }
        catch (...)
        {
        }
    }
}
