# VM Cleaner

Native Win32 console tool that locates virtual-machine disk images from any
hypervisor (VirtualBox, VMware, Hyper-V, QEMU/KVM, Parallels, ...) across all
local drives and the registry, groups them per-VM, reports reclaimable space,
and can recycle or delete them. Writes a timestamped log.

## Build

Run `build.bat` (VS2022 Community, x64, Release, static /MT). Output:
`bin\x64\Release\vmcleaner.exe`. `src\version.h` auto-increments on every build.

## Usage

    vmcleaner.exe                 dry-run: scan all drives + registry, report only
    vmcleaner.exe <path>          scan a single folder only
    vmcleaner.exe /recycle        move found VM files to the Recycle Bin
    vmcleaner.exe /delete         permanently delete found VM files
    vmcleaner.exe /yes            skip the confirmation prompt
    vmcleaner.exe /log:<path>     write log to a specific file
    vmcleaner.exe /noregistry     skip registry detection

Examples:

    vmcleaner.exe                      # find everything, change nothing
    vmcleaner.exe /delete              # prompt, then delete on "YES"
    vmcleaner.exe D:\VMs /recycle /yes # recycle a folder, no prompt

## What it detects

Disk images: `.vdi .vmdk .vhdx .vhd .avhdx .avhd .qcow2 .qcow .qed .cow`
`.hdd .hds .pvm .pvs .pvi .img .raw .dsk .iso .dmg .wim .ova .ovf`

Config / state: `.vbox .vbox-prev .vmx .vmem .vmsd .vmsn .vmxf .vmss .vswp`
`.nvram .vmcx .vmrs .vmgs`

Registry probes: VirtualBox (`DefaultMachineFolder` / `InstallDir` / `Version`),
VMware Workstation (`InstallPath` / VM path), Hyper-V feature key + `vmms.exe`,
and any installed app whose DisplayName matches a VM vendor (uninstall keys —
64-bit, 32-bit, and per-user).

## Report

Findings are grouped by their parent folder. Folders containing at least one
core disk/config file are listed as **VM folders** (with each file + size);
everything else (`.iso`, `.img`, ...) is listed as **loose disk images**. A
total reclaimable-size line closes the report.

## Cleanup behaviour

- `/delete` requires typing `YES` (or `/yes`); `/recycle` requires `y`.
- Files currently open by another process (e.g. a running VM) are detected and
  **skipped**, never forced.
- `/recycle` uses the Recycle Bin (`FOF_ALLOWUNDO`) — recoverable. On removable
  or network drives Windows may fall back to permanent deletion.
- `/delete` is permanent and not recoverable.

## Notes

- Empty (0-byte) files are ignored — placeholder/breadcrumb noise.
- Junctions/symlinks and system dirs (`Windows`, `Program Files`,
  `$Recycle.Bin`, `System Volume Information`, ...) are skipped to avoid
  cycles and keep the scan fast.
- Log: `vmcleaner_YYYYMMDD_HHMMSS.log` (UTF-8 with BOM) in the current
  directory or the `/log:` target.
