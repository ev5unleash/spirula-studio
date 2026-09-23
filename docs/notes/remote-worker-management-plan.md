# Remote Worker Management

**Status:** The Windows M0 service-account Vulkan probe is historically recorded.
M1-M4 and M5 administrative controls are present in source; the current Windows
Vulkan build completes, and focused pairing, transfer, wire, package, signed
intent, feature, reconstruction, and training job checks pass. The integrated
leader test still fails feature publication, checkpoint rehydration, and signed
update dispatch; those paths are under repair. No physical two-worker, reboot,
activation, rollback, or GUI visual acceptance has been performed. Linux and
macOS compile/runtime evidence remain open. This is an implementation and
evidence ledger, not authorization.

The one-shot Windows M0 probe was installed as an automatic service and started
through SCM for that historical proof. That does not establish the current
service installation state or remote-job acceptance. No firewall rule,
listener, credential, model, driver, remote execution path, push, publication,
or reboot is authorized by this document.

## Scope Lock

### Accepted

- M0 health and Vulkan capability probing from an SCM-launched, least-privilege
  worker service, with the actual service identity and an explicit physical
  device UUID recorded. Reboot validation is not an M0 requirement.
- Later service-managed execution of one frozen native feature-extraction
  request or shard per worker process. Inputs, masks, recipe, model/shader
  provenance, request identity, attempt identity, and output root are immutable
  for that attempt.
- Whole remote reconstruction owned by one worker, using the existing native
  reconstruction pipeline and returning a validated model to the leader.
- Whole remote training owned by one worker and one GPU, returning resumable
  checkpoints, configuration, logs, and required output artifacts.
- Per-image feature payloads, receipts, shard results, cancellation records,
  and a sealed collection transferred to a coordinator through an approved
  mechanism. The coordinator remains the sole owner of the mutable
  reconstruction workspace.
- Existing native Vulkan on non-NVIDIA hardware with `SS_ENABLE_PATENTED=OFF`.
  A worker may use CPU work that the accepted native path already uses, but
  remote acceptance is for the explicitly selected native workload, not for
  arbitrary GPU programs.

### Excluded

- Distributed matching, mapping, bundle adjustment, or optimizer state. Whole
  remote reconstruction and single-GPU whole remote training remain in scope.
- Remote shared mutable feature, match, resume-journal, sparse-model, or
  training-output directories; a shared filesystem is transport, not a lock or
  database.
- Raw-video resampling/masking distribution, external COLMAP, external Python
  masking, arbitrary shell/plugin execution, remote GUI or live viewport
  attachment, and a general DAG or command broker.
- Automatic migration, retry, device failover, GPU hot switching, priority or
  preemption, multi-job VRAM packing, machine-wide GPU reservation, CUDA, and
  NVIDIA validation.
- An unauthenticated listener, public discovery, Internet exposure workflow,
  relay, shared fleet password, custom cryptography, or remote shell.

The repository's completed two-host feature campaign used manual authenticated
transfer and explicitly did not add a network service
(`docs/notes/distributed-dataset-creation-plan.md:24-33,285-301,997-1030`). It
is evidence for artifact/central-pipeline behavior, not for service management.

## Invariants

- A service request is allowlisted, schema-validated, immutable, and bound to
  `job_id`, `attempt_id`, plan/request digests, absolute host-local roots, and a
  canonical Vulkan UUID. The current worker boundary already rejects unresolved
  `auto` devices and unknown phases (`src/app/WorkerRequest.cpp:775-818`).
- One worker process tree uses one host-local physical device. Explicit UUID
  selection is resolved in the worker's own Vulkan instance; an unavailable or
  ambiguous UUID blocks the attempt instead of falling back to a name, ordinal,
  CPU, or another GPU (`src/core/VulkanDeviceSelection.h:185-235,254-330`).
- The device lease covers `Starting` through process-tree exit and reap, not a
  progress message. The lease is host-local and only coordinates participating
  Spirula workers; it does not reserve VRAM or exclude unrelated applications.
- Output ownership and device ownership are separate. An attempt holds an
  OS-backed output lease for its whole lifetime; no result is accepted from an
  unclaimed or surviving writer (`src/app/OutputLease.cpp:25-79`).
- A payload is not completed work until its receipt validates. A shard is not
  complete until every assigned image has a successful or valid zero-feature
  outcome, and a collection is not consumable until its sealed index validates.
- Remote execution reuses the existing reconstruction and training paths.
  Feature shards retain the centralized collection/import barrier; whole-job
  results are staged and validated before leader publication.
