# Skills and development-process review — 2026-10-02

## Verdict

[INFERENCE] Keep Matt Pocock's targeted engineering skills, but simplify the surrounding execution process. The clearest skill-linked episode supports better debugging. The clearest process problem is repeated human orchestration between experiments. The records do not establish that Matt's skills caused that orchestration problem, nor do they measure a net productivity gain.

## Scope and attribution

Parsed 23 top-level Spirula-related OMP session logs with messages from September 27 through the review snapshot, including early October 3 UTC messages belonging to the ongoing October 2 local session. Extracted all user requests, assistant text and explicit skill-read calls; inspected selected numerical tool outputs. The sample contains 6,057 message records, not 6,057 independent tasks. Child sessions were not exhaustively counted. The latest execution was still active and is not classified as failed or prematurely completed.

The installed `.skill-lock.json` identifies `mattpocock/skills` as the source of the relevant engineering skills, installed September 27. It separately attributes Ponytail to `DietrichGebert/ponytail`, installed minutes later. Spirula's validation skill and repository rules are separate again. Installation provenance is not proof of authorship of every component. There is no matched pre-installation control in this review. Model configuration changed during the period; hardware, storage, task difficulty and evolving plans also confound attribution.

Explicit top-level `read` calls, including reads inside parallel tool calls:

| Skill | Read calls | Distinct sessions |
|---|---:|---:|
| Matt: diagnosing-bugs | 12 | 9 |
| Matt: code-review | 8 | 7 |
| Matt: codebase-design | 9 | 6 |
| Matt: research | 1 | 1 |
| Separate: ponytail | 34 | 22 |
| Separate: spirula-validation | 21 | 15 |

No explicit reads of `to-spec`, `to-tickets`, `implement`, `tdd`, or `handoff` appeared in the sampled top-level records. No additional absolute-path skill-file reads were found. This does not prove that child agents never used them or that their ideas had no indirect influence. It does mean that calling the observed plan/handoff cycle an execution of Matt's full workflow would be unsupported.

## What helped

### Concrete debugging rather than loosening acceptance

The September 30 G1 execution initially failed packed-state parity despite in-tolerance gradients. After reading `diagnosing-bugs`, the assistant stated three falsifiable hypotheses: raster accumulation order, image-dependent optimizer state, and packet/snapshot restoration. It replayed retained gradients and state through the native updater. Tool output identified `saved_outputs_and_repeats_exact` and the first divergence at `raster_screen_gradients`. Subsequent proof outputs reported zero canonical byte differences on the source and edge cases. The ordered path was described as proof-scoped, not a general production fix. [S1]

[INFERENCE] This is the strongest evidence that a Matt skill was useful: its prescribed hypothesis/replay structure is visible in the actions, followed by concrete diagnostic and parity results. It still does not establish what the same agent would have done without the skill.

### Negative results remained negative

The G1 comparison reported 801.617 s native versus 5,640.418 s staged G1, rejecting that schedule as 7.036× slower rather than declaring success because the implementation passed checks. [S1]

The October 1 B2 qualification reported a 1.6024 dB mean raw-PSNR deficit over 132 matched views, plus failed recovery screens, and withheld release approval. The October 2 causal run distinguished exact immediate save/load from divergence in both save/load and no-save continuations; it did not falsely declare the original recovery problem repaired. [S2, S3]

These are useful engineering outcomes consistent with evidence-driven skills. Their acceptance criteria also came from the plans and Spirula rules; credit cannot be assigned exclusively to Matt's bundle.

## What hurt the process

### The user repeatedly had to coordinate continuation

The same planning thread contains requests for autonomous completion on October 1 at 02:30 and 20:50 UTC. Another session repeats the request on October 2 at 15:20: “ALL of our work can run autonomously until we have settled on a split-gpu path.” The eventual plan update explicitly says a failed experiment closes that experiment, not the campaign. [S4, S5]

[INFERENCE] The workflow was better at closing a bounded experiment than owning the user's broader decision. This imposed observable steering work on the user. It is not evidence that every stop was wrong: some plans required stopping at a failed gate, the user explicitly requested several handoffs and documentation updates, and the October 2 pause was expressly requested.

