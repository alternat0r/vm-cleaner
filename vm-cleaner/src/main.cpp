// vm-cleaner.cpp — find VM disk images (any vendor) across drives + registry, log results,
// group them per-VM, and optionally recycle/delete them.
// Builds with MSVC 2022 (/MT static, Unicode). No external dependencies.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

#include "version.h"

// ---------------------------------------------------------------------------
// VM file type table. `core` marks real VM disks/configs (vs. generic images).
// ---------------------------------------------------------------------------
struct VmExtDef { const wchar_t* suffix; const wchar_t* type; bool core; };

static const VmExtDef g_exts[] = {
    { L".vbox-prev", L"VirtualBox (config backup)", true },
    { L".vdi",       L"VirtualBox (VDI disk)", true },
    { L".vbox",      L"VirtualBox (VM config)", true },
    { L".vmdk",      L"VMware (VMDK disk)", true },
    { L".vmx",       L"VMware (VM config)", true },
    { L".vmem",      L"VMware (memory)", true },
    { L".vmsd",      L"VMware (snapshot metadata)", true },
    { L".vmsn",      L"VMware (snapshot)", true },
    { L".vmxf",      L"VMware (config)", true },
    { L".vmss",      L"VMware (suspended state)", true },
    { L".vswp",      L"VMware (swap)", true },
    { L".nvram",     L"VMware/QEMU (NVRAM)", true },
    { L".vmcx",      L"Hyper-V (VM config)", true },
    { L".vmrs",      L"Hyper-V (runtime state)", true },
    { L".vmgs",      L"Hyper-V (guest state)", true },
    { L".avhdx",     L"Hyper-V (differencing disk)", true },
    { L".avhd",      L"Hyper-V (differencing disk)", true },
    { L".vhdx",      L"Hyper-V (VHDX disk)", true },
    { L".vhd",       L"VHD disk (Hyper-V / VirtualBox)", true },
    { L".qcow2",     L"QEMU/KVM (QCOW2 disk)", true },
    { L".qcow",      L"QEMU/KVM (QCOW disk)", true },
    { L".qed",       L"QEMU (QED disk)", true },
    { L".cow",       L"QEMU (COW disk)", true },
    { L".hdd",       L"Parallels (HDD disk)", true },
    { L".hds",       L"Parallels (HDS disk)", true },
    { L".pvm",       L"Parallels (VM bundle)", true },
    { L".pvs",       L"Parallels (config)", true },
    { L".pvi",       L"Parallels (info)", true },
    { L".ova",       L"OVF appliance (importable)", false },
    { L".ovf",       L"OVF descriptor", false },
    { L".dmg",       L"Apple disk image", false },
    { L".img",       L"Raw disk image (QEMU/generic)", false },
    { L".raw",       L"Raw disk image (QEMU/generic)", false },
    { L".dsk",       L"Disk image (generic)", false },
    { L".iso",       L"Optical disk image", false },
    { L".wim",       L"Windows imaging (WIM)", false },
};