- Stop, crash, and service restart preserve the last authoritative artifact and
  mark the current attempt interrupted. Recovery creates a new attempt or
  resumes at an existing stage boundary; it never relabels a stale result as
  current and never kills a reused PID without ownership proof.
- Machine configuration, durable state, work files, logs, and model caches are
  service-scoped. The service must not inherit a logged-in user's `HOME`,
  `APPDATA`, `LOCALAPPDATA`, current directory, GUI state, or credentials
  (`src/app/AppPaths.cpp:47-72`).

## Service Design

The worker service is implemented around the allowlisted `spirula worker
--request <file>` boundary, not as a generic process launcher. Windows SCM
uses `spirula.exe agent service --config-root <absolute> --state-root
<absolute> --storage-root <absolute>`; POSIX managers use `spirula agent run`
with those same roots. The hidden `agent probe` and one-shot Windows
`agent service-probe` remain M0 evidence tools, separate from the operational
control plane. Source for service management, TLS, pairing, and workloads now
exists; the M0 service-account proof does not verify remote pairing or jobs.

### Windows SCM

The exact SCM record is:

| Field | Required value |
|---|---|
| Service name | `SpirulaRemoteWorker` |
| Type | `SERVICE_WIN32_OWN_PROCESS` |
| Start | `SERVICE_AUTO_START`; boot start, not delayed or interactive-user start |
| Account | virtual service account `NT SERVICE\SpirulaRemoteWorker` |
| Token | `SERVICE_SID_TYPE_RESTRICTED`; no administrator or `LocalSystem` identity, no interactive logon |
| Image | `<install-root>\spirula.exe agent service --config-root <machine-config> --state-root <state-root> --storage-root <storage-root>` |
| Desktop access | none; no window station, GUI, viewer, or user-session dependency |
| Failure action | restart the daemon after 5 s, 30 s, then 300 s; reset the failure count after 24 h |
| Job retry | never implicit; daemon restart reconciles the attempt and leaves it interrupted until an authorized retry |
| Stop/shutdown | accept SCM stop and shutdown; drain or stop owned workers, persist state, then report stopped |

The service SID and account receive read access to the installed binary,
Vulkan loader/driver-visible resources, machine config, and explicitly
configured read-only model/input roots. They receive modify access only to
durable service state, attempt storage, and logs. The install tree, arbitrary
user profiles, and unconfigured paths are not writable. The service-control
DACL is distinct from the worker token; remote administrative actions are not
an operator-supported path while M5 broker integration remains pending.

`SERVICE_RUNNING` means only that the daemon is alive, not that Vulkan is usable
or that a remote worker is ready. The live worker must report ready,
compatible, healthy capability status for its configured device/build. The
historical M0 probe is supporting local-device evidence, not a dispatch gate or
proof of an operational remote connection. Windows boot/reboot behavior for
remote jobs has not been accepted.

### Linux systemd

`AgentServiceManager.cpp` contains systemd service-management source; its
`ExecStart` contract is `<install-root>/spirula agent run --config-root
<machine-config> --state-root <state-root> --storage-root <work-root>`. The
system-unit target is `spirula-remote-worker.service` under
`/etc/systemd/system/`, enabled for `multi-user.target`, with
`Type=simple`, `User=spirula-worker`, `Group=spirula-worker`,
`Restart=on-failure`, `RestartSec=5`, `NoNewPrivileges=true`,
`PrivateTmp=true`, `ProtectSystem=strict`, `ProtectHome=true`, and `UMask=0077`.
Only the deployment's render-device group may be added when required by the
driver; `ReadWritePaths` is limited to state, work, and log roots. This target
is not installed or validated, and Windows evidence is not Linux compile or
runtime evidence.

### macOS LaunchDaemon

`AgentServiceManager.cpp` contains LaunchDaemon management source. Its target
is `/Library/LaunchDaemons/com.spirula.remote-worker.plist`, label
`com.spirula.remote-worker`, `RunAtLoad=true`, `KeepAlive=true`,
`ProcessType=Background`, and a dedicated non-root `spirula-worker` account.
It invokes `spirula agent run` with the same explicit roots and has no
Aqua-session or GUI dependency. The target is not installed or validated.

**macOS GPU access at boot is unverified.** A LaunchDaemon running before user
login may not see the Vulkan loader, ICD, or a usable physical device even when
the same binary works in a GUI session. No root or login-session workaround is
accepted. Linux and macOS builds and runtime remain unverified.

## Locations

These are conceptual machine-scoped locations, not personal paths and not an
authorization to create them:

