# VM Cleaner

Native Win32 console tool that locates virtual-machine disk images from any
hypervisor (VirtualBox, VMware, Hyper-V, QEMU/KVM, Parallels, ...) across all
local drives and the registry, groups them per-VM, reports reclaimable space,
and can recycle or delete them. Writes a timestamped log.

## Use case

Designed for bulk cleanup of shared / multi-seat machines — training rooms,
cybercafes, school labs, internet kiosks — where many users leave behind
VirtualBox / VMware / Hyper-V / QEMU images. One dry-run finds everything and
reports reclaimable space; `/recycle` or `/delete` then clears it in a single
pass, with in-use files skipped so a machine still running a VM is never
corrupted.

## Build

Run `build.bat` (VS2022 Community, x64, Release, static /MT). Output:
`bin\x64\Release\vmcleaner.exe`. `src\version.h` auto-increments the **minor**
version on every build (build number resets to 0), e.g. `1.1.0` → `1.2.0`.

## Release

Run `release.bat` to publish to GitHub:

    release.bat            build (minor bump) + commit + push + gh release
    release.bat /dryrun    build + report the version, no push / no release

It commits the source, pushes, and creates a tagged GitHub release
(`vMAJ.MIN.BLD`) with `vmcleaner.exe` attached as an asset, using the
`push.bat` helper and the `gh` CLI. Requires `gh` to be authenticated
(`gh auth login`).


## Usage

    vmcleaner.exe                 dry-run: scan all drives + registry, report only
    vmcleaner.exe <path>          scan a single folder only
    vmcleaner.exe /recycle        move found VM files to the Recycle Bin
    vmcleaner.exe /delete         permanently delete found VM files
    vmcleaner.exe /yes            skip the confirmation prompt
    vmcleaner.exe /accept         record license agreement without prompting
    vmcleaner.exe /log:<path>     write log to a specific file
    vmcleaner.exe /noregistry     skip registry detection

Examples:

    vmcleaner.exe                      # find everything, change nothing
    vmcleaner.exe /delete              # prompt, then delete on "YES"
    vmcleaner.exe D:\VMs /recycle /yes # recycle a folder, no prompt

## License gate

On first run the tool shows a short license agreement and requires the user to
agree before **any** task (scan or cleanup) runs. Declining aborts immediately
(exit code 2, nothing scanned or changed).

Acceptance is persisted per-user in the registry
(`HKCU\Software\VMCleaner`, `LicenseAccepted=1`), so the prompt appears only
once. For scripted / bulk deployment (training rooms, cybercafes) use
`/accept` to record agreement without an interactive prompt:

    vmcleaner.exe /accept /delete /yes

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