// ---------------------------------------------------------------------------
// Archives that may contain VM images. Only flagged when larger than the
// threshold (a VM disk archive is rarely small). core=false -> "loose" group.
// ---------------------------------------------------------------------------
static const wchar_t* g_archiveExts[] = {
    L".zip", L".7z", L".rar", L".tar.gz", L".tgz", L".tar", L".gz",
    L".tar.bz2", L".tbz2", L".tbz", L".tar.xz", L".txz", L".bz2", L".xz",
};
static const wchar_t* g_archiveType = L"Archive (may contain VM image)";
static unsigned int g_minArchiveMB = 200; // size threshold, in MB

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](wchar_t c) { return (wchar_t)towlower(c); });
    return r;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring FormatSize(ULONGLONG bytes) {
    if (bytes < 1024) {
        wchar_t b[64];
        swprintf_s(b, L"%llu B", (unsigned long long)bytes);
        return b;
    }
    const wchar_t* units[] = { L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = (double)bytes;
    int u = -1;
    do { v /= 1024.0; u++; } while (v >= 1024.0 && u < 4);
    wchar_t b[64];
    swprintf_s(b, L"%.2f %s", v, units[u]);
    return b;
}

static std::wstring Join(const std::wstring& d, const std::wstring& n) {
    if (d.empty()) return n;
    if (d.back() == L'\\' || d.back() == L'/') return d + n;
    return d + L"\\" + n;
}

static std::wstring ParentDir(const std::wstring& p) {
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return p;
    if (slash == 2 && p.size() >= 3 && p[1] == L':') return p.substr(0, 3); // drive root
    return p.substr(0, slash);
}

static std::wstring NowStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[64];
    swprintf_s(b, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return b;
}

static std::wstring NowFileStamp() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t b[64];
    swprintf_s(b, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    return b;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(0, slash);
    return p;
}

// ---------------------------------------------------------------------------
// Console output (Unicode-safe for both console and redirected stdout)
// ---------------------------------------------------------------------------
class Output {
    HANDLE m_out;
    bool   m_console;
public:
    Output() : m_out(GetStdHandle(STD_OUTPUT_HANDLE)), m_console(false) {
        DWORD mode;
        m_console = (GetConsoleMode(m_out, &mode) != 0);
    }
    void Print(const std::wstring& s) {
        if (m_console) {
            DWORD written;
            WriteConsoleW(m_out, s.c_str(), (DWORD)s.size(), &written, nullptr);
        } else {
            std::string u8 = ToUtf8(s);
            DWORD written;
            WriteFile(m_out, u8.c_str(), (DWORD)u8.size(), &written, nullptr);
        }
    }
};

// ---------------------------------------------------------------------------
// Logger: mirrors every line to console and to a UTF-8 (BOM) log file.
// ---------------------------------------------------------------------------
class Logger {
    Output m_console;
    HANDLE m_file;
    bool   m_open;
public:
    Logger() : m_file(INVALID_HANDLE_VALUE), m_open(false) {}
    ~Logger() { Close(); }

    bool Open(const std::wstring& path) {
        m_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) return false;
        const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        DWORD w; WriteFile(m_file, bom, 3, &w, nullptr);
        m_open = true;
        return true;
    }
    void Close() {
        if (m_open) { CloseHandle(m_file); m_open = false; }
        m_file = INVALID_HANDLE_VALUE;
    }
    void Log(const std::wstring& line) {
        std::wstring full = line + L"\r\n";
        m_console.Print(full);
        if (m_open) {
            std::string u8 = ToUtf8(full);
            DWORD w; WriteFile(m_file, u8.c_str(), (DWORD)u8.size(), &w, nullptr);
        }
    }
};

// ---------------------------------------------------------------------------
// Scan state
// ---------------------------------------------------------------------------
struct Finding {
    std::wstring path;
    std::wstring dir;
    std::wstring type;
    ULONGLONG    size;
    bool         core;
};
static std::vector<Finding> g_findings;
static ULONGLONG g_dirsScanned = 0;

struct VmMatch { std::wstring type; bool core; };

static VmMatch MatchVmType(const std::wstring& nameLower, ULONGLONG size) {
    for (const auto& e : g_exts) {
        size_t len = wcslen(e.suffix);
        if (nameLower.size() >= len &&
            nameLower.compare(nameLower.size() - len, len, e.suffix) == 0)
            return { e.type, e.core };
    }
    ULONGLONG thresh = (ULONGLONG)g_minArchiveMB * 1024ULL * 1024ULL;
    if (size >= thresh) {
        for (const wchar_t* s : g_archiveExts) {
            size_t len = wcslen(s);
            if (nameLower.size() >= len &&
                nameLower.compare(nameLower.size() - len, len, s) == 0)
                return { g_archiveType, false };
        }
    }
    return { L"", false };
}

static void RecordFinding(Logger& log, const std::wstring& path, ULONGLONG size,
                          const std::wstring& type, bool core) {
    g_findings.push_back({ path, ParentDir(path), type, size, core });
    log.Log(L"  [" + type + L"] " + path + L"  (" + FormatSize(size) + L")");
}

