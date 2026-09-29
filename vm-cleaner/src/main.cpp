// vm-cleaner.cpp — find VM disk images (any vendor) across drives + registry, log results,
// group them per-VM, and optionally recycle/delete them.
// Builds with MSVC 2022 (/MT static, Unicode). No external dependencies.
// Licensed under the GNU General Public License v3.0 (see LICENSE).
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
#pragma comment(lib, "version.lib")

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

// 64-bit FILETIME value (u64), from a FILETIME or a WIN32_FILE_ATTRIBUTE_DATA.
static ULONGLONG Ftime64(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------------------
// User-supplied custom file extensions (/ext:). Each is matched as a core
// VM file (grouped as a VM folder) and listed in the log.
// ---------------------------------------------------------------------------
static std::vector<std::wstring> g_customExts;   // lowercased, with leading dot
static std::wstring g_customExtsArg;             // accepted tokens, joined (for logging)

static bool IsExtCharW(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z'); }

// Normalize one /ext: token to a lowercased suffix starting with '.'.
// Returns false if the token is empty or invalid.
static bool NormalizeExtToken(const std::wstring& t, std::wstring& out) {
    std::wstring s = ToLowerW(t);
    size_t a = s.find_first_not_of(L" \t\r\n,");
    size_t b = s.find_last_not_of(L" \t\r\n,");
    if (a == std::wstring::npos || b < a) return false;   // empty token
    s = s.substr(a, b - a + 1);
    if (s[0] == L'.') s.erase(0, 1);                       // dot optional in input
    if (s.empty() || s.size() > 24) return false;
    for (wchar_t c : s)
        if (!IsExtCharW(c)) return false;
    out = L"." + s;
    return true;
}

// Parse a comma-separated /ext: value; appends normalized suffixes to `exts`.
// Returns the number of valid tokens added.
static int ParseCustomExts(const std::wstring& value, std::vector<std::wstring>& exts) {
    int n = 0;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(L',', start);
        if (comma == std::wstring::npos) comma = value.size();
        if (comma > start) {
            std::wstring norm;
            if (NormalizeExtToken(value.substr(start, comma - start), norm)) { exts.push_back(norm); n++; }
        }
        start = comma + 1;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Vendor filter (/only:). A finding's vendor is derived from the type label
// the user already sees (e.g. "VMware (VMDK disk)" -> vmware). Empty or "all"
// means no filtering (the default).
// ---------------------------------------------------------------------------
static std::wstring g_onlyVendor; // normalized vendor id, or "all"
static ULONGLONG g_maxAgeDays = 0; // /maxage: protect folders modified within N days (0 = off)

static std::wstring VendorOf(const std::wstring& type) {
    std::wstring t = ToLowerW(type);
    auto starts = [&](const wchar_t* p) {
        size_t n = wcslen(p);
        return t.size() >= n && t.compare(0, n, p) == 0;
    };
    if (starts(L"virtualbox"))           return L"virtualbox";
    if (starts(L"vmware"))               return L"vmware";
    if (starts(L"hyper-v"))              return L"hyperv";
    if (starts(L"qemu"))                 return L"qemu";
    if (starts(L"parallels"))            return L"parallels";
    if (starts(L"ovf"))                  return L"ovf";
    if (starts(L"apple"))                return L"apple";
    if (starts(L"vhd disk"))             return L"hyperv";   // .vhd: "VHD disk (Hyper-V / VirtualBox)"
    if (starts(L"raw disk image"))       return L"generic";
    if (starts(L"disk image"))           return L"generic";
    if (starts(L"windows imaging"))      return L"wim";
    if (starts(L"archive"))              return L"archive";
    if (starts(L"custom"))               return L"custom";
    return L"other";
}

// Normalize a user-supplied vendor token. Returns "" when unknown.
static std::wstring NormalizeVendor(const std::wstring& in) {
    std::wstring s = ToLowerW(in);
    std::wstring r;                        // strip spaces/hyphens: "hyper-v" == "hyperv"
    for (wchar_t c : s)
        if (c != L' ' && c != L'\t' && c != L'-') r.push_back(c);
    if (r.empty()) return L"all";
    static const wchar_t* known[] = {
        L"all", L"virtualbox", L"vmware", L"hyperv", L"qemu",
        L"parallels", L"ovf", L"apple", L"wim", L"generic",
        L"archive", L"custom", L"other",
    };
    for (auto k : known) if (r == k) return r;
    return L"";                            // unknown
}

static bool VendorAllowed(const std::wstring& type) {
    if (g_onlyVendor.empty() || g_onlyVendor == L"all") return true;
    return VendorOf(type) == g_onlyVendor;
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
    ULONGLONG    mtime; // last write time as FILETIME (u64)
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
    for (const auto& s : g_customExts) {
        size_t len = s.size();
        if (nameLower.size() >= len &&
            nameLower.compare(nameLower.size() - len, len, s.c_str()) == 0)
            return { L"Custom (" + s.substr(1) + L")", true };
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
                          const std::wstring& type, bool core, ULONGLONG mtime = 0) {
    if (!VendorAllowed(type)) return;   // /only: vendor filter
    g_findings.push_back({ path, ParentDir(path), type, size, core, mtime });
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
                RecordFinding(log, Join(dir, name), size, m.type, m.core, Ftime64(fd.ftLastWriteTime));
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

// ---------------------------------------------------------------------------
// PE version info (user information only): file/product version + copyright.
// ---------------------------------------------------------------------------
struct PeVer { std::wstring fileVer; std::wstring prodVer; std::wstring copyright; };

static std::wstring VerString(const void* base, const wchar_t* key) {
    wchar_t* out = nullptr;
    UINT len = 0;
    if (VerQueryValueW(base, key, (void**)&out, &len) && len) {
        // Some resources report len that includes the NUL terminator; stop at first NUL.
        size_t n = 0;
        while (n < len && out[n] != L'\0') n++;
        return std::wstring(out, n);
    }
    return L"";
}

static PeVer ReadPeVersion(const std::wstring& path) {
    PeVer v;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (!size) return v;
    std::vector<char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return v;
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT rl = 0;
    if (VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &rl) && rl >= sizeof(VS_FIXEDFILEINFO)) {
        wchar_t b[32];
        swprintf_s(b, L"%u.%u.%u.%u",
            (ffi->dwFileVersionMS >> 16) & 0xFFFF, ffi->dwFileVersionMS & 0xFFFF,
            (ffi->dwFileVersionLS >> 16) & 0xFFFF, ffi->dwFileVersionLS & 0xFFFF);
        v.fileVer = b;
        swprintf_s(b, L"%u.%u.%u.%u",
            (ffi->dwProductVersionMS >> 16) & 0xFFFF, ffi->dwProductVersionMS & 0xFFFF,
            (ffi->dwProductVersionLS >> 16) & 0xFFFF, ffi->dwProductVersionLS & 0xFFFF);
        v.prodVer = b;
    }
    const WORD* trans = nullptr;
    UINT tl = 0;
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&trans, &tl) &&
        tl >= 4) {
        wchar_t base[32];
        swprintf_s(base, L"\\StringFileInfo\\%04x%04x\\", trans[0], trans[1]);
        wchar_t key[128];
        swprintf_s(key, L"%sLegalCopyright", base);
        v.copyright = VerString(buf.data(), key);
        swprintf_s(key, L"%sProductVersion", base);
        std::wstring pv = VerString(buf.data(), key);
        if (!pv.empty()) v.prodVer = pv;   // string product version wins over numeric
    }
    return v;
}

// Primary executables to probe (in order) for a given install directory.
static const wchar_t* const g_vmExes[] = {
    L"VirtualBox.exe", L"vmware.exe", L"vmrun.exe", L"vmms.exe",
    L"qemu-system-x86_64.exe", L"qemu-system-i386.exe", L"prlctl.exe",
    L"vboxmanage.exe", L"vmware-vmx.exe",
};

// Locate the primary exe under `dir` (preferring `exeHint`) and log its
// version + copyright. Informational only.
static void LogSoftwareVersion(Logger& log, const std::wstring& label,
                               const std::wstring& dir, const std::wstring& exeHint) {
    if (dir.empty()) return;
    std::wstring exePath;
    if (!exeHint.empty()) {
        std::wstring p = Join(dir, exeHint);
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) exePath = p;
    }
    if (exePath.empty()) {
        for (const auto* c : g_vmExes) {
            std::wstring p = Join(dir, c);
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { exePath = p; break; }
        }
    }
    if (exePath.empty()) return;
    PeVer v = ReadPeVersion(exePath);
    if (v.fileVer.empty() && v.copyright.empty()) return;
    log.Log(L"  " + label + L" version: " + (v.fileVer.empty() ? v.prodVer : v.fileVer) +
            (v.copyright.empty() ? L"" : L"  [copyright: " + v.copyright + L"]") +
            L"  (" + exePath + L")");
}

struct UninstallEntry { std::wstring display; std::wstring location; std::wstring version; };

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
        out.push_back({ disp,
                        RegReadString(root, sub.c_str(), L"InstallLocation", extra),
                        RegReadString(root, sub.c_str(), L"DisplayVersion", extra) });
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
    if (!vbVer.empty())
        log.Log(L"  VirtualBox version (registry): " + vbVer + L"  [for user information]");
    LogSoftwareVersion(log, L"VirtualBox", vbInst, L"VirtualBox.exe");

    std::wstring vmInst = RegReadString(HKEY_LOCAL_MACHINE,
        L"Software\\VMware, Inc.\\VMware Workstation", L"InstallPath", 0);
    if (!vmInst.empty()) { any = true; log.Log(L"  VMware Workstation InstallPath: " + vmInst); }
    LogSoftwareVersion(log, L"VMware Workstation", vmInst, L"vmware.exe");
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
        LogSoftwareVersion(log, L"Hyper-V (vmms.exe)", L"C:\\Windows\\System32", L"vmms.exe");
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
                    (e.version.empty() ? L"" : L"  [version " + e.version + L"]") +
                    (e.location.empty() ? L"" : L"  @ " + e.location) +
                    L"  [for user information]");
            if (!e.location.empty()) {
                extraDirs.push_back(e.location);
                LogSoftwareVersion(log, e.display, e.location, L"");
            }
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
// Age filter (/maxage:<days>). Folder-level: a VM folder is protected while
// ANY of its files was modified within the last N days - so a VM still in use
// is never partially cleaned. All findings in protected folders are logged
// and removed (grouping, cleanup and CSV then ignore them). 0 disables.
// ---------------------------------------------------------------------------
static void ApplyAgeFilter(Logger& log) {
    ULONGLONG days = g_maxAgeDays;
    if (days == 0) return;

    // newest last-write time per folder
    std::map<std::wstring, ULONGLONG> newest;
    for (const auto& f : g_findings)
        if (f.mtime > newest[f.dir]) newest[f.dir] = f.mtime;

    // FILETIME (100ns ticks since 1601) -> seconds, relative to now
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG now64 = Ftime64(now);
    const ULONGLONG tickPerSec = 10000000ULL;
    ULONGLONG cutoffAgeSec = days * 24 * 60 * 60;

    // folders whose newest file is still within the cutoff are protected
    std::set<std::wstring> protectedDirs;
    for (const auto& kv : newest) {
        ULONGLONG ft64 = kv.second;
        if (ft64 < now64) {
            ULONGLONG ageSec = (now64 - ft64) / tickPerSec;
            if (ageSec < cutoffAgeSec) protectedDirs.insert(kv.first);
        }
    }
    if (protectedDirs.empty()) return;

    size_t kept = 0, excluded = 0;
    std::vector<Finding> nf;
    nf.reserve(g_findings.size());
    for (const auto& f : g_findings) {
        if (protectedDirs.count(f.dir)) { excluded++; continue; }
        nf.push_back(f);
        kept++;
    }
    for (const auto& d : protectedDirs)
        log.Log(L"  [skipped] " + d + L"  (modified within " +
                std::to_wstring(days) + L" day(s) - protected by /maxage)");
    log.Log(L"Age filter: " + std::to_wstring(kept) + L" file(s) kept, " +
            std::to_wstring(excluded) + L" file(s) excluded in " +
            std::to_wstring(protectedDirs.size()) + L" folder(s)");
    g_findings = std::move(nf);
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
//   /task:status show the registered task (read-only, no admin/license)
//   /task:off    unregister
// Requires admin to register machine tasks (schtasks /Create fails otherwise).
// ---------------------------------------------------------------------------
static std::wstring TaskRunCmd(bool assumeLicense, CleanMode mode, bool assumeYes,
                               const std::wstring& csvPath, const std::wstring& extArg,
                               const std::wstring& onlyArg, unsigned long long maxAgeDays,
                               const std::wstring& path, bool allDrives) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L"";
    std::wstring c = L"\"" + std::wstring(exe) + L"\"";
    if (assumeLicense) c += L" /accept";
    if (mode == CleanMode::Recycle) c += L" /recycle";
    else if (mode == CleanMode::Delete) c += L" /delete";
    if (assumeYes) c += L" /yes";
    if (!csvPath.empty()) c += L" /csv:" + csvPath;
    if (!extArg.empty()) c += L" /ext:" + extArg;
    if (!onlyArg.empty()) c += L" /only:" + onlyArg;
    if (maxAgeDays > 0) c += L" /maxage:" + std::to_wstring(maxAgeDays);
    if (!path.empty()) c += L" \"" + path + L"\"";
    else if (mode != CleanMode::None && allDrives) c += L" /alldrives";
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

// Same as RunHidden, but captures the helper's stdout into out. Uses a temp
// file (not an anonymous pipe): a pipe can deadlock if a grandchild inherits
// the write end and holds it open after the direct child exits, whereas a
// file is read only after the child has fully terminated. Returns the exit
// code, -1 = not found, -2 = launch failed.
static int RunCapture(const wchar_t* app, const wchar_t* args, std::wstring& out) {
    out.clear();
    wchar_t appPath[MAX_PATH];
    if (!SearchPathW(nullptr, app, nullptr, MAX_PATH, appPath, nullptr))
        return -1;
    std::wstring cl = std::wstring(appPath) + L" " + args;
    std::vector<wchar_t> cmd(cl.begin(), cl.end());
    cmd.push_back(L'\0');

    wchar_t tmpdir[MAX_PATH], tmp[MAX_PATH + 64];
    GetTempPathW(MAX_PATH, tmpdir);
    wsprintf(tmp, L"%shc_%.4x_%.4x.bin", tmpdir,
             (unsigned)GetCurrentProcessId(), (unsigned)GetTickCount());
    HANDLE hf = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return -2;
    SetHandleInformation(hf, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    HANDLE hNul = CreateFileW(L"NUL", 0, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hf; si.hStdError = hf;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hf); if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        DeleteFileW(tmp);
        return -2;
    }
    CloseHandle(hf);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    // Wait for the child to fully exit BEFORE reading, then read the file.
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD rc = 0; GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    HANDLE r = CreateFileW(tmp, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (r != INVALID_HANDLE_VALUE) {
        std::string bytes; char b[8192]; DWORD n;
        while (ReadFile(r, b, sizeof(b), &n, nullptr) && n > 0) bytes.append(b, n);
        CloseHandle(r);
        int wl = MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
        if (wl > 0) {
            out.resize((size_t)wl);
            MultiByteToWideChar(CP_OEMCP, 0, bytes.data(), (int)bytes.size(), &out[0], wl);
        }
    }
    DeleteFileW(tmp);
    return (int)rc;
}

static void ScheduleTask(Logger& log, int day, CleanMode mode, bool assumeYes,
                         bool assumeLicense, const std::wstring& csvPath,
                         const std::wstring& extArg, const std::wstring& onlyArg,
                         unsigned long long maxAgeDays, const std::wstring& path,
                         bool allDrives) {
    static const wchar_t* dayNames[] = { L"mon", L"tue", L"wed", L"thu", L"fri", L"sat", L"sun" };
    if (day < 1 || day > 7) day = 7;
    std::wstring dn = dayNames[day - 1];
    std::wstring cmd = TaskRunCmd(assumeLicense, mode, assumeYes, csvPath, extArg, onlyArg, maxAgeDays, path, allDrives);
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

// ---------------------------------------------------------------------------
// /task:status - read-only fleet audit of the self-scheduled task (no admin
// required). Prints the task's run command, schedule and next run, or a clear
// "not registered" line. Uses schtasks /xml (locale-stable tag names) for the
// definition and /fo CSV (locale-stable English headers) for the next run.
// Returns 1 when the task is not registered (useful for fleet scripts).
// ---------------------------------------------------------------------------
static int StatusTask(Logger& log) {
    std::wstring xml;
    int rc = RunCapture(L"schtasks.exe", L"/query /tn VMCleaner /xml", xml);
    if (rc != 0) {
        log.Log(L"  Task 'VMCleaner' is NOT registered (vmcleaner.exe /task[=<day>] to create it).");
        return 1;
    }
    auto tag = [&](const wchar_t* t) -> std::wstring {
        std::wstring tn = t;
        std::wstring open = L"<" + tn + L">", close = L"</" + tn + L">";
        size_t a = xml.find(open);
        if (a == std::wstring::npos) return L"";
        a += open.size();
        size_t b = xml.find(close, a);
        if (b == std::wstring::npos) return L"";
        return xml.substr(a, b - a);
    };
    std::wstring cmd  = tag(L"Command");
    std::wstring args = tag(L"Arguments");
    std::wstring when = tag(L"StartBoundary");
    std::wstring wd   = tag(L"WorkingDirectory");
    log.Log(L"  Task 'VMCleaner': registered");
    if (!cmd.empty())
        log.Log(L"  Run: " + cmd + (args.empty() ? L"" : L" " + args));
    if (!when.empty())
        log.Log(L"  Schedule: weekly @ " + when.substr(11, 5) +
                L" (ISO " + when + L")");
    if (!wd.empty())
        log.Log(L"  Working dir: " + wd);

    // /fo CSV: header row is fixed English; field 2 of the data row is the
    // next run time. Values are locale-formatted but the position is stable.
    std::wstring csv;
    int rc2 = RunCapture(L"schtasks.exe", L"/query /tn VMCleaner /fo CSV", csv);
    if (rc2 == 0) {
        // Two lines: header ("Task Name,Next Run Time,Status") + data row.
        // Take the second line; split on commas (fields may be quoted).
        std::wstring row;
        size_t nl1 = csv.find(L'\n');
        if (nl1 != std::wstring::npos) {
            size_t nl2 = csv.find(L'\n', nl1 + 1);
            row = csv.substr(nl1 + 1, (nl2 == std::wstring::npos ? csv.size() : nl2) - nl1 - 1);
        } else {
            row = csv; // single line (defensive)
        }
        // strip trailing CR
        while (!row.empty() && (row.back() == L'\r' || row.back() == L'\n')) row.pop_back();
        if (!row.empty()) {
            std::vector<std::wstring> f;
            std::wstring cur;
            bool inQ = false;
            for (size_t i = 0; i < row.size(); i++) {
                wchar_t ch = row[i];
                if (inQ) {
                    if (ch == L'"') {
                        if (i + 1 < row.size() && row[i + 1] == L'"') { cur += L'"'; i++; }
                        else inQ = false;
                    } else cur += ch;
                } else if (ch == L'"') inQ = true;
                else if (ch == L',') { f.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            f.push_back(cur);
            // fields: 0=TaskName, 1=Next Run Time, 2=Status
            if (f.size() >= 2 && f[1].size() > 3 && f[1] != L"N/A")
                log.Log(L"  Next run: " + f[1]);
        }
    }
    return 0;
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
    o.Print(L"\r\n");
    o.Print(L" Licensed under the GNU General Public License v3.0\r\n");
    o.Print(L" https://www.gnu.org/licenses/gpl-3.0.html\r\n");
    o.Print(L"\r\n");
    o.Print(L" DISCLAIMER: This tool is provided \"as is\", without any\r\n");
    o.Print(L"    warranty. The author accepts no liability for data loss,\r\n");
    o.Print(L"    corruption, or any other damage caused by its use.\r\n");
    o.Print(L"    Always review the report (run a dry-run first) before\r\n");
    o.Print(L"    deleting anything.\r\n");
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

// Is the current process running with elevated (administrator) rights?
static bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te = {};
        DWORD sz = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz))
            elevated = (te.TokenIsElevated != 0);
        CloseHandle(tok);
    }
    return elevated;
}

// Warn before any permanent deletion: irreversibility + admin requirement
// + disclaimer. Goes to console AND log (unattended runs see it too).
static void PrintDeleteWarning(Logger& log) {
    log.Log(L"WARNING: /delete is PERMANENT - deleted files cannot be recovered.");
    if (!IsElevated())
        log.Log(L"WARNING: not running as administrator. Deleting files in system-protected locations (e.g. C:\\ProgramData, Windows) or files owned by other users requires administrator rights; such files may fail to delete. Re-run elevated for full coverage.");
    log.Log(L"DISCLAIMER: The author accepts no liability for data loss or damage caused by this tool.");
}

static int PerformCleanup(Logger& log, CleanMode mode, bool assumeYes) {
    if (g_findings.empty()) {
        log.Log(L"Nothing to clean.");
        return 0;
    }
    ULONGLONG totalBytes = 0;
    for (const auto& f : g_findings) totalBytes += f.size;

    if (mode == CleanMode::Delete)
        PrintDeleteWarning(log);

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
    out.Print(L"  vmcleaner.exe /delete         permanently delete found VM files (admin needed for protected files)\r\n");
    out.Print(L"  vmcleaner.exe /yes            skip the confirmation prompt (works with /recycle and /delete)\r\n");
    out.Print(L"  vmcleaner.exe /accept         record license agreement without prompting\r\n");
    out.Print(L"  vmcleaner.exe /log:<path>     write log to a specific file\r\n");
    out.Print(L"  vmcleaner.exe /minsize:<MB>   archive size threshold (default 200 MB)\r\n");
    out.Print(L"  vmcleaner.exe /ext:<a,b,...>  also match these custom file extensions\r\n");
    out.Print(L"  vmcleaner.exe /only:<vendor>  only report/clean one vendor\r\n");
    out.Print(L"                              (all, virtualbox, vmware, hyperv, qemu,\r\n");
    out.Print(L"                               parallels, ovf, apple, generic, archive,\r\n");
    out.Print(L"                               wim, custom, other) - default all\r\n");
    out.Print(L"  vmcleaner.exe /maxage:<days>  ignore VM folders modified within the last\r\n");
    out.Print(L"                              N days (protects active VMs; default off)\r\n");
    out.Print(L"  vmcleaner.exe /noregistry     skip registry detection\r\n");
    out.Print(L"  vmcleaner.exe /csv:<path>     also write a CSV report\r\n");
    out.Print(L"                              columns: host,vm,group,type,path,size_bytes,status\r\n");
    out.Print(L"  vmcleaner.exe /task[=<day>]  self-schedule a weekly cleanup (mon..sun, default sun)\r\n");
    out.Print(L"  vmcleaner.exe /task:off      remove the self-scheduled task\r\n");
    out.Print(L"  vmcleaner.exe /task:status   show the self-scheduled task (read-only, no admin)\r\n");
    out.Print(L"  vmcleaner.exe /alldrives     allow unscoped /recycle or /delete on ALL drives\r\n");
    out.Print(L"                              (without a path you must pass /alldrives or type\r\n");
    out.Print(L"                               ALDRIVES at the prompt; scoping is recommended)\r\n");
    out.Print(L"\r\nExit codes: 0 = ok   1 = error   2 = license declined   3 = cleanup failure(s)\r\n");
    out.Print(L"\r\nExamples:\r\n");
    out.Print(L"  vmcleaner.exe /delete                      unscoped: confirm all-drives, type YES\r\n");
    out.Print(L"  vmcleaner.exe D:\\\\VMs /recycle /yes         recycle a folder (scoped: no confirmation)\r\n");
    out.Print(L"  vmcleaner.exe /alldrives /recycle /yes /accept /csv:C:\\\\fleet\\\\r.csv\r\n");
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
    std::wstring onlyVendor;   // /only: normalized vendor id ("" = all / default)
    unsigned long long maxAgeDays = 0; // /maxage: days (0 = filter off)
    bool alldrives = false; // /alldrives: acknowledge unscoped (all-drives) cleanup
    std::vector<std::wstring> customExts;
    int taskOp = 0; // 0=off(no action)  1..7=register on that day  -1=unregister
    bool taskStatus = false; // /task:status - read-only audit, pre-license

    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a.rfind(L"/log:", 0) == 0)                 logOverride = a.substr(5);
        else if (a.rfind(L"/minsize:", 0) == 0) {
            wchar_t* end = nullptr;
            unsigned long v = wcstoul(a.c_str() + 9, &end, 10);
            if (end && *end == L'\0') g_minArchiveMB = (unsigned int)v;  // 0 = disable archive flagging
        }
        else if (a.rfind(L"/csv:", 0) == 0)            csvOverride = a.substr(5);
        else if (a.rfind(L"/ext:", 0) == 0) {
            size_t before = customExts.size();
            ParseCustomExts(a.substr(5), customExts);
            if (customExts.size() == before) {
                Output o;
                o.Print(L"WARNING: /ext:" + a.substr(5) +
                        L" - no valid extension tokens (letters/digits, max 24 chars, comma-separated)\r\n");
            }
        }
        else if (a.rfind(L"/only:", 0) == 0) {
            std::wstring nv = NormalizeVendor(a.substr(6));
            if (nv.empty()) {
                Output o;
                o.Print(L"WARNING: /only:" + a.substr(6) +
                        L" - unknown vendor (use: all, virtualbox, vmware, hyperv, qemu, "
                        L"parallels, ovf, apple, generic, archive, wim, custom, other). "
                        L"Scanning all vendors.\r\n");
            } else if (nv != L"all") {
                onlyVendor = nv;
            }
        }
        else if (a.rfind(L"/maxage:", 0) == 0) {
            wchar_t* start = (wchar_t*)a.c_str() + 8;
            wchar_t* end = nullptr;
            unsigned long long d = wcstoull(start, &end, 10);
            if (end && *end == L'\0' && end > start && d <= 3650)
                maxAgeDays = d;
            else {
                Output o;
                o.Print(L"WARNING: /maxage:" + a.substr(8) +
                        L" - invalid day count (use 0-3650). Age filter disabled.\r\n");
            }
        }
        else if (a.rfind(L"/task:", 0) == 0) {
            std::wstring d = ToLowerW(a.substr(6));
            if (d == L"off") taskOp = -1;
            else if (d == L"status") taskStatus = true;
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
        else if (a == L"/alldrives" || a == L"--alldrives")  alldrives = true;
        else if (a == L"/?" || a == L"--help" || a == L"-h" || a == L"-help") {
            Output o; PrintUsage(o); return 0;
        }
        else if (!a.empty() && a[0] != L'/') customPath = a;
    }

    // Store accepted custom extensions for matching + re-logging.
    g_customExts = customExts;
    for (size_t i = 0; i < g_customExts.size(); i++) {
        if (i) g_customExtsArg += L",";
        g_customExtsArg += g_customExts[i];
    }
    g_onlyVendor = onlyVendor;   // vendor filter for the scan
    g_maxAgeDays = (ULONGLONG)maxAgeDays;

    // License must be agreed before any task (scan or cleanup) runs.
    // /task:status is read-only fleet auditing and is exempt (no scan, no
    // cleanup, no registry writes) so it works on unlicensed machines.
    if (taskStatus) {
        std::wstring lp = logOverride;
        if (lp.empty()) {
            wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
            lp = std::wstring(cwd) + L"\\vmcleaner_" + NowFileStamp() + L".log";
        }
        Logger lg;
        int st = 0;
        if (lg.Open(lp)) {
            lg.Log(L"VM Cleaner v" VERSION_STRING L" - self-scheduled task status");
            st = StatusTask(lg);
            lg.Log(L"Log file: " + lp);
            lg.Close();
        }
        return st;
    }
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

    // -----------------------------------------------------------------
    // SAFETY GUARD: with no path, /delete and /recycle scan AND clean
    // EVERY drive. That is easy to mistype and dangerous (it can delete
    // data that merely looks like a VM image). It must be acknowledged
    // explicitly - either the /alldrives flag, or by typing ALDRIVES at
    // the prompt - before an unscoped destructive run proceeds.
    // (A /task: run with no path would otherwise bake this into a weekly
    // scheduled job.)
    // -----------------------------------------------------------------
    if (customPath.empty() && mode != CleanMode::None) {
        if (!alldrives) {
            Output o;
            o.Print(L"\r\n");
            o.Print(L"====================================================================\r\n");
            o.Print(L" WARNING: NO PATH GIVEN - this run will scan AND clean ALL\r\n");
            o.Print(L" drives (C:, D:, ...) for VM files and act on every match.\r\n");
            o.Print(L" This is dangerous: any file matching a VM extension on any\r\n");
            o.Print(L" drive will be " + std::wstring(mode == CleanMode::Delete ? L"permanently deleted" : L"recycled") + L".\r\n");
            o.Print(L"\r\n");
            o.Print(L" Pass /alldrives to acknowledge this, or type a specific path\r\n");
            o.Print(L" to scope the run (recommended). In a scheduled task (/task),\r\n");
            o.Print(L" /alldrives must be part of the command line - otherwise the\r\n");
            o.Print(L" run aborts every time.\r\n");
            o.Print(L"====================================================================\r\n");
            o.Print(L"Continue with ALL drives? Type ALDRIVES to confirm: ");
            std::string ans = ReadLineA();
            if (ans != "ALDRIVES" && ans != "alldrives") {
                o.Print(L"Aborted - no drives were modified.\r\n");
                return 1;
            }
            alldrives = true; // acknowledged interactively - thread into any /task
        }
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
    if (!g_customExts.empty())
        log.Log(L"Custom extensions: " + g_customExtsArg + L" (matched as core VM files)");
    if (!g_onlyVendor.empty() && g_onlyVendor != L"all")
        log.Log(L"Vendor filter: only " + g_onlyVendor + L" (other vendors excluded)");
    if (g_maxAgeDays > 0)
        log.Log(L"Age filter: protect folders modified within the last " +
                std::to_wstring(g_maxAgeDays) + L" day(s)");

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
            if (GetFileAttributesExW(customPath.c_str(), GetFileExInfoStandard, &fad)) {
                size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                VmMatch m = MatchVmType(ToLowerW(customPath), size);
                if (!m.type.empty())
                    RecordFinding(log, customPath, size, m.type, m.core, Ftime64(fad.ftLastWriteTime));
                else
                    log.Log(L"  (not a VM image file)");
            }
        } else {
            log.Log(L"ERROR: path not found: " + customPath);
        }
    } else {
        std::vector<std::wstring> extraDirs;
        if (!skipRegistry) DetectVmSoftware(log, extraDirs);
        ScanAllDrives(log, extraDirs);
    }

    log.Log(L"Scan finished: " + NowStamp());
    ApplyAgeFilter(log);
    PrintGroupedReport(log);

    if (taskOp >= 1) ScheduleTask(log, taskOp, mode, assumeYes, assumeLicense, csvOverride, g_customExtsArg, onlyVendor, maxAgeDays, customPath, alldrives);

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