### Planning and evidence maintenance had real costs

Several sessions ended with planning-only updates and link checks rather than execution: for example, September 30 11:33 expanded C0–C4 and checked 46 links; September 30 15:07 created O0–O3 and checked 72 links; October 2 15:18 expanded the autonomy contract and checked 63 links. Those actions answered the immediate user requests. [S5, S6, S7]

[INFERENCE] The concern is repeated synchronization across a master plan, execution plan, scorecard, usage record and index—not that a link check is intrinsically wasteful. The records do not provide a defensible percentage of time wasted.

Storage pressure also produced repeated cleanup requests and an admission block. The October 2 handoff reported 1.77 GB reclaimed but only 14.43 GB free against a 15.03 GB admission requirement. The next run's user asked both “What are we blocked by?” and “Are we stuck or still working?” [S8, S9] Storage and visibility are concrete problems; blaming them on a debugging skill would be speculation.

## Skill-text mismatches worth adapting

These are instruction-level risks, not demonstrated causes of the observed delays:

- `diagnosing-bugs` requires a red-capable loop before hypothesis generation, calls for seconds rather than minutes, and requires 3–5 hypotheses. A tight native replay is valuable, but full GPU-training qualification and stochastic numerics cannot always satisfy a deterministic seconds-long loop. Allow bounded measured counterfactuals when a smaller reproducer cannot preserve the phenomenon.
- `code-review` prescribes a fixed-point `...HEAD` diff and two review axes in separate agents. A commit-only diff is insufficient for uncommitted worktree changes. Review the actual changed surface; reserve two independent reviewers for changes whose risk warrants them.
- `codebase-design` usefully rejects hypothetical seams, but mandates exact terminology. Vocabulary enforcement is not a delivery outcome.
- `research` mandates delegation and an in-repo Markdown finding. That can be appropriate for a substantial investigation, but would add avoidable artifacts to a quick factual question. This review follows that instruction; its note is not itself evidence of earlier harm.

Sources: installed `diagnosing-bugs/SKILL.md`, `code-review/SKILL.md` (fixed point and two-axis process), `codebase-design/SKILL.md` (glossary and seam principles), and `research/SKILL.md`. The installation lock is the provenance source, not an upstream-current version claim.

## Remediation plan

**Status:** planned; configuration changes and trial remain pending. This section is the authoritative remediation plan. Its objective is fewer avoidable user interventions and less rework while preserving numerical, recovery and publication guarantees.

### Decisions and attribution

- Retain targeted diagnosis, substantive review and design guidance. S1 supports a concrete diagnosis-skill-to-action connection; negative experimental results are useful decisions, not failed productivity.
- Target the combined execution instructions and campaign completion contract. The sampled OMP records do not support blaming Matt's spec/ticket/handoff workflow for the continuation loop. Explicitly requested handoffs, authorized stop gates and resource blocks remain legitimate.
- Keep the OMP counts above separate from the earlier OpenCode audit. That audit found repeated review-skill loads in child review sessions [R1–R3]; this sample counts top-level OMP reads, includes parallel calls and has a later snapshot. Neither sample measures a causal productivity effect, and their counts must not be pooled as a common denominator.

### 1. Own the campaign decision and consolidate live status

**Location:** the active campaign's master plan and existing evidence index, with continuation behavior in each client's global execution instructions.

- Carry forward the October 2 autonomy contract [S5]: a failed candidate closes that candidate, then the execution owner pursues the next authorized alternative. Finish at the agreed decision, explicit user pause, or a concrete resource, authority or material-decision blocker.
- Keep one live checkpoint: authoritative plan and worktree/revision, current milestone, acceptance criteria, observed evidence, blocker and next bounded action. A continuation reads this checkpoint and the evidence needed for its next action.
- Other execution records retain their historical results; scorecards, usage records and indexes point to live status rather than duplicate it. Preserve evidence identities and dependencies when replacing duplicated current-status prose.
- Report whether work is running, waiting or blocked when the state changes. Name the exact resource or user action needed to unblock it; unavailable resources mean blocked, not settled. Cleanup still requires the applicable authorization and evidence-retention checks.