static bool IsSkipDir(const std::wstring& lower, bool isRoot) {
    static const wchar_t* everywhere[] = {
        L"$recycle.bin", L"system volume information", L"$windows.~bt", L"$windows.~ws",
        L"$winreagent", L"recovery", L"perflogs", L"msocache", L"config.msi",
        L"package cache", L"$sysreset",
    };
    for (const auto* s : everywhere) if (lower == s) return true;
    if (isRoot) {
        static const wchar_t* rootOnly[] = { L"windows", L"program files", L"program files (x86)" };
        for (const auto* s : rootOnly) if (lower == s) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Recursive directory scan
// ---------------------------------------------------------------------------
static void ScanDirectory(const std::wstring& dir, Logger& log, bool isRoot) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';
    pattern += L"*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue; // skip junctions/symlinks
            if (IsSkipDir(ToLowerW(name), isRoot)) continue;
            g_dirsScanned++;
            ScanDirectory(Join(dir, name), log, false);
        } else {
            ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (size == 0) continue; // skip empty placeholder/breadcrumb files
            VmMatch m = MatchVmType(ToLowerW(name), size);
            if (!m.type.empty())
                RecordFinding(log, Join(dir, name), size, m.type, m.core);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------------------
// Registry helpers
// ---------------------------------------------------------------------------
static std::wstring RegReadString(HKEY root, const wchar_t* sub, const wchar_t* val,
                                  REGSAM extra) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | extra, &k) != ERROR_SUCCESS) return L"";
    wchar_t buf[2048];
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    LONG r = RegQueryValueExW(k, val, nullptr, &type, (LPBYTE)buf, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return L"";
    size_t len = sz / sizeof(wchar_t);
    while (len > 0 && buf[len - 1] == 0) --len;
    std::wstring s(buf, len);
    if (type == REG_EXPAND_SZ) {
        wchar_t out[2048];
        DWORD need = ExpandEnvironmentStringsW(s.c_str(), out, 2048);
        if (need && need <= 2048) s = out;
    }
    return s;
}

static bool KeyExists(HKEY root, const wchar_t* sub, REGSAM extra) {
    HKEY k;
    LONG r = RegOpenKeyExW(root, sub, 0, KEY_READ | extra, &k);
    if (r == ERROR_SUCCESS) { RegCloseKey(k); return true; }
    return false;
}

struct UninstallEntry { std::wstring display; std::wstring location; };

