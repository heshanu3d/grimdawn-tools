/* dpyes-ext injector
 *
 * Build this source once for x86 and once for x64. Each injector deliberately
 * injects only a target of the same architecture, which keeps LoadLibraryW
 * pointer widths and the remote thread ABI correct.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

struct Options {
    DWORD pid = 0;
    std::wstring dll_path;
    std::wstring exe_path;
    std::wstring config_path;
    bool allow_launch = true;
    bool dll_from_cli = false;
    bool exe_from_cli = false;
    bool config_from_cli = false;
};

struct LauncherConfig {
    std::wstring game_x86;
    std::wstring game_x64;
    std::wstring dll_x86;
    std::wstring dll_x64;
};

std::wstring last_error_message(DWORD error) {
    wchar_t *buffer = nullptr;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring message = size && buffer ? buffer : L"unknown error";
    if (buffer)
        LocalFree(buffer);
    while (!message.empty() &&
        (message.back() == L'\r' || message.back() == L'\n'))
        message.pop_back();
    return message;
}

std::wstring module_directory(void) {
    std::vector<wchar_t> path(32768);
    DWORD size = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
        return L".";
    std::wstring result(path.data(), size);
    size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

std::wstring full_path(const std::wstring &path) {
    std::vector<wchar_t> buffer(32768);
    DWORD size = GetFullPathNameW(path.c_str(),
        static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    return size && size < buffer.size()
        ? std::wstring(buffer.data(), size) : path;
}

bool file_exists(const std::wstring &path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
        !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring path_directory(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring path_filename(const std::wstring &path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring join_path(const std::wstring &directory,
    const std::wstring &path) {
    if (directory.empty() || directory == L".")
        return path;
    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + path;
    return directory + L"\\" + path;
}

bool path_is_rooted(const std::wstring &path) {
    if (path.empty())
        return false;
    if (path[0] == L'\\' || path[0] == L'/')
        return true;
    return path.size() >= 2 && path[1] == L':';
}

std::wstring expand_environment(const std::wstring &value) {
    DWORD size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!size)
        return value;
    std::vector<wchar_t> buffer(size);
    DWORD written = ExpandEnvironmentStringsW(value.c_str(), buffer.data(),
        static_cast<DWORD>(buffer.size()));
    return written && written <= buffer.size()
        ? std::wstring(buffer.data()) : value;
}

std::wstring resolve_path(const std::wstring &path,
    const std::wstring &base_directory) {
    std::wstring expanded = expand_environment(path);
    return full_path(path_is_rooted(expanded)
        ? expanded : join_path(base_directory, expanded));
}

std::wstring trim(const std::wstring &value) {
    size_t begin = 0;
    while (begin < value.size() && iswspace(value[begin]))
        ++begin;
    size_t end = value.size();
    while (end > begin && iswspace(value[end - 1]))
        --end;
    return value.substr(begin, end - begin);
}

std::wstring unquote(std::wstring value) {
    value = trim(value);
    if (value.size() >= 2 &&
        ((value.front() == L'"' && value.back() == L'"') ||
         (value.front() == L'\'' && value.back() == L'\''))) {
        value = trim(value.substr(1, value.size() - 2));
    }
    return value;
}

bool decode_text(const std::vector<unsigned char> &bytes,
    std::wstring *text) {
    text->clear();
    if (bytes.empty())
        return true;

    if (bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe) {
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            wchar_t ch = static_cast<wchar_t>(bytes[i] |
                (static_cast<unsigned>(bytes[i + 1]) << 8));
            if (ch != 0xfeff)
                text->push_back(ch);
        }
        return true;
    }
    if (bytes.size() >= 2 && bytes[0] == 0xfe && bytes[1] == 0xff) {
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            wchar_t ch = static_cast<wchar_t>(
                (static_cast<unsigned>(bytes[i]) << 8) | bytes[i + 1]);
            if (ch != 0xfeff)
                text->push_back(ch);
        }
        return true;
    }

    size_t offset = bytes.size() >= 3 && bytes[0] == 0xef &&
        bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    const char *data = reinterpret_cast<const char *>(bytes.data() + offset);
    int byte_count = static_cast<int>(bytes.size() - offset);
    if (!byte_count)
        return true;

    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int char_count = MultiByteToWideChar(code_page, flags, data, byte_count,
        nullptr, 0);
    if (!char_count) {
        code_page = CP_ACP;
        flags = 0;
        char_count = MultiByteToWideChar(code_page, flags, data, byte_count,
            nullptr, 0);
    }
    if (!char_count)
        return false;
    text->resize(static_cast<size_t>(char_count));
    return MultiByteToWideChar(code_page, flags, data, byte_count,
        &(*text)[0], char_count) == char_count;
}

bool read_text_file(const std::wstring &path, std::wstring *text,
    std::wstring *error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = last_error_message(GetLastError());
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 1024 * 1024) {
        *error = size.QuadPart > 1024 * 1024
            ? L"configuration file is larger than 1 MiB"
            : last_error_message(GetLastError());
        CloseHandle(file);
        return false;
    }

    std::vector<unsigned char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
            &read, nullptr) && read == bytes.size());
    if (!ok)
        *error = last_error_message(GetLastError());
    CloseHandle(file);
    if (!ok)
        return false;
    if (!decode_text(bytes, text)) {
        *error = L"unsupported text encoding";
        return false;
    }
    return true;
}

bool load_launcher_config(const std::wstring &path, LauncherConfig *config,
    std::wstring *error) {
    std::wstring text;
    if (!read_text_file(path, &text, error))
        return false;

    bool in_launcher = false;
    size_t offset = 0;
    while (offset <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", offset);
        std::wstring line = trim(text.substr(offset,
            end == std::wstring::npos ? std::wstring::npos : end - offset));
        if (!line.empty() && line.front() != L'#' && line.front() != L';') {
            if (line.front() == L'[' && line.back() == L']') {
                std::wstring section = trim(line.substr(1, line.size() - 2));
                in_launcher = _wcsicmp(section.c_str(), L"launcher") == 0;
            } else if (in_launcher) {
                size_t equals = line.find(L'=');
                if (equals != std::wstring::npos) {
                    std::wstring key = trim(line.substr(0, equals));
                    std::wstring value = unquote(line.substr(equals + 1));
                    if (_wcsicmp(key.c_str(), L"game_x86") == 0)
                        config->game_x86 = value;
                    else if (_wcsicmp(key.c_str(), L"game_x64") == 0)
                        config->game_x64 = value;
                    else if (_wcsicmp(key.c_str(), L"dll_x86") == 0)
                        config->dll_x86 = value;
                    else if (_wcsicmp(key.c_str(), L"dll_x64") == 0)
                        config->dll_x64 = value;
                    /* Unknown fields (including injector, wait_settle and
                     * wait_process) are intentionally ignored. */
                }
            }
        }
        if (end == std::wstring::npos)
            break;
        offset = end + 1;
        if (offset < text.size() && text[end] == L'\r' &&
            text[offset] == L'\n')
            ++offset;
    }
    return true;
}