**Acceptance:** an authorized rejected arm advances without another permission request; a genuine blocker produces a precise handoff. Exactly one document owns current campaign status, and historical results remain traceable. A planning deliverable is terminal only when planning was the requested scope.

### 2. Make skill routing proportional to the task

**Locations:** OpenCode's global `AGENTS.md` and `opencode.json`; OMP's verified global instruction entry point; shared installed `skills/<name>/SKILL.md` entry points.

- First resolve both clients' active instruction and skill sources, override precedence, and installation/update ownership. Verify whether each loader honors invocation metadata: `implement-spec` already declares `disable-model-invocation: true`. Record any loader limitation rather than adding the same ineffective flag again.
- Use focused diagnosis, review and design guidance when the problem warrants it. Routine implementation follows repository instructions directly. Full spec/ticket/worktree/PR orchestration is explicit opt-in and remains subject to repository publishing authorization.
- Narrow broad skill descriptions to those branches. Reuse instructions already available in context; reload when their content is missing or changed, not merely because another turn began.
- Keep routing policy at user level and project-specific acceptance in the repository. Extend existing instructions instead of adding another router skill or parallel policy document.

**Acceptance:** fresh sessions in both clients route a routine edit, substantive review, hard numerical bug and explicitly requested full workflow appropriately. Verify observed skill calls and loaded sources; a metadata edit alone is not proof of routing behavior. No publishing action is needed to test selection.

### 3. Bound review and adapt diagnosis without weakening evidence

**Locations:** OpenCode's review-agent definition in `opencode.json`, OMP's `agents/reviewer.md`, and the installed `code-review` and `diagnosing-bugs` skill instructions.

- A delegated reviewer completes its assigned question directly. Review staged, unstaged and relevant untracked changes when that is the requested surface; a committed `...HEAD` diff alone is insufficient for working-copy review.
- Default to one focused review. Use two independent reviewers when distinct numerical, checkpoint or publication risks justify separate questions. Preserve separate standards/spec reviews when that full workflow is explicitly requested.
- Re-review fixes and affected behavior incrementally. At the first compaction or scope expansion, return retained findings, verification limits and the next unresolved question rather than restarting completed scans. Keep suspected and verified findings distinct.
- Prefer a tight red-capable reproducer. When a seconds-long deterministic loop loses the GPU-training phenomenon, use a bounded native replay or measured counterfactual that preserves it; record runtime, variability and what the probe can establish. Generate hypotheses that distinguish plausible causes rather than filling a quota.
- Preserve supported-device, numerical, quality and recovery acceptance. A diagnostic shortcut cannot replace the full qualification it was designed to explain. Design vocabulary and quick factual research should support the immediate decision rather than mandate new artifacts or delegation.

**Acceptance:** a focused reviewer returns actionable findings and verification gaps without invoking another full review workflow; a follow-up inspects the changed behavior. A numerical investigation records discriminating evidence and keeps unresolved recovery or quality failures unresolved.

### 4. Evaluate the next five comparable development sessions

Record one compact row per session in the existing evidence index, linking its transcript and delivered decision. Use the same definitions for selected historical comparison sessions; mark unavailable measurements unknown.

| Measure | Record |
|---|---|
| User interventions | Requests needed to restart already-authorized work or restore scope; separately count material decisions, resource/authority blocks and user-requested pauses. |
| Time to first useful execution | Request to first relevant test, replay, runtime inspection or experiment; separate blocked/waiting time and classify planning-only requests separately. |
| Rework | Reopened defects, invalidated evidence and repeated investigation of settled questions. |
| Verified outcome | Delivered behavior or evidence-backed candidate/campaign decision, remaining blockers and acceptance still unproven. |

Also check whether live status remained singular and negative results led to the correct next action. Skill loads, links checked, documents updated and plans completed are diagnostic context, not success metrics. Record client/model, task class and resource constraints so unlike sessions are not presented as a controlled comparison.

