# VM Cleaner

Native Win32 console tool that locates virtual-machine disk images from any
hypervisor (VirtualBox, VMware, Hyper-V, QEMU/KVM, Parallels, ...) across all
local drives and the registry, groups them per-VM, reports reclaimable space,
and can recycle or delete them. Writes a timestamped log.

## TL;DR

    vmcleaner.exe /alldrives /accept /delete /log:vmcleaner.log

## Use case

Designed for bulk cleanup of shared / multi-seat machines — training rooms,
cybercafes, school labs, internet kiosks — where many users leave behind
VirtualBox / VMware / Hyper-V / QEMU images. One dry-run finds everything and
reports reclaimable space; `/recycle` or `/delete` then clears it in a single
pass, with in-use files skipped so a machine still running a VM is never
corrupted.

## License

VM Cleaner is licensed under the GNU General Public License v3.0 (GPL-3.0) —
see [`LICENSE`](LICENSE). You are free to copy, redistribute, and modify the
code and release binary, commercial or otherwise, under the terms of the GPL.

## Build

Run `build.bat` (VS2022 Community, x64, Release, static /MT). Output:
`bin\x64\Release\vmcleaner.exe`. `src\version.h` auto-increments the **minor**
version on every build (build number resets to 0), e.g. `1.1.0` → `1.2.0`.

## Usage

    vmcleaner.exe                 dry-run: scan all drives + registry, report only
    vmcleaner.exe <path>          scan a single folder only
    vmcleaner.exe /recycle        move found VM files to the Recycle Bin
    vmcleaner.exe /delete         permanently delete found VM files (admin rights needed for protected files)
    vmcleaner.exe /yes            skip the confirmation prompt (works with /recycle and /delete)
    vmcleaner.exe /accept         record license agreement without prompting
    vmcleaner.exe /log:<path>     write log to a specific file
    vmcleaner.exe /minsize:<MB>   archive size threshold (default 200 MB; 0 = never flag archives)
    vmcleaner.exe /ext:<a,b,...>  also match these custom file extensions
    vmcleaner.exe /only:<vendor>  only report/clean one vendor (see Vendor filter)
    vmcleaner.exe /maxage:<days>  protect VM folders modified within the last N days
    vmcleaner.exe /cloud          include dehydrated cloud placeholders (excluded by default)
    vmcleaner.exe /noregistry     skip registry detection
    vmcleaner.exe /csv:<path>     also write a CSV report
    vmcleaner.exe /task[=<day>]  self-schedule a weekly cleanup (mon..sun, default sun)
    vmcleaner.exe /task:off      remove the self-scheduled task
    vmcleaner.exe /task:status   show the self-scheduled task (read-only audit)
    vmcleaner.exe /alldrives     required for unscoped /recycle or /delete (all drives)

Examples:

    vmcleaner.exe                      # find everything, change nothing
    vmcleaner.exe /alldrives /delete   # unscoped: confirm all-drives, then type YES
    vmcleaner.exe D:\VMs /recycle /yes # recycle a folder, no prompt
    vmcleaner.exe /alldrives /recycle /yes /accept /csv:C:\fleet\r.csv  # fleet

## Fleet mode (unattended)

For deploying across many machines without interaction:

- `/recycle /yes` or `/delete /yes` — no prompts at all. Combine with `/accept`
  to also skip the one-time license agreement:

      vmcleaner.exe /accept /recycle /yes

  With no path this is a **full all-drives cleanup**, so the safety guard
  requires you to acknowledge it — add `/alldrives` to the command line
  (see *Safety* below):

      vmcleaner.exe /accept /alldrives /recycle /yes

- `/csv:<path>` — machine-readable report, one row per finding:
  `host,vm,group,type,path,size_bytes,status` where `status` is
  `found` (dry run), `recycled`, `deleted`, `skipped_in_use`, or `failed`.
  Copy the CSV from every machine to collect fleet-wide results.
- `/task[=<day>]` — registers a weekly Task Scheduler task named `VMCleaner`
  (03:30, default Sunday) that re-runs this same exe with the current cleanup
  flags, so machines clean themselves on a schedule. `/task:off` removes it.
  Registering machine tasks needs an elevated prompt; the tool reports
  failure gracefully otherwise.