bool same_architecture(HANDLE process) {
    using IsWow64Process2Fn = BOOL (WINAPI *)(HANDLE, USHORT *, USHORT *);
    auto is_wow64_process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (is_wow64_process2) {
        USHORT self_process = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT self_native = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT target_process = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT target_native = IMAGE_FILE_MACHINE_UNKNOWN;
        if (is_wow64_process2(GetCurrentProcess(), &self_process,
                &self_native) &&
            is_wow64_process2(process, &target_process, &target_native)) {
            USHORT self_machine = self_process == IMAGE_FILE_MACHINE_UNKNOWN
                ? self_native : self_process;
            USHORT target_machine =
                target_process == IMAGE_FILE_MACHINE_UNKNOWN
                ? target_native : target_process;
            return self_machine == target_machine;
        }
    }

    BOOL self_wow64 = FALSE;
    BOOL target_wow64 = FALSE;
    if (!IsWow64Process(GetCurrentProcess(), &self_wow64) ||
        !IsWow64Process(process, &target_wow64))
        return false;
    return self_wow64 == target_wow64;
}

DWORD find_matching_process(const std::wstring &expected_exe_path) {
    std::wstring expected_path = expected_exe_path.empty()
        ? L"" : full_path(expected_exe_path);
    std::wstring expected_name = expected_path.empty()
        ? L"Grim Dawn.exe" : path_filename(expected_path);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, expected_name.c_str()) != 0)
                continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, entry.th32ProcessID);
            if (!process)
                continue;

            bool match = same_architecture(process);
            if (match && !expected_path.empty()) {
                std::vector<wchar_t> image_path(32768);
                DWORD image_size = static_cast<DWORD>(image_path.size());
                match = QueryFullProcessImageNameW(process, 0,
                    image_path.data(), &image_size) &&
                    _wcsicmp(full_path(std::wstring(image_path.data(),
                        image_size)).c_str(), expected_path.c_str()) == 0;
            }
            CloseHandle(process);
            if (match) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