**Acceptance:** record the five-session comparison with transcript-backed examples of remaining friction and any rigor lost. Aim for zero avoidable continuation requests and retain changes only where observed behavior supports them; revise or revert rules that suppress useful review or relax acceptance. Five sessions provide a bounded trial, not proof of causation.

### Current checkpoint

- **Plan/revision:** this section, branch `black-panther`, base `8f8c0f5a`; documentation-only integration.
- **Milestone:** findings integrated; remediation steps 1–4 unexecuted.
- **Evidence:** S1–S9 support the OMP assessment; R1–R3 locate the separate review-loop examples. Historical tests were not rerun.
- **Blocker:** none to planning. OMP's global instruction entry point and both clients' effective routing remain to be verified before configuration edits.
- **Next action:** resolve active client instruction/skill sources and the active campaign's status owner, then implement the smallest scoped changes when remediation execution is requested. This planning request does not launch a training campaign.

No skills, runtime configuration, or project policy were changed by this review or plan integration.

## Primary session references

Session IDs and message IDs below locate evidence in local OMP transcripts without recording private filesystem paths. Times above are UTC unless explicitly described as local. Historical test results are transcript evidence; this review did not rerun those tests or experiments.

- **S1 — G1 implementation:** `01a0efb8-9c34-7529-be9a-9fe057bfc456`. Skill read `2c10db4d` (line 695); hypotheses `ca5232f7` (722); replay result `66402658` (799); source proof `00ff9bcf` (823); edge proof `5a705954` (824); final comparison `6414c135` (1178).
- **S2 — B2 qualification:** `01a0f7d1-f575-70f2-b814-68d473a8e690`. Diagnosis skill read `f85bca50` (167); producer/consumer gap `d95ebff5` (170); final qualification report `9b329ec7` (336).
- **S3 — B2 causal investigation:** `01a0fa7f-6894-736e-b064-85653f1b1338`. Skill reads `fc9f0b4a` (6); late-state probe report `222b453f` (489); final disposition `c95f115d` (602).
- **S4 — Repeated planning/continuation:** `01a0f372-824b-71f5-8a65-3bd7c4d89fba`. Autonomous-completion requests `b056167f` (113) and `e247accf` (369); next-step question `820b9a08` (446); view-policy plan completion `e6461877` (543).
- **S5 — Autonomy contract update:** `01a0fd31-c6f1-77ed-b369-9c751d391454`. User request `30a749f3` (19); completed plan update `d8268c3b` (118).
- **S6 — G1 execution-ready plan:** `01a0f217-1398-7314-9cff-c5d2b51c59a4`, final `38980e52` (90).
- **S7 — O1 screen plan:** `01a0f2da-d034-77d8-939f-231ac2282bac`, final `4532bc61` (65).
- **S8 — Requested pause and storage-bound handoff:** `01a0fd41-8c1e-70dc-84e4-c3ff6c2dadd8`, user pause/handoff request (368); final `9e1e778b` (452).
- **S9 — Active continuation:** `01a0fd78-257b-71b7-8093-7a97c02753c3`, user blocker question (120), cleanup request (124), progress question `207cb997` (547). Later messages show ongoing execution; no final outcome is inferred.

Separate OpenCode audit references (not included in the OMP counts above):

- **R1 — Review B1 timing proof:** `ses_f1526390effeYenJ1D7Bne4ekC`; four `code-review` loads and five context summaries; final static findings included incomplete merge coverage and non-resumable default stop checkpoints.
- **R2 — Review final B1 proof:** `ses_f1502cfd6ffeHGwjMOOzFZ5RYb`; four `code-review` loads and four context summaries; final static findings included missing source identity and a stop-endpoint defect.
- **R3 — Review source-bound B1 gate:** `ses_f14edb08fffet0wGa2iLshlp6E`; five `code-review` loads and seven context summaries; final static findings included contract binding and no-overwrite publication risks. The final review withdrew an earlier suspected repeated-resume hash failure.