static void EnumUninstall(HKEY root, const wchar_t* base, REGSAM extra,
                          std::vector<UninstallEntry>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, base, 0, KEY_READ | extra, &k) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; i++) {
        wchar_t name[256];
        DWORD ns = 256;
        if (RegEnumKeyExW(k, i, name, &ns, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        std::wstring sub = std::wstring(base) + L"\\" + name;
        std::wstring disp = RegReadString(root, sub.c_str(), L"DisplayName", extra);
        if (disp.empty()) continue;
        out.push_back({ disp, RegReadString(root, sub.c_str(), L"InstallLocation", extra) });
    }
    RegCloseKey(k);
}

static bool IsVmVendor(const std::wstring& lower) {
    static const wchar_t* kws[] = {
        L"virtualbox", L"vmware", L"hyper-v", L"hyperv", L"qemu", L"parallels",
        L"virtual pc", L"virtualpc", L"virt-manager", L"oracle vm", L"proxmox",
        L"xen", L"virtio", L"citrix hypervisor", L"guest additions", L"vm tools",
        L"windows sandbox", L"android emulator", L"bochs", L"limbo",
    };
    for (const auto* k : kws) if (lower.find(k) != std::wstring::npos) return true;
    return false;
}

static void DetectVmSoftware(Logger& log, std::vector<std::wstring>& extraDirs) {
    bool any = false;
    log.Log(L"");
    log.Log(L"== Detecting installed VM software (registry) ==");

    std::wstring vbFolder = RegReadString(HKEY_CURRENT_USER,
        L"Software\\Oracle\\VirtualBox", L"DefaultMachineFolder", 0);
    if (!vbFolder.empty()) {
        any = true;
        log.Log(L"  VirtualBox detected (HKCU DefaultMachineFolder): " + vbFolder);
        extraDirs.push_back(vbFolder);
    }
    std::wstring vbInst = RegReadString(HKEY_LOCAL_MACHINE,
        L"Software\\Oracle\\VirtualBox", L"InstallDir", 0);
    if (!vbInst.empty()) { any = true; log.Log(L"  VirtualBox InstallDir: " + vbInst); }
    std::wstring vbVer = RegReadString(HKEY_LOCAL_MACHINE,
        L"Software\\Oracle\\VirtualBox", L"Version", 0);
    if (!vbVer.empty()) log.Log(L"  VirtualBox version: " + vbVer);

    std::wstring vmInst = RegReadString(HKEY_LOCAL_MACHINE,
        L"Software\\VMware, Inc.\\VMware Workstation", L"InstallPath", 0);
    if (!vmInst.empty()) { any = true; log.Log(L"  VMware Workstation InstallPath: " + vmInst); }
    std::wstring vmDef = RegReadString(HKEY_CURRENT_USER,
        L"Software\\VMware, Inc.\\VMware Workstation", L"VMware VMs Path", 0);
    if (!vmDef.empty()) {
        any = true;
        log.Log(L"  VMware default VM path: " + vmDef);
        extraDirs.push_back(vmDef);
    }

    bool hv = KeyExists(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization", 0);
    if (hv) { any = true; log.Log(L"  Hyper-V feature detected (Virtualization key present)"); }
    if (GetFileAttributesW(L"C:\\Windows\\System32\\vmms.exe") != INVALID_FILE_ATTRIBUTES) {
        any = true;
        log.Log(L"  Hyper-V Virtual Machine Management service present (vmms.exe)");
    }

    std::vector<UninstallEntry> entries;
    EnumUninstall(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, entries);
    EnumUninstall(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", KEY_WOW64_32KEY, entries);
    EnumUninstall(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0, entries);

    for (const auto& e : entries) {
        if (IsVmVendor(ToLowerW(e.display))) {
            any = true;
            log.Log(L"  Installed: " + e.display +
                    (e.location.empty() ? L"" : L"  @ " + e.location));
            if (!e.location.empty()) extraDirs.push_back(e.location);
        }
    }

    if (!any) log.Log(L"  (no VM software detected in registry)");
}

// ---------------------------------------------------------------------------
// Drive + path scanning
// ---------------------------------------------------------------------------
static void ScanAllDrives(Logger& log, const std::vector<std::wstring>& extraDirs) {
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1 << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;

        log.Log(L"");
        log.Log(L"== Scanning " + std::wstring(root) + L" ==");
        g_dirsScanned = 0;
        ScanDirectory(root, log, true);
        log.Log(L"  (" + std::to_wstring(g_dirsScanned) + L" directories scanned)");
    }

    for (const auto& d : extraDirs) {
        if (d.empty()) continue;
        DWORD attr = GetFileAttributesW(d.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) continue;
        log.Log(L"");
        log.Log(L"== Scanning configured VM folder " + d + L" ==");
        ScanDirectory(d, log, false);
    }
}

// ---------------------------------------------------------------------------
// Grouped reclaimable-space report
// ---------------------------------------------------------------------------
struct GroupInfo {
    std::wstring dir;
    std::vector<Finding> files;
    ULONGLONG bytes = 0;
    bool hasCore = false;
};

static void PrintGroupedReport(Logger& log) {
    std::map<std::wstring, GroupInfo> m;
    for (const auto& f : g_findings) {
        auto& g = m[f.dir];
        g.dir = f.dir;
        g.files.push_back(f);
        g.bytes += f.size;
        if (f.core) g.hasCore = true;
    }
    std::vector<GroupInfo> v;
    v.reserve(m.size());
    for (auto& kv : m) v.push_back(std::move(kv.second));
    std::sort(v.begin(), v.end(),
              [](const GroupInfo& a, const GroupInfo& b) { return a.bytes > b.bytes; });

    log.Log(L"");
    log.Log(L"================================================");
    log.Log(L"SCAN REPORT");
    log.Log(L"================================================");

    log.Log(L"");
    log.Log(L"== VM folders (reclaimable) ==");
    ULONGLONG vmBytes = 0, vmCount = 0;
    for (const auto& g : v) if (g.hasCore) {
        vmBytes += g.bytes;
        vmCount += (ULONGLONG)g.files.size();
        log.Log(L"  " + g.dir + L"   (" + std::to_wstring(g.files.size()) +
                L" file(s), " + FormatSize(g.bytes) + L")");
        for (const auto& f : g.files)
            log.Log(L"      " + f.path + L"  " + FormatSize(f.size));
    }
    if (vmCount == 0) log.Log(L"  (none)");

    log.Log(L"");
    log.Log(L"== Loose disk images & archives (reclaimable) ==");
    ULONGLONG looseBytes = 0, looseCount = 0;
    for (const auto& g : v) if (!g.hasCore) {
        looseBytes += g.bytes;
        looseCount += (ULONGLONG)g.files.size();
        for (const auto& f : g.files)
            log.Log(L"  [" + f.type + L"] " + f.path + L"  (" + FormatSize(f.size) + L")");
    }
    if (looseCount == 0) log.Log(L"  (none)");

    log.Log(L"");
    log.Log(L"  TOTAL reclaimable: " + FormatSize(vmBytes + looseBytes) +
            L" in " + std::to_wstring(g_findings.size()) + L" file(s)");
    log.Log(L"================================================");
}

// ---------------------------------------------------------------------------
// Cleanup: recycle bin or hard delete
// ---------------------------------------------------------------------------
enum class CleanMode { None, Recycle, Delete };

static bool IsFileLocked(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return false; }
    DWORD e = GetLastError();
    return (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION);
}

static bool RecycleFile(const std::wstring& path) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::wstring p = path;
    p.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = p.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

static bool DeleteFileHard(const std::wstring& path) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) != 0;
}