| Data | Windows | Linux | macOS |
|---|---|---|---|
| Service definition | SCM database | `/etc/systemd/system/spirula-remote-worker.service` | `/Library/LaunchDaemons/com.spirula.remote-worker.plist` |
| Read-only service config | `%ProgramData%\Spirula\RemoteWorker\config` | `/etc/spirula/remote-worker/` | `/Library/Application Support/Spirula/RemoteWorker/config/` |
| Durable state and lock | `%ProgramData%\Spirula\RemoteWorker\state` | `/var/lib/spirula/remote-worker/state/` | `/Library/Application Support/Spirula/RemoteWorker/state/` |
| Attempt storage | `%ProgramData%\Spirula\RemoteWorker\storage` (service-owned attempts) | `/var/lib/spirula/remote-worker/work/` | `/Library/Application Support/Spirula/RemoteWorker/work/` |
| Logs | SCM/Event Log plus per-attempt logs | journald plus per-attempt logs | unified log plus per-attempt logs |
| Model/cache inputs | explicit read-only configured roots | explicit read-only configured roots | explicit read-only configured roots |

Each attempt contains its immutable request/binding, `payload/`, `receipts/`,
`result.json`, and diagnostics. Incoming transfers are staged separately from
active work and copied into coordinator-owned storage before acceptance. State
publication uses the existing temp-write/flush/atomic-replacement pattern;
current local scheduler state is `job-state.json` with a sibling lock
(`src/app/JobScheduler.cpp:600-629,1119-1170`). A remote service must not share
that user-scheduler file or a mutable workspace with another host.

The TLS dependency and pairing implementation are present: `cmake/SsAgentTls.cmake`
pins Mbed TLS 3.6.7; `src/app/AgentTls.cpp` and `src/app/AgentPairing.cpp`
implement the authenticated TLS and pinned enrollment path. Worker identity
and trust state are machine-scoped under the protected service state root.
This corrects the earlier dependency-absent note; physical remote-operation
evidence remains pending.

## M0 Vulkan Probe

The canonical M0 command is the hidden `agent` probe. Its source selects
one explicit device, acquires the local lease, allocates device memory, performs
a memset and device-to-host readback, verifies the bytes, frees the allocation,
and emits machine-readable JSON (`src/app/cli/agent_main.cpp:106-142,169-225`).
It is an active GPU/lease probe, not read-only status inspection or acceptance
of remote training:

```text
<install-root>\spirula.exe agent probe --device uuid:<canonical-device-uuid> --hold-ms 1000
```

This records historical command syntax only; do not rerun it as part of
read-only evidence collection.

On 2026-09-22 the Windows Vulkan build completed with
`build_develop.bat -DSS_BACKEND=vulkan`. The focused subprocess test and full
headless selection passed, with 24/24 headless tests. That historical build
predates the later source changes; current-source build evidence is recorded
in the ledger below. An interactive-user probe
on an AMD Radeon AI PRO R9700 returned `success=true`, canonical selector
`uuid:00000000010000000000000000000000`, Vulkan API version `4211037`, and
driver version `8389003`. A concurrent second process returned exit code 1 and
`device lease busy`; omission of `--device` returned exit code 2 and `device
option is required`.

The final build, SHA-256
`dc2d8627345232f5cc21e534ad83aba27a9ece05d2b5664f19cd393d074bd6cb`, was
installed and launched through SCM. Service PID 16172 launched child PID 14528
in session zero under `NT SERVICE\SpirulaRemoteWorker`. The current and expected
service SIDs matched, the service SID was restricted, and the nested probe
reported `success=true`, `lease_acquired=true`, backend `vulkan`, the requested
UUID, API version `4211037`, driver version `8389003`, and no error. SCM reported
exit code zero and the installed binary hash matched the evidence.
The recorded Windows M0 proof used the SCM-launched one-shot probe under
`NT SERVICE\SpirulaRemoteWorker`, with no interactive-user process substitution.
It recorded the service SID/account, daemon and child PIDs, executable
fingerprint, Vulkan API/driver, device name, canonical UUID, probe JSON, exit
code, lease outcome, and service logs. The probe emitted `success=true`,
`backend=vulkan`, the requested UUID, no error, and completed allocation,
dispatch, readback, and free. This is local M0 evidence only; it does not
validate a paired remote worker, a distributed workload, or two physical
worker hosts. Device listing, `spirula --help`, a compile, or a shell run as an
administrator is not equivalent evidence. If a future separately authorized
M0 recheck is needed, use a canonical UUID (never an ordinal) and require the
explicit selector to win over a conflicting inherited selector when two
devices are available. `vk_runtime_smoke` is supporting evidence only.

## Device Lease