uintptr_t remote_module_base(DWORD pid, const wchar_t *module_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    uintptr_t result = 0;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, module_name) == 0) {
                result = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool module_is_loaded(DWORD pid, const std::wstring &dll_path) {
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool loaded = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(full_path(entry.szExePath).c_str(),
                    dll_path.c_str()) == 0) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return loaded;
}

bool inject_dll(DWORD pid, const std::wstring &dll_path) {
    constexpr DWORD access = PROCESS_CREATE_THREAD |
        PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (!process) {
        fwprintf(stderr, L"OpenProcess(%lu) failed: %ls\n", pid,
            last_error_message(GetLastError()).c_str());
        return false;
    }

    if (!same_architecture(process)) {
        fwprintf(stderr,
            L"Architecture mismatch: use the %ls injector for this target.\n",
            sizeof(void *) == 8 ? L"x86" : L"x64");
        CloseHandle(process);
        return false;
    }

    if (module_is_loaded(pid, dll_path)) {
        wprintf(L"Already injected: %ls\n", dll_path.c_str());
        CloseHandle(process);
        return true;
    }

    HMODULE local_kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC local_load_library = GetProcAddress(local_kernel32,
        "LoadLibraryW");
    HMODULE local_owner = nullptr;
    if (!local_kernel32 || !local_load_library ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local_load_library), &local_owner)) {
        fwprintf(stderr, L"Unable to resolve the local LoadLibraryW.\n");
        CloseHandle(process);
        return false;
    }

    wchar_t owner_path[MAX_PATH] = {};
    GetModuleFileNameW(local_owner, owner_path, MAX_PATH);
    const wchar_t *owner_name = wcsrchr(owner_path, L'\\');
    owner_name = owner_name ? owner_name + 1 : owner_path;
    uintptr_t remote_owner = 0;
    for (unsigned attempt = 0; attempt < 300 && !remote_owner; ++attempt) {
        remote_owner = remote_module_base(pid, owner_name);
        if (!remote_owner)
            Sleep(100);
    }
    if (!remote_owner) {
        fwprintf(stderr, L"Unable to locate target module %ls.\n", owner_name);
        CloseHandle(process);
        return false;
    }
    uintptr_t load_library_rva =
        reinterpret_cast<uintptr_t>(local_load_library) -
        reinterpret_cast<uintptr_t>(local_owner);
    auto remote_load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remote_owner + load_library_rva);

    SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    void *remote_path = VirtualAllocEx(process, nullptr, bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote_path) {
        fwprintf(stderr, L"VirtualAllocEx failed: %ls\n",
            last_error_message(GetLastError()).c_str());
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    bool ok = WriteProcessMemory(process, remote_path, dll_path.c_str(), bytes,
        &written) && written == bytes;
    if (!ok) {
        fwprintf(stderr, L"WriteProcessMemory failed: %ls\n",
            last_error_message(GetLastError()).c_str());
    }

    HANDLE thread = nullptr;
    if (ok) {
        thread = CreateRemoteThread(process, nullptr, 0, remote_load_library,
            remote_path, 0, nullptr);
        if (!thread) {
            fwprintf(stderr, L"CreateRemoteThread failed: %ls\n",
                last_error_message(GetLastError()).c_str());
            ok = false;
        }
    }

    if (thread) {
        DWORD wait = WaitForSingleObject(thread, 30000);
        DWORD result = 0;
        if (wait != WAIT_OBJECT_0 || !GetExitCodeThread(thread, &result) ||
            result == 0) {
            fwprintf(stderr, L"Remote LoadLibraryW failed or timed out.\n");
            ok = false;
        }
        CloseHandle(thread);
    }

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
    CloseHandle(process);
    if (ok)
        wprintf(L"Injected %ls into PID %lu.\n", dll_path.c_str(), pid);
    return ok;
}

