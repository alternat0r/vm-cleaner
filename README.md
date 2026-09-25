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
    vmcleaner.exe /delete         permanently delete found VM files (admin rights needed for protected files)
    vmcleaner.exe /yes            skip the confirmation prompt (works with /recycle and /delete)
    vmcleaner.exe /accept         record license agreement without prompting
    vmcleaner.exe /log:<path>     write log to a specific file
    vmcleaner.exe /minsize:<MB>   archive size threshold (default 200 MB)
    vmcleaner.exe /noregistry     skip registry detection
    vmcleaner.exe /csv:<path>     also write a CSV report
    vmcleaner.exe /task[=<day>]  self-schedule a weekly cleanup (mon..sun, default sun)
    vmcleaner.exe /task:off      remove the self-scheduled task

Examples:

    vmcleaner.exe                      # find everything, change nothing
    vmcleaner.exe /delete              # prompt, then delete on "YES"
    vmcleaner.exe D:\VMs /recycle /yes # recycle a folder, no prompt
    vmcleaner.exe /recycle /yes /accept /csv:C:\fleet\results.csv   # unattended + report

## Fleet mode (unattended)

For deploying across many machines without interaction:

- `/recycle /yes` or `/delete /yes` — no prompts at all. Combine with `/accept`
  to also skip the one-time license agreement:

      vmcleaner.exe /accept /recycle /yes

- `/csv:<path>` — machine-readable report, one row per finding:
  `host,vm,group,type,path,size_bytes,status` where `status` is
  `found` (dry run), `recycled`, `deleted`, `skipped_in_use`, or `failed`.
  Copy the CSV from every machine to collect fleet-wide results.
- `/task[=<day>]` — registers a weekly Task Scheduler task named `VMCleaner`
  (03:30, default Sunday) that re-runs this same exe with the current cleanup
  flags, so machines clean themselves on a schedule. `/task:off` removes it.
  Registering machine tasks needs an elevated prompt; the tool reports
  failure gracefully otherwise.
- Exit codes: `0` ok, `1` error, `2` license declined, `3` cleanup had
  failure(s) — convenient for batch files and remote scripts.
- `/delete` always prints a **warning + disclaimer** before deleting
  (console and log, even with `/yes`). When the process is **not elevated**,
  an extra admin warning is printed — files in system-protected locations
  (e.g. `C:\ProgramData`) or owned by other users require administrator
  rights and will otherwise fail to delete. Run elevated for full coverage.

## License gate

On first run the tool shows a short license agreement **and disclaimer**
("as is", no warranty, no liability for data loss or damage; review a
dry-run before deleting) and requires the user to agree before **any** task
(scan or cleanup) runs. Declining aborts immediately (exit code 2, nothing
scanned or changed).

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

Archives (may contain a VM image) — only when at least the size threshold
(default 200 MB, tune with `/minsize:<MB>`):
`.zip .7z .rar .tar.gz .tgz .tar .gz .tar.bz2 .tbz2 .tbz .tar.xz .txz .bz2 .xz`

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
- Before any `/delete` the tool prints: a permanence warning, a disclaimer
  (no warranty / no liability), and — when not elevated — an **admin warning**
  (protected or foreign-owned files need administrator rights and may fail).
- Files currently open by another process (e.g. a running VM) are detected and
  **skipped**, never forced.
- `/recycle` uses the Recycle Bin (`FOF_ALLOWUNDO`) — recoverable. On removable
  or network drives Windows may fall back to permanent deletion.
- `/delete` is permanent and not recoverable. Run as administrator when the
  targets include system-protected locations (e.g. `C:\ProgramData\Microsoft\Windows\Hyper-V`).

## Notes

- Empty (0-byte) files are ignored — placeholder/breadcrumb noise.
- Junctions/symlinks and system dirs (`Windows`, `Program Files`,
  `$Recycle.Bin`, `System Volume Information`, ...) are skipped to avoid
  cycles and keep the scan fast.
- Log: `vmcleaner_YYYYMMDD_HHMMSS.log` (UTF-8 with BOM) in the current
  directory or the `/log:` target.