- `/task:status` — read-only audit of the `VMCleaner` task: prints its run
  command (including any `/only:`/`/maxage:`/`/ext:`/`/csv:` it carries),
  schedule, and next run time. No admin rights and no license agreement
  needed (it touches nothing), so it works on unlicensed fleet machines.
  Exit code `1` when the task is not registered, `0` when it is — handy for
  fleet scripts that verify the schedule was applied.
- Exit codes: `0` ok, `1` error, `2` license declined, `3` cleanup had
  failure(s) — convenient for batch files and remote scripts.
## Safety

- **All-drives guard (`/alldrives`).** With no path, `/recycle` and `/delete`
  scan and clean **every** drive. Because that is easy to mistype and can
  remove data that merely *looks* like a VM image, the tool refuses to run
  it unacknowledged: it prints a prominent warning and requires you to type
  `ALDRIVES` at the prompt, or to pass `/alldrives` on the command line
  (needed for unattended fleet runs and scheduled tasks). A scoped run with
  a path is unaffected and still needs no acknowledgement. If the guard
  fires in a scheduled task, the run aborts cleanly (nothing is deleted) —
  a visible, safe failure rather than a silent sweep.
- `/delete` is **permanent**; prefer `/recycle` (recoverable via the Recycle
  Bin) unless you specifically need it gone.
- `/delete` always prints a **warning + disclaimer** before deleting
  (console and log, even with `/yes`). When the process is **not elevated**,
  an extra admin warning is printed — files in system-protected locations
  (e.g. `C:\ProgramData`) or owned by other users require administrator
  rights and will otherwise fail to delete. Run elevated for full coverage.
- `/maxage:<days>` (see *Age filter*) is an extra safety net for unattended
  runs: it skips VM folders modified within the last N days, so an active
  VM is never touched.

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

Installed-software versions (user information only): for each detected VM
product the tool logs a **version** and, when available, the **copyright**
year range — from the uninstall key's `DisplayVersion` and from the PE version
resource of the product's main executable (e.g. `VirtualBox.exe`, `vmware.exe`,
`vmms.exe`). These lines are informational (marked `[for user information]`)
to help spot outdated software; they never affect scanning or cleanup.

Custom extensions: `/ext:<a,b,...>` adds user-defined extensions to the match
list (e.g. `/ext:vmsave,mydisk`). Tokens are case-insensitive, the dot is
optional, letters/digits only (max 24 chars), comma-separated, repeatable.
Custom extensions are treated as **core** VM files (they group a folder as a
VM folder) and are recorded in the log; they also carry over into a
self-scheduled `/task`.

Vendor filter: `/only:<vendor>` limits the scan (and any cleanup) to a single
vendor. The vendor of each finding is taken from the type label shown in the
report. Accepted values (case-insensitive): `all` (default), `virtualbox`,
`vmware`, `hyperv`, `qemu`, `parallels`, `ovf`, `apple`, `generic` (`.img` /
`.raw` / `.dsk`), `wim`, `archive`, `custom` (files matched via `/ext:`), and
`other` (anything unmatched, e.g. `.iso`). An unknown value prints a warning
and scans all vendors. The filter is logged at startup and carries over into a
self-scheduled `/task`.
Example: `vmcleaner.exe /only:vmware /recycle` touches only VMware files.
Note: `.vhd`/`.vhdx` are classified as Hyper-V. `.nvram` carries the label
"VMware/QEMU (NVRAM)" and therefore classifies to the **vmware** bucket, so
`/only:qemu` excludes it.

Age filter: `/maxage:<days>` protects VM folders that were modified recently.
A folder (any of its files) with a last-write time within the last N days is
excluded from the report, cleanup, and CSV; the exclusion is logged. Default
is off (`0`). Invalid values (non-numeric, > 3650) print a warning and leave
the filter off. This is the safety net for unattended fleet runs — e.g.
`vmcleaner.exe /delete /yes /maxage:7` never touches a VM used within the
last week. Carries over into a self-scheduled `/task`.

Cloud filter: dehydrated cloud placeholders (OneDrive / Dropbox "Files
On-Demand") are excluded by default. A placeholder's directory entry reports
its full remote size even though little or nothing is stored on local disk,
so counting it would overstate reclaimable space — and recycling/deleting it
can trigger a re-download or delete the cloud copy instead of just freeing
local disk. Excluded files are logged individually and summarized, and are
left out of the report, cleanup, and CSV, same as `/maxage`. Pass `/cloud` to
include them anyway. Carries over into a self-scheduled `/task`.

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