bool launch_game(const std::wstring &exe_path, PROCESS_INFORMATION *pi) {
    std::wstring command_line = L"\"" + exe_path + L"\"";
    std::vector<wchar_t> mutable_command(command_line.begin(),
        command_line.end());
    mutable_command.push_back(L'\0');
    std::wstring working_dir = exe_path;
    size_t slash = working_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        working_dir.resize(slash);
    else
        working_dir = L".";

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    *pi = {};
    if (!CreateProcessW(exe_path.c_str(), mutable_command.data(), nullptr,
            nullptr, FALSE, 0, nullptr, working_dir.c_str(),
            &startup, pi)) {
        fwprintf(stderr, L"CreateProcessW failed for %ls: %ls\n",
            exe_path.c_str(), last_error_message(GetLastError()).c_str());
        return false;
    }
    return true;
}

void print_usage(const wchar_t *program) {
    wprintf(L"dpyes-ext Grim Dawn injector (%ls)\n\n",
        sizeof(void *) == 8 ? L"x64" : L"x86");
    wprintf(L"Usage: %ls [--config PATH] [--pid PID] [--dll PATH] "
        L"[--exe PATH] [--no-launch]\n\n", program);
    wprintf(L"By default, reads launcher.cfg next to the injector. The %ls "
        L"build uses game_%ls and dll_%ls. Command-line paths override the "
        L"configuration file.\n",
        sizeof(void *) == 8 ? L"x64" : L"x86",
        sizeof(void *) == 8 ? L"x64" : L"x86",
        sizeof(void *) == 8 ? L"x64" : L"x86");
}