static std::string ReadLineA() {
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return "";
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Status per found file for the CSV report; defaults to "found" until
// cleanup runs and upgrades it to recycled / deleted / skipped / failed.
static std::map<std::wstring, std::string> g_status;

// ---------------------------------------------------------------------------
// CSV report (fleet collection): one row per finding, UTF-8, CRLF.
// Columns: host,vm,group,type,path,size_bytes,status
//   vm    = name of the folder containing the file (the VM folder, if a VM)
//   group = "vm" when the folder holds a core VM file, else "loose"
//   status= found | recycled | deleted | skipped_in_use | failed
// ---------------------------------------------------------------------------
static std::string CsvEscape(const std::wstring& w) {
    std::string u8 = ToUtf8(w);
    if (u8.find_first_of("\",\r\n") == std::string::npos) return u8;
    std::string r = "\"";
    for (char c : u8) { if (c == '"') r += "\"\""; else r += c; }
    r += "\"";
    return r;
}

// ---------------------------------------------------------------------------
// Self-scheduling: register a weekly Task Scheduler task that runs this very
// exe with the same cleanup flags (fleet deployment without external tools).
//   /task        register weekly (Sundays 03:30, interactive or not, on AC+bat)
//   /task:<day>  register weekly on the given day (mon..sun or 1..7)
//   /task:off    unregister
// Requires admin to register machine tasks (schtasks /Create fails otherwise).
// ---------------------------------------------------------------------------
static std::wstring TaskRunCmd(bool assumeLicense, CleanMode mode, bool assumeYes,
                               const std::wstring& csvPath) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L"";
    std::wstring c = L"\"" + std::wstring(exe) + L"\"";
    if (assumeLicense) c += L" /accept";
    if (mode == CleanMode::Recycle) c += L" /recycle";
    else if (mode == CleanMode::Delete) c += L" /delete";
    if (assumeYes) c += L" /yes";
    if (!csvPath.empty()) c += L" /csv:" + csvPath;
    return c;
}

// Launch a helper exe (resolved from PATH) hidden; returns its exit code,
// or -1 if the helper cannot be started.
static int RunHidden(const wchar_t* app, const wchar_t* args) {
    wchar_t appPath[MAX_PATH];
    if (!SearchPathW(nullptr, app, nullptr, MAX_PATH, appPath, nullptr))
        return -1;
    std::wstring cl = std::wstring(appPath) + L" " + args;
    std::vector<wchar_t> cmd(cl.begin(), cl.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return -1;
        return -2;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)rc;
}

static void ScheduleTask(Logger& log, int day, CleanMode mode, bool assumeYes,
                         bool assumeLicense, const std::wstring& csvPath) {
    static const wchar_t* dayNames[] = { L"mon", L"tue", L"wed", L"thu", L"fri", L"sat", L"sun" };
    if (day < 1 || day > 7) day = 7;
    std::wstring dn = dayNames[day - 1];
    std::wstring cmd = TaskRunCmd(assumeLicense, mode, assumeYes, csvPath);
    if (cmd.empty()) { log.Log(L"ERROR: cannot determine executable path."); return; }
    std::wstring schedArgs = L"/create /tn VMCleaner /tr \"" + cmd + L"\" /sc weekly /d " +
                             dn + L" /st 03:30 /f";

    int rc = RunHidden(L"schtasks.exe", schedArgs.c_str());
    if (rc == -1)
        log.Log(L"  ERROR: schtasks.exe not found. Admin rights required to register.");
    else if (rc == -2)
        log.Log(L"  ERROR: could not run schtasks (error " +
                std::to_wstring(GetLastError()) + L"). Admin rights required to register.");
    else if (rc == 0)
        log.Log(L"  Task 'VMCleaner' registered: weekly " + dn + L" 03:30 -> " + cmd);
    else
        log.Log(L"  Task registration failed (schtasks exit " +
                std::to_wstring(rc) + L"). Run from an elevated prompt to register.");
}

static void UnscheduleTask(Logger& log) {
    int rc = RunHidden(L"schtasks.exe", L"/delete /tn VMCleaner /f");
    if (rc == -1)
        log.Log(L"  ERROR: schtasks.exe not found.");
    else if (rc == -2)
        log.Log(L"  ERROR: could not run schtasks (error " +
                std::to_wstring(GetLastError()) + L").");
    else if (rc == 0)
        log.Log(L"  Task 'VMCleaner' removed.");
    else
        log.Log(L"  No 'VMCleaner' task found (or not admin).");
}