The lease key is `(host identity, canonical Vulkan UUID)`. Resolve and validate
the UUID at request bind time and again in the worker's own Vulkan instance.
`auto`, display names, and ordinals may be input conveniences before binding;
they are never persisted as the running identity. A missing, duplicate,
unusable, or changed UUID blocks the attempt.

The service persists `Starting` before spawn, acquires the device and output
leases, launches one allowlisted worker tree, and retains both leases until the
tree has exited, all descendants are reaped, and the current attempt result has
been accepted or marked failed. A progress line, EOF, or expected exit code
alone does not release either lease. A survivor or ambiguous ownership keeps
the device/output blocked. Recovery is explicit and creates a new attempt ID;
there is no automatic failover.

This is the same ownership shape already present in the local scheduler:
one lease map per normalized device, first-eligible dispatch, and release only
on reaping (`src/app/JobScheduler.h:1-12,236-245`; `src/app/JobScheduler.cpp:1471-1623,1677-1783`).
The service extension must preserve that invariant across daemon restarts and
hosts; it must not claim exclusive physical GPU access or guaranteed VRAM.

The current primitive uses a `Global\` named object on Windows, an abstract
Unix-domain socket on Linux, and a `/var/tmp` `flock` file on other POSIX
systems. The scheduler passes the native handle or descriptor to the worker so
the lease survives scheduler-side release until the worker exits. Linux
coordination is limited to one network namespace; the systemd service must not
use a private network namespace, and containerized workers need a different
host-shared lock. The non-Linux POSIX path assumes the lock-file owner does not
maliciously unlink and replace the file. Neither limitation has runtime evidence
on its target platform.

## Observed Recovery Boundaries

| Boundary | Current code evidence | Consequence for service pause/recovery |
|---|---|---|
| Feature payload and receipt | `src/sfm/core/FeatureWork.h:81-128`; `src/sfm/Pipeline.cpp:1832-1882,1996-2041,2082-2129` | The durable unit is one validated image receipt. A payload without a receipt is redoable, a valid zero-feature receipt is successful, and an interrupted shard may be retried/adopted without claiming the whole shard completed. |
| Shard result and collection seal | `src/sfm/Pipeline.cpp:2132-2155,2235-2257,2260-2430`; `src/sfm/core/FeatureWork.cpp:789-881` | Publish `result.json` only after assigned outcomes exist; reject stale request/cancellation identities, missing receipts, digest disagreement, path escape, and conflicting payloads. Collection import is a barrier. |
| Matching journal | `src/sfm/core/Resume.cpp:288-325,388-412,414-521`; `src/sfm/Pipeline.cpp:2744-2757,2885-2897` | Completed pairs are the resume unit. Torn tails are trimmed, dependency/product/payload digests are checked, and only unjournaled pairs are retried. A service stop must not advertise an unfinished match database. |
| Matching cancellation | `src/sfm/feature/Verification.h:631-652,679-779`; `src/sfm/core/Cancel.h:36-41` | Cancel is checked between batches/pairs and after verifier threads join. The safe boundary is after worker join and journal flush, not an arbitrary thread suspension. |
| Mapper growth and seed attempts | `src/sfm/map/Mapper.h:391-426,1901-1974` | Cancellation is observed at seed/growth boundaries. There is no remote mapper checkpoint contract; a stopped central mapping phase restarts from its sealed feature/match inputs. |
| Mapper/BA resource gates | `src/sfm/map/Mapper.h:767-780,1293-1354` | `BAOverBudget` declines or splits a solve; it is not a recoverable partial result and must not trigger device migration or distributed BA. |
| BA iteration state | `src/sfm/ba/Solver.h:448-554`; CPU fallback `src/sfm/ba/SolverCpu.h:94-178` | Cancel is checked per iteration; rejected steps restore parameters and the final download is the last accepted state. No service protocol may claim a checkpoint inside a live solve. |
| Trainer iteration/pause | `src/app/TrainerCore.cpp:1240-1313` | Training pause waits between iterations; stop is checked before the next step. Data-error retry decrements the step because that step did not run. |
| Trainer checkpoint/resume | `src/app/TrainerCore.cpp:1004-1064,1066-1160,1329-1338`; worker stop control `src/app/cli/main.cpp:518-562,628-640` | Resume starts from a validated checkpoint. Stop-and-save publishes a staged, validated checkpoint after the loop. A forced stop only preserves the last published checkpoint and is not successful completion. |

The service exposes pause only where the underlying phase has a real pause
boundary. Feature extraction uses receipt publication; reconstruction uses
cooperative stage or algorithm gates and separately defined restart points.
Training may pause between iterations, but service-restart recovery remains a
published checkpoint rather than in-memory `TrainerSession` state.

## Windows Operator Reference

This reference describes the existing Windows GUI/CLI surfaces; it does not
authorize their use. Run state-changing steps only after separate local
authorization. Windows service and machine-policy changes use local
administrator APIs; the GUI does not elevate itself. Keep ordinary operator
work in a non-elevated session. The restricted `NT SERVICE\SpirulaRemoteWorker`
account runs work and never serves as an administrator or authorization
principal.

### Prerequisites and service configuration

Use matching, trusted Windows `spirula.exe` builds on the interactive leader
and each worker. A worker needs a usable Vulkan, non-NVIDIA physical GPU, its
canonical `uuid:<32 lowercase hex digits>` selector in the worker policy, and
enough configured disk budget/free storage. Worker admission requires a
connected, paired, ready, healthy, compatible worker with the exact build and
workload capability. The leader's policy address is a private numeric IPv4 or
IPv6 address; the TLS server name is a DNS name. Use only already-approved
private-network connectivity and ports. This document does not authorize
opening listeners or changing firewalls.

The Windows source defaults are `%ProgramData%\Spirula\RemoteWorker\config`,
`%ProgramData%\Spirula\RemoteWorker\state`, and
`%ProgramData%\Spirula\RemoteWorker\storage`; `Worker Management` shows the
selected paths. Choose existing, disjoint, machine-owned roots with reviewed
service-account ACLs. The panel does not create or secure these roots.
`spirula.exe agent --help` documents the CLI:
`agent probe`, `agent run`, and `agent service` are the agent entry points.
Windows `agent service --config-root ... --state-root ... --storage-root ...`
is an SCM entry point, not a console substitute. There is no supported CLI
pairing/leader/job-management command; use the GUI.

After separate local approval, open `View` > `Workers...` (window title
`Worker Management`; the worker status strip also opens it). In `Worker machine
policy`, load or enter the machine policy and confirm `Leader numeric address`,
`TLS server name`, `Operational port`, `Allowed Vulkan GPU UUIDs (one per
line)`, `Max jobs`, and positive `Disk budget`; save it while the service is
stopped. Keep `Allow remote reboot` and `Allow remote update` off unless a
separate local policy grant explicitly permits that operation. Those checkboxes
are not by themselves authorization.

In `Local worker service`, confirm `Executable`, `Machine config folder`,
`State folder`, and `Worker storage folder`, then use the separately authorized
`Install service` action. Installation validates existing roots, registers
`SpirulaRemoteWorker` as `SERVICE_WIN32_OWN_PROCESS`, uses
`NT SERVICE\SpirulaRemoteWorker` with `SERVICE_SID_TYPE_RESTRICTED`, and sets
automatic startup and failure recovery at 5/30/300 seconds. It does not start
the service. Confirm paths and start mode before proceeding. The equivalent
CLI flags are used by the SCM image; no `sc.exe create/config/start` command is
part of this reference.

### Leader and pairing

On the interactive leader, in `Worker Management` > `Leader`, set the leader
state folder, private numeric `Bind address`, `TLS server name`, and distinct
`Operational port` and `Enrollment port`. Use ports already approved for the
private network. Confirm `Start leader`; keep the application running while
serving jobs. The window shows the actual ports and an enrollment SPKI
fingerprint. Verify the fingerprint independently; then use `Issue one-use
invitation` and convey its short-lived code (five-minute lifetime) separately
from the fingerprint.

`Start leader` also initializes or reloads a separate update-signing key.
Copy its read-only `Update signer SHA-256` fingerprint from the leader panel
over an independently trusted channel when configuring the Windows worker's
machine policy. It is not the enrollment SPKI fingerprint; never substitute
one for the other or have a remote worker set its own trusted pin.

On each installed Windows worker, keep `SpirulaRemoteWorker` stopped. In
`Pair this worker`, enter `Leader numeric address`, matching `TLS server name`,
the leader's actual enrollment port, a worker name, invitation code, and the
independently verified 64-hex-digit leader SPKI pin. Confirm `Redeem`. The
invitation travels only within pinned TLS. On the leader, select the pending
worker in `Workers`, verify its identity, and explicitly confirm `Approve`.
Back on the worker, use `Check approval` while its service remains stopped;
pairing does not save machine policy. Verify the policy's operational address
and actual leader operational port. If protected administration is approved,
complete the next section while the service is stopped; then separately
confirm `Start service`.
Repeat one worker at a time. In the leader's `Workers` table confirm distinct
worker IDs, `Paired`, `Connected`, `Ready`, compatible build, healthy status,
and the expected platform/GPU. `SERVICE_RUNNING` alone is not readiness.

`Reject`, `Revoke`, and `Forget local pairing` are identity-changing operations,
not troubleshooting shortcuts. Pairing, approval, revocation, forgetting, and
service-owned state changes require local authorization and the UI's explicit
confirmation. If an approved re-pair reports a pending pairing-state update,
keep that service stopped and use `Retry pairing-state update` only after
resolving the reported error.

### Protected administration on Windows

Before a local administrator considers `Install administration broker`, stop
the worker service, pair it, save its machine policy with an independently
verified update-signer SPKI pin, and explicitly choose `allow_reboot` and/or
`allow_remote_update`. The worker must use automatic startup. Enter the
locally approved **incumbent security version**: a positive monotonic baseline
for the installed worker image, not a version guessed from its build label.
The broker refuses an installing executable whose bytes differ from that
worker image, protects its own executable/journal and pins the baseline.
Installation does not authorize a reboot or update.

For a ready compatible worker, `Restart Spirula` drains supported jobs;
`Force Restart Spirula` can lose unpublished work. Confirm each target and
inspect `Command history`. `Reboot machine` requires an explicit matching
worker policy grant, current signed leader authority, and a separate
confirmation. A post-reboot worker that does not report healthy readiness
within one hour is recorded Failed, not retried; repair GPU, disk, pairing, or
service health locally before issuing further work. For `Update Spirula`,
choose a local package and enter its platform, architecture, build, release,
and a security version above the broker's current durable floor. The leader
hashes and signs the staged bytes; the restricted worker transfers them, and
the protected broker verifies, activates, and checks worker health or rolls
back. Do not treat an Accepted request as a completed operation. These
workflows have source and host-check coverage only, not physical acceptance.

### Submit the three workloads

These are GUI workflows, not `agent` CLI job commands. All selections are
explicit or based on the current ready/compatible snapshot; there is no
automatic worker failover.

1. **Feature shards:** In the normal dataset workflow, set `Feature extraction
   shards` above 1 and submit the run. When the leader is running, the GUI
   automatically assigns eligible connected workers at most one shard each
   before filling remaining shards locally; the coordinator imports only after
   validated receipts succeed. Follow per-shard worker/state and collection
   status in the dataset view and `Worker Management` > `Workers`. There is no
   per-shard worker picker. A cancelled/superseded result is fenced from
   collection, but that alone does not guarantee an active remote process has
   stopped.
2. **Whole reconstruction:** In the dataset/reconstruction view select a
   `Whole-SfM worker` (default `Local`), keep the built-in engine and local
   built-in preparation, disable external masking, and set feature shards to
   1. Submit with `Create dataset`/`Update dataset`. The selected worker owns
   the whole reconstruction; the validated model returns to the leader.
   Unavailable workers do not trigger local fallback or reassignment.
3. **Whole training:** In `Train`, select a `Training worker` (default `This
   computer (local)`), configure the dataset/run including an explicit resume
   checkpoint if needed, then use `Train on worker`. The worker must be paired,
   ready, compatible, on the same build, and report non-NVIDIA Vulkan training
   capability. Check the remote job and identity status; after verified
   success use `Resume Locally` only if the returned checkpoint and rebound
   dataset are available. `Cancel remote training` fences/supersedes the
   attempt; do not infer successful completion from a cancellation request.

### Safe operation, restart, and recovery

For routine control use the selected worker's `Maintenance`, `Online`, `Pause`,
`Resume`, `Stop selected job`, or `Stop all active jobs` controls. Confirm stop
commands and inspect their acknowledgement/outcome in `Command history`.
`Service Stop` and `Service Start` are separate local-admin UI operations; a
service stop can interrupt work. After any process/service interruption,
accept only the last validated/published result: feature receipts and the
sealed collection, a validated reconstruction result, or the last returned
training checkpoint. Recovery is a new, explicit attempt or resume from that
checkpoint; never relabel partial or stale output as success and do not delete
state/attempt roots to clear an error.

`Restart Spirula` and `Force Restart Spirula` are visible for a compatible,
connected paired worker; each has a separate target-aware confirmation.
Safe restart refuses unsupported active phases (including reconstruction)
and stop-and-saves supported training/feature work; forced restart stops
active agent-owned jobs and preserves only already-published artifacts.
Neither has accepted physical runtime evidence. Do not infer recovery from
SCM `SERVICE_RUNNING` alone.

Safe failure triage:

- **No worker snapshot / disconnected:** verify the leader is running, actual
  numeric address and ports agree with worker policy, TLS name and separately
  verified pin match, and existing approved connectivity is available. Do not
  change firewall/network policy here.
- **Pending approval:** explicitly approve the correct worker on the leader,
  then stop the worker service and run `Check approval`.
- **Connected but not ready/compatible:** compare worker ID, exact build,
  platform, reported GPU, health, and capabilities; confirm the host policy
  allows its known canonical Vulkan UUID and the configured disk budget is
  available. Do not use an ordinal or silently substitute another worker.
- **Failed/interrupted/unknown attempt:** retain its state and diagnostics.
  Read the reported error and Windows SCM events; choose the workload-specific
  explicit stop/retry/resume path only after identifying the durable boundary.
  A missing leader snapshot, failed command, or unverified output is not success.

### Read-only evidence collection

These inspection commands do not install, start, stop, configure, or probe a
service/GPU. Run them only on hosts within the approved evidence scope; redact
machine identifiers before sharing:

```powershell
$Spirula = '<approved-path-to-spirula.exe>'
$Port = 47000 # replace with an already-approved port
& $Spirula agent --help
Get-CimInstance Win32_Service -Filter "Name='SpirulaRemoteWorker'" |
  Select-Object Name, State, StartMode, StartName, ProcessId, PathName, ExitCode