bool parse_options(int argc, wchar_t **argv, Options *options) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            options->pid = wcstoul(argv[++i], nullptr, 10);
        } else if (_wcsicmp(argv[i], L"--config") == 0 && i + 1 < argc) {
            options->config_path = argv[++i];
            options->config_from_cli = true;
        } else if (_wcsicmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            options->dll_path = argv[++i];
            options->dll_from_cli = true;
        } else if (_wcsicmp(argv[i], L"--exe") == 0 && i + 1 < argc) {
            options->exe_path = argv[++i];
            options->exe_from_cli = true;
        } else if (_wcsicmp(argv[i], L"--no-launch") == 0) {
            options->allow_launch = false;
        } else if (_wcsicmp(argv[i], L"--help") == 0 ||
            _wcsicmp(argv[i], L"-h") == 0) {
            print_usage(argv[0]);
            ExitProcess(0);
        } else {
            fwprintf(stderr, L"Unknown or incomplete option: %ls\n", argv[i]);
            return false;
        }
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        print_usage(argv[0]);
        return 2;
    }

    const std::wstring directory = module_directory();
    if (options.config_path.empty())
        options.config_path = join_path(directory, L"launcher.cfg");
    else
        options.config_path = full_path(expand_environment(options.config_path));

    LauncherConfig config;
    bool config_loaded = false;
    if (file_exists(options.config_path)) {
        std::wstring error;
        if (!load_launcher_config(options.config_path, &config, &error)) {
            fwprintf(stderr, L"Unable to read configuration %ls: %ls\n",
                options.config_path.c_str(), error.c_str());
            return 3;
        }
        config_loaded = true;
        wprintf(L"Loaded configuration: %ls\n", options.config_path.c_str());
    } else if (options.config_from_cli) {
        fwprintf(stderr, L"Configuration file not found: %ls\n",
            options.config_path.c_str());
        return 3;
    }

    bool exe_path_is_explicit = options.exe_from_cli;
    if (config_loaded) {
        const std::wstring &configured_dll = sizeof(void *) == 8
            ? config.dll_x64 : config.dll_x86;
        const std::wstring &configured_game = sizeof(void *) == 8
            ? config.game_x64 : config.game_x86;
        const std::wstring config_directory =
            path_directory(options.config_path);
        if (!options.dll_from_cli && !configured_dll.empty())
            options.dll_path = resolve_path(configured_dll, config_directory);
        if (!options.exe_from_cli && !configured_game.empty()) {
            options.exe_path = resolve_path(configured_game, config_directory);
            exe_path_is_explicit = true;
        }
    }

    if (options.dll_path.empty()) {
        options.dll_path = join_path(directory,
            sizeof(void *) == 8 ? L"dpyes_ext-x64.dll"
                                 : L"dpyes_ext-x86.dll");
    } else if (options.dll_from_cli) {
        options.dll_path = full_path(expand_environment(options.dll_path));
    }
    options.dll_path = full_path(options.dll_path);
    if (!file_exists(options.dll_path)) {
        fwprintf(stderr, L"DLL not found: %ls\n", options.dll_path.c_str());
        return 4;
    }

    if (options.exe_from_cli)
        options.exe_path = full_path(expand_environment(options.exe_path));

    const std::wstring expected_running_exe = exe_path_is_explicit
        ? options.exe_path : L"";
    DWORD pid = options.pid ? options.pid
                            : find_matching_process(expected_running_exe);
    if (pid)
        return inject_dll(pid, options.dll_path) ? 0 : 5;

    if (!options.allow_launch) {
        if (expected_running_exe.empty()) {
            fwprintf(stderr, L"No matching Grim Dawn process is running.\n");
        } else {
            fwprintf(stderr, L"Configured game process is not running: %ls\n",
                expected_running_exe.c_str());
        }
        return 6;
    }

    if (options.exe_path.empty()) {
        options.exe_path = join_path(directory,
            sizeof(void *) == 8 ? L"x64\\Grim Dawn.exe"
                                 : L"Grim Dawn.exe");
    }
    options.exe_path = full_path(options.exe_path);
    if (!file_exists(options.exe_path)) {
        fwprintf(stderr,
            L"Game executable not found: %ls\n"
            L"Set game_%ls in launcher.cfg, copy the injector to the Grim "
            L"Dawn directory, or use --exe PATH.\n", options.exe_path.c_str(),
            sizeof(void *) == 8 ? L"x64" : L"x86");
        return 7;
    }

    PROCESS_INFORMATION process = {};
    if (!launch_game(options.exe_path, &process))
        return 8;

    /* Let the Windows loader initialize the process before resolving the
     * remote module that owns LoadLibraryW. inject_dll() also polls the
     * module list, so slow game startup is handled without a fixed delay. */
    WaitForInputIdle(process.hProcess, 10000);
    bool injected = inject_dll(process.dwProcessId, options.dll_path);
    if (injected)
        wprintf(L"Started Grim Dawn (PID %lu).\n", process.dwProcessId);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return injected ? 0 : 9;
}