static bool WriteCsvReport(const std::wstring& csvPath, Logger& log) {
    HANDLE h = CreateFileW(csvPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        log.Log(L"ERROR: cannot open CSV file: " + csvPath);
        return false;
    }
    auto W = [&](const std::string& s) {
        DWORD w; WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr);
    };
    W("host,vm,group,type,path,size_bytes,status\r\n");
    wchar_t host[256]; DWORD hs = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, host, &hs)) wcscpy_s(host, 256, L"unknown");
    std::string hostS = ToUtf8(host);

    // which directories contain a core VM file?
    std::set<std::wstring> vmDirs;
    for (const auto& f : g_findings) if (f.core) vmDirs.insert(f.dir);

    for (const auto& f : g_findings) {
        std::wstring dirName = f.dir;
        size_t slash = dirName.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dirName = dirName.substr(slash + 1);
        std::string st = "found";
        auto it = g_status.find(f.path);
        if (it != g_status.end()) st = it->second;
        W(hostS + "," + CsvEscape(dirName) + "," +
          (vmDirs.count(f.dir) ? "vm" : "loose") + "," +
          CsvEscape(f.type) + "," + CsvEscape(f.path) + "," +
          std::to_string((unsigned long long)f.size) + "," + st + "\r\n");
    }
    CloseHandle(h);
    log.Log(L"CSV report: " + csvPath);
    return true;
}

// ---------------------------------------------------------------------------
// License gate: user must agree before any task runs.
// Acceptance is persisted under HKCU\Software\VMCleaner (prompted once).
// ---------------------------------------------------------------------------
static const wchar_t* LICENSE_KEY = L"Software\\VMCleaner";

static bool LicenseAccepted() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, LICENSE_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    DWORD v = 0, sz = sizeof(v);
    LONG r = RegQueryValueExW(k, L"LicenseAccepted", nullptr, nullptr, (LPBYTE)&v, &sz);
    RegCloseKey(k);
    return (r == ERROR_SUCCESS && v == 1);
}

static void MarkLicenseAccepted() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, LICENSE_KEY, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = 1;
    RegSetValueExW(k, L"LicenseAccepted", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
}

static void PrintLicense(Output& o) {
    o.Print(L"====================================================\r\n");
    o.Print(L" VM CLEANER - LICENSE AGREEMENT\r\n");
    o.Print(L"====================================================\r\n");
    o.Print(L" 1. This tool finds and removes virtual machine disk\r\n");
    o.Print(L"    images and configuration files.\r\n");
    o.Print(L" 2. /delete is PERMANENT and cannot be undone. Use\r\n");
    o.Print(L"    /recycle when you want recovery.\r\n");
    o.Print(L" 3. Only remove files you own or are authorized to\r\n");
    o.Print(L"    remove on this machine.\r\n");
    o.Print(L" 4. Provided \"as is\", without warranty of any kind.\r\n");
    o.Print(L" 5. The author is not liable for data loss or any\r\n");
    o.Print(L"    damage caused by using this tool.\r\n");
    o.Print(L"====================================================\r\n");
}

static bool LicenseGate(bool assumeLicense) {
    if (LicenseAccepted()) return true;
    Output o;
    PrintLicense(o);
    if (assumeLicense) {
        MarkLicenseAccepted();
        o.Print(L"License accepted via /accept flag.\r\n");
        return true;
    }
    o.Print(L"Do you agree to the above terms? [y/N]: ");
    std::string ans = ReadLineA();
    if (ans != "y" && ans != "Y" && ans != "yes" && ans != "YES") {
        o.Print(L"Aborted - license not accepted. No action was performed.\r\n");
        return false;
    }
    MarkLicenseAccepted();
    o.Print(L"License accepted.\r\n");
    return true;
}