sc.exe qc SpirulaRemoteWorker
sc.exe qfailure SpirulaRemoteWorker
Get-FileHash $Spirula -Algorithm SHA256
Get-FileHash "$env:ProgramData\Spirula\RemoteWorker\config\agent-policy.json" -Algorithm SHA256
Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue
Get-WinEvent -FilterHashtable @{ LogName='System'; ProviderName='Service Control Manager' } -MaxEvents 100 |
  Select-Object TimeCreated, Id, LevelDisplayName, Message
hostname
Get-CimInstance Win32_ComputerSystemProduct | Select-Object Vendor, Name, UUID
```

Record each host's separately obtained machine identity, worker ID, build,
platform, GPU/UUID, policy and executable hashes, service account/SID and
start/state, job/attempt IDs, result-validation state, and relevant event
times. Distinct worker IDs do not prove distinct physical hosts; collect the
machine identity on each host. Reading prior M0 evidence is read-only, but
`agent probe` itself allocates GPU memory and acquires a device lease. GUI
source controls are implemented, but the recorded screen capture was black:
the `Worker Management` window/status strip is visually unverified.

Linux and macOS have POSIX `agent run`/service-manager source, but no compile or
runtime acceptance; the Windows steps and evidence do not transfer to those
platforms. No two physical remote workers have been validated.

## Evidence Ledger

Status is deliberately separated: **source-implemented** means code exists;
**compile-verified** and **runtime-verified** name the exact historical lane;
**physical acceptance** requires the actual distributed workload and hosts.
None of these states alone authorizes an operation.

| Item | Status | Current evidence and remaining limit |
|---|---|---|
| M0 `DeviceLease`, `agent probe`, one-shot Windows SCM probe | source-implemented; Windows compile- and runtime-verified historically | The 2026-09-22 Vulkan build, focused subprocess test, 24/24 headless tests, explicit-device probe, process contention check, and restricted SCM service-account probe passed on an AMD Radeon AI PRO R9700. The installed `service-probe` evidence is not remote pairing/job acceptance. |
| Current Windows Vulkan source build and headless suite | compile-verified; host tests passed | `build_develop.bat -DSS_BACKEND=vulkan` completes with broker, wire, GUI, and distributed sources; all 38 labeled headless tests pass, including `agent_leader_test` for feature publication, reconstruction return, training rehydration, and signed update dispatch. This loopback/host evidence is not two-worker service acceptance. |
| Existing feature schemas and manual transfer campaign | source/test-verified; manual/file workflow only | `src/sfm/core/FeatureWork.h/.cpp`, `src/sfm/tests/sfm_feature_work_test.cpp`; `docs/notes/distributed-dataset-creation-plan.md:997-1075` records manual authenticated transfer and central handoff. No agent TLS service or SCM/systemd/LaunchDaemon participated. |
| M1 service lifecycle, policy, TLS/pairing, revocation, capabilities, and durable command state | source-implemented | `src/app/AgentServiceManager.cpp`, `AgentConfig.cpp`, `AgentTls.cpp`, `AgentPairing.cpp`, `AgentClient.cpp`; M0 service proof covers only the probe, not operational pairing, reconnect, or remote controls. |
| M2 remote feature shards and sealed collection | source-implemented; leader loopback test passed; physical runtime unverified | `src/app/AgentLeader.cpp`, `AgentFeatureWorker.cpp`, `AgentTransfer.cpp`, `src/app/gui/GuiApp.cpp`; sealed central publication passed in `agent_leader_test`, but no two-physical-worker acceptance. |
| M3 whole remote reconstruction | source-implemented; leader loopback test passed; physical runtime unverified | `src/app/AgentReconstructionJob.cpp`, `AgentLeader.cpp`, and GUI coordinator; returned model validation passed in `agent_leader_test`, not a physical remote reconstruction. |
| M4 whole remote training | source-implemented; leader loopback test passed; physical runtime unverified | `src/app/AgentTrainingJob.cpp`, `AgentLeader.cpp`, and GUI coordinator; training rebound/rehydration passed in `agent_leader_test`. The AMD R9700 `cli_training_smoke` covers the local CLI trainer only, not remote checkpoint return. |
| Safe/forced service-restart request handling | source-implemented; physical runtime unverified | `src/app/AgentClient.cpp`, `AgentFeatureWorker.cpp`, `AgentWire.cpp`, and localized worker-panel buttons; no service-restart runtime acceptance. |
| M5 signed admin intent and Windows protected broker | source-implemented and Windows-compiled; physical runtime unverified | `src/app/AgentAdminIntent.*`, `AgentAdminBroker.*`, `AgentLeader.cpp`, `AgentClient.cpp`, and worker-panel controls. Signed-intent, wire, and integrated signed-update dispatch host tests pass. No reboot, service installation, activation, or rollback was exercised. |
| Worker Management GUI and status strip | source-implemented; visual acceptance unverified | `src/app/gui/AgentPanel.cpp`, `GuiApp.cpp`; source controls exist, but the screen capture was black. |
| Focused AgentLeader test output | passed, host-only | Feature sealed publication, reconstruction return, training rebound/rehydration, and signed update package transfer all pass in `agent_leader_test`; no service installation or physical remote operation occurred. |
| Linux systemd compile/runtime and service/GPU acceptance | unverified | Source exists, but no Linux build, service installation, boot, or remote workload was accepted. |
| macOS LaunchDaemon compile/runtime and GPU-at-boot acceptance | unverified | Source exists, but no macOS build, daemon launch, boot-context GPU proof, or remote workload was accepted. |
| Two physical remote workers / distributed acceptance | not performed | No two physical remote workers have been validated. Windows-only local Vulkan evidence does not establish this criterion. |

## Milestone Gates

| Gate | Required result | State |
|---|---|---|
| M0: service feasibility | Build the active Vulkan configuration; prove unattended Windows service-account GPU execution and local/agent device ownership; inventory pause and recovery boundaries. | **Complete for the historical Windows M0 probe only.** It does not imply reboot validation or remote workload acceptance. |
| M1: secure agent | Service lifecycle, pairing/revocation, authenticated connection lifecycle, capabilities, durable node state, maintenance, and allowlisted execution. | **Source and Windows build verified; physical operational acceptance pending.** |
| M2: remote feature shards | Frozen input/result transfer, multi-worker scheduling, validated receipts/collection, progress, and reconnect reconciliation. | **Source and leader loopback test verified; physical two-worker and remote service acceptance pending.** |
| M3: remote reconstruction | Whole-job contract, worker-owned workspace, safe controls, and validated usable model return. | **Source and leader loopback test verified; physical remote result acceptance pending.** |
| M4: remote training | One-GPU training contract, concurrent independent jobs, returned resumable checkpoints. | **Source and leader loopback test verified; remote checkpoint/resume acceptance pending. Local CLI smoke is not this evidence.** |
| M5: administrative controls | Safe/forced restart, permission-gated reboot, and signed verified update/activation with rollback. | **Source, Windows build, and host-only intent/dispatch tests verified; not accepted.** Reboot, service restart, update, and rollback have not been exercised on a worker. |
| M6: acceptance and closeout | GUI/status integration, fault testing, platform evidence, and operator documentation for all three workloads and management requirements. | **Operator reference updated; closeout remains open** for visual GUI evidence, physical two-worker acceptance, fault/restart evidence, and Linux/macOS evidence. |

## Authorization Boundary

This document is an operator reference and evidence ledger, not authorization.
It does not authorize service installation/registration/start/stop, machine
policy save, pairing/approval/revocation, invitation use, listener or firewall
changes, credentials, remote job submission/commands/transfers, model/driver/
binary/config update, reboot, activation, commit, push, or deployment. Each
state-changing action needs separate explicit authorization and any required
local administrator policy grant plus operation-specific confirmation. The
worker service account is never the authorization principal. M5 administrative
actions have no validated physical operator workflow yet; no reboot, update, or
activation is claimed here. Read-only evidence commands above do not relax
this boundary.
