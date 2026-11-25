# Integrity audit follow-up

Audit input: haiku-native at 255a06f. Working baseline: 14d782f, private Listi 0.38.0-5. No release or remote administration is implied. Existing passing tests qualify only the cases exercised. Keep normal workflows; call out any proposed behavior change separately.

| Item | Initial evidence / question | Acceptance evidence needed |
| --- | --- | --- |
| A: remote publication | VirtualOperation.cpp checks size, not bytes | Same-length corruption and failed readback must preserve the old target; test protocol interruptions |
| B: archive replacement | ArchiveJobs.cpp gates decode verification on archiveVerifyOutput | Reject mismatched decoded content before replacement; classify post-publish errors separately |
| C: cancellation | Prior VM disconnect evidence covers Move in both directions; separately inspect plain Copy from USB | Blocked read/write/flush, unrelated work, exit and remount |
| D: durable publication | File, archive and settings publication have different ownership semantics | Distinguish errors before and after rename; document filesystem limits |
| E: staging recovery | Check ownership, cleanup and visibility after crash | Preserve unrelated entries and disclose failed cleanup |
| F: rename recovery | Separate in-session undo and crash recovery | Test restart at transaction boundaries |
| G: destructive races | Identity checks do not by themselves make a rename conditional | Document irreducible races; exercise substituted entries |
| K: release qualification | Native audit, opt-in protocols, installed package and physical testing are distinct | Record artifact, commit, runner, skips and limits |
| H: resource budgets | Assess usage and existing limits before adding policy | Exercise actual byte limits without changing ordinary transfers |
| I: remote metadata | Establish request counts before optimization | Compare measured requests without weakening freshness |
| J: progress/live search | Verify scan costs and generation guards | Measure latency, avoid stale results |
| L: testable modules | Prefer separations justified by specific coverage gaps | Keep native integration coverage |
| M: metadata and connection UI | Distinguish clear reporting from workflow changes | Verify existing behavior before proposing changes |

Baseline evidence (already tested, not a rerun of the new candidate): on Haiku x86_64 0.38.0-5 passed 78 default native test programs, installed self-test 58/58, full-volume Copy and Move, and 27 network integration scenarios. These tests did not inject same-length remote corruption. New native results must identify the actual source and package exercised.