static int PerformCleanup(Logger& log, CleanMode mode, bool assumeYes) {
    if (g_findings.empty()) {
        log.Log(L"Nothing to clean.");
        return 0;
    }
    ULONGLONG totalBytes = 0;
    for (const auto& f : g_findings) totalBytes += f.size;

    if (!assumeYes) {
        Output o;
        if (mode == CleanMode::Delete) {
            o.Print(L"PERMANENTLY DELETE " + std::to_wstring(g_findings.size()) +
                    L" file(s) (" + FormatSize(totalBytes) + L")? Type YES to confirm: ");
            std::string ans = ReadLineA();
            if (ans != "YES" && ans != "yes") { log.Log(L"  Aborted (no confirmation)."); return 0; }
        } else {
            o.Print(L"Move " + std::to_wstring(g_findings.size()) +
                    L" file(s) (" + FormatSize(totalBytes) + L") to Recycle Bin? [y/N]: ");
            std::string ans = ReadLineA();
            if (ans != "y" && ans != "Y" && ans != "yes" && ans != "YES") {
                log.Log(L"  Aborted (no confirmation).");
                return 0;
            }
        }
    }

    log.Log(L"");
    log.Log(L"== Cleaning up " + std::to_wstring(g_findings.size()) + L" file(s) ==");

    ULONGLONG done = 0, skipped = 0, failed = 0;
    for (const auto& f : g_findings) {
        if (IsFileLocked(f.path)) {
            skipped++;
            g_status[f.path] = "skipped_in_use";
            log.Log(L"  [skipped: in use] " + f.path);
            continue;
        }
        bool ok = (mode == CleanMode::Recycle) ? RecycleFile(f.path) : DeleteFileHard(f.path);
        if (ok) {
            done++;
            g_status[f.path] = (mode == CleanMode::Recycle) ? "recycled" : "deleted";
            log.Log(L"  [removed] " + f.path);
        } else {
            failed++;
            g_status[f.path] = "failed";
            log.Log(L"  [FAILED] " + f.path);
        }
    }

    log.Log(L"");
    log.Log(L"  Cleanup complete: " + std::to_wstring(done) + L" removed, " +
            std::to_wstring(skipped) + L" skipped (in use), " +
            std::to_wstring(failed) + L" failed");
    return (int)failed;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void PrintUsage(Output& out) {
    out.Print(L"VM Cleaner v" VERSION_STRING L" - find & clean VM disk images (any vendor)\r\n");
    out.Print(L"Usage:\r\n");
    out.Print(L"  vmcleaner.exe                 scan (dry-run): all drives + registry\r\n");
    out.Print(L"  vmcleaner.exe <path>          scan a single folder only\r\n");
    out.Print(L"  vmcleaner.exe /recycle        move found VM files to Recycle Bin\r\n");
    out.Print(L"  vmcleaner.exe /delete         permanently delete found VM files\r\n");
    out.Print(L"  vmcleaner.exe /yes            skip the confirmation prompt (works with /recycle and /delete)\r\n");
    out.Print(L"  vmcleaner.exe /accept         record license agreement without prompting\r\n");
    out.Print(L"  vmcleaner.exe /log:<path>     write log to a specific file\r\n");
    out.Print(L"  vmcleaner.exe /minsize:<MB>   archive size threshold (default 200 MB)\r\n");
    out.Print(L"  vmcleaner.exe /noregistry     skip registry detection\r\n");
    out.Print(L"  vmcleaner.exe /csv:<path>     also write a CSV report\r\n");
    out.Print(L"                              columns: host,vm,group,type,path,size_bytes,status\r\n");
    out.Print(L"  vmcleaner.exe /task[=<day>]  self-schedule a weekly cleanup (mon..sun, default sun)\r\n");
    out.Print(L"  vmcleaner.exe /task:off      remove the self-scheduled task\r\n");
    out.Print(L"\r\nExit codes: 0 = ok   1 = error   2 = license declined   3 = cleanup failure(s)\r\n");
    out.Print(L"\r\nExamples:\r\n");
    out.Print(L"  vmcleaner.exe /delete                      delete after typing YES\r\n");
    out.Print(L"  vmcleaner.exe D:\\VMs /recycle /yes          recycle a folder without prompt\r\n");
    out.Print(L"  vmcleaner.exe /recycle /yes /accept /csv:C:\\fleet\\r.csv\r\n");
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t* argv[]) {
    CleanMode mode = CleanMode::None;
    bool assumeYes = false;
    bool assumeLicense = false;
    bool skipRegistry = false;
    std::wstring customPath;
    std::wstring logOverride;
    std::wstring csvOverride;
    int taskOp = 0; // 0=off(no action)  1..7=register on that day  -1=unregister

    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a.rfind(L"/log:", 0) == 0)                 logOverride = a.substr(5);
        else if (a.rfind(L"/minsize:", 0) == 0) {
            unsigned v = (unsigned)wcstoul(a.c_str() + 9, nullptr, 10);
            if (v) g_minArchiveMB = v;
        }
        else if (a.rfind(L"/csv:", 0) == 0)            csvOverride = a.substr(5);
        else if (a.rfind(L"/task:", 0) == 0) {
            std::wstring d = ToLowerW(a.substr(6));
            if (d == L"off") taskOp = -1;
            else {
                static const wchar_t* dn[] = { L"mon", L"tue", L"wed", L"thu", L"fri", L"sat", L"sun" };
                taskOp = 7;
                for (int k = 0; k < 7; k++) if (d == dn[k]) { taskOp = k + 1; break; }
                if (d.size() == 1 && d[0] >= L'1' && d[0] <= L'7') taskOp = (int)(d[0] - L'0');
            }
        }
        else if (a == L"/task" || a == L"--task")      taskOp = 7;
        else if (a == L"/noregistry" || a == L"--noregistry") skipRegistry = true;
        else if (a == L"/recycle" || a == L"--recycle")      mode = CleanMode::Recycle;
        else if (a == L"/delete"  || a == L"--delete")       mode = CleanMode::Delete;
        else if (a == L"/yes" || a == L"--yes" || a == L"-y") assumeYes = true;
        else if (a == L"/accept" || a == L"--accept")        assumeLicense = true;
        else if (a == L"/?" || a == L"--help" || a == L"-h" || a == L"-help") {
            Output o; PrintUsage(o); return 0;
        }
        else if (!a.empty() && a[0] != L'/') customPath = a;
    }

    // License must be agreed before any task (scan or cleanup) runs.
    if (!LicenseGate(assumeLicense)) return 2;

    // /task:off is self-contained (no scan): unregister and exit.
    if (taskOp == -1) {
        std::wstring lp = logOverride;
        if (lp.empty()) {
            wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
            lp = std::wstring(cwd) + L"\\vmcleaner_" + NowFileStamp() + L".log";
        }
        Logger lg;
        if (lg.Open(lp)) {
            lg.Log(L"VM Cleaner v" VERSION_STRING L" - removing self-scheduled task");
            UnscheduleTask(lg);
            lg.Log(L"Log file: " + lp);
            lg.Close();
        }
        return 0;
    }

    // Absolute-ize the custom path so findings are absolute (required for SHFileOperation).
    if (!customPath.empty()) {
        wchar_t abs[MAX_PATH];
        if (GetFullPathNameW(customPath.c_str(), MAX_PATH, abs, nullptr))
            customPath = abs;
    }

    std::wstring logPath = logOverride;
    if (logPath.empty()) {
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        logPath = std::wstring(cwd) + L"\\vmcleaner_" + NowFileStamp() + L".log";
    }

    Logger log;
    if (!log.Open(logPath)) {
        Output o;
        o.Print(L"ERROR: cannot open log file: " + logPath + L"\r\n");
        return 1;
    }

    log.Log(L"VM Cleaner v" VERSION_STRING L" - VM disk image finder");
    log.Log(L"Scan started: " + NowStamp());
    log.Log(L"Archive threshold: >= " + std::to_wstring(g_minArchiveMB) +
            L" MB (.zip .7z .rar .tar.gz .tgz .tar .gz .bz2 .xz .tbz2 .txz)");

    if (!customPath.empty()) {
        log.Log(L"Target path: " + customPath);
        DWORD attr = GetFileAttributesW(customPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            log.Log(L"");
            log.Log(L"== Scanning " + customPath + L" ==");
            g_dirsScanned = 0;
            ScanDirectory(customPath, log, false);
            log.Log(L"  (" + std::to_wstring(g_dirsScanned) + L" directories scanned)");
        } else if (attr != INVALID_FILE_ATTRIBUTES) {
            WIN32_FILE_ATTRIBUTE_DATA fad;
            ULONGLONG size = 0;
            if (GetFileAttributesExW(customPath.c_str(), GetFileExInfoStandard, &fad))
                size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            VmMatch m = MatchVmType(ToLowerW(customPath), size);
            if (!m.type.empty())
                RecordFinding(log, customPath, size, m.type, m.core);
            else
                log.Log(L"  (not a VM image file)");
        } else {
            log.Log(L"ERROR: path not found: " + customPath);
        }
    } else {
        std::vector<std::wstring> extraDirs;
        if (!skipRegistry) DetectVmSoftware(log, extraDirs);
        ScanAllDrives(log, extraDirs);
    }

    log.Log(L"Scan finished: " + NowStamp());
    PrintGroupedReport(log);

    if (taskOp >= 1) ScheduleTask(log, taskOp, mode, assumeYes, assumeLicense, csvOverride);

    int failed = 0;
    if (mode != CleanMode::None)
        failed = PerformCleanup(log, mode, assumeYes);

    if (!csvOverride.empty())
        WriteCsvReport(csvOverride, log);

    log.Log(L"Log file: " + logPath);
    log.Close();

    Output o;
    o.Print(L"\r\nLog written to: " + logPath + L"\r\n");
    return (failed > 0) ? 3 : 0;
}
