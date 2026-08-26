---
name: build-rk3588-vision-app
description: Build or revise an end-to-end feature in this RK3588 visual-analysis repository. Use when a request spans requirement analysis, channel or global C++ Logic, module manifests, event reporting, delivery contracts, Web configuration, validation, packaging, or documentation, and when changes must match the repository's current implementation rather than inferred APIs.
---

# Build an RK3588 vision feature

Treat current source as the contract. Inspect before editing; never resurrect a removed path, field, module, or helper from memory.

## Route the task

- For per-frame, per-channel behavior, load [`../rk3588-channel-logic/SKILL.md`](../rk3588-channel-logic/SKILL.md).
- For cross-channel aggregation or periodic application logic, load [`../rk3588-global-logic/SKILL.md`](../rk3588-global-logic/SKILL.md).
- For reporting, delivery, Web use, services, or UI changes, load [`../rk3588-console-ops/SKILL.md`](../rk3588-console-ops/SKILL.md).
- For engine APIs, configuration, pipeline, inference, rendering, or lifecycle changes, load [`../rk3588-src-modules/SKILL.md`](../rk3588-src-modules/SKILL.md).

Read [`references/requirement-contract.md`](references/requirement-contract.md) before implementing ambiguous business behavior. Read [`references/acceptance-checklist.md`](references/acceptance-checklist.md) before handoff. Use [`references/prompt-recipes.md`](references/prompt-recipes.md) when composing a bounded task for another model or developer.

## Logic-only wizard override

When the startup prompt identifies a `develop_feature` isolated session, its allowlist overrides this Skill's broader
end-to-end scope. Write only below `vision_analysis/src/logic/modules/` and
`vision_analysis/src/logic/global_modules/`. Treat engine, config, tests, Web, services, documentation, scripts, and
generated files as read-only. Steps below that would change those locations become unsupported in that session; report
the missing capability and stop instead of asking for wider access. Read the wizard's
[`write-boundary.md`](../rk3588-feature-wizard/references/write-boundary.md) before editing.

## Establish the current baseline

1. Inspect `git status --short`; preserve unrelated user changes.
2. Locate the target source with `rg --files` and `rg`; do not assume pre-refactor directories such as `src/analyzer`, `src/core`, or `src/player` exist.
3. Inspect the target module's C++ and `logic.json` together.
4. Inspect the public header that owns the API being used.
5. For Web behavior, inspect both the FastAPI route and the React caller.
6. For reporting, inspect `vision_analysis/src/event/event_report.h/.cpp`, the module template, Adapter catalog,
   and delivery service.
7. Compare any referenced example name against actual `REGISTER_*` macros or run the built binary's list command.

Current registered source modules do not include `logic_path_sop`, `logic_periodic_snapshot_demo`, `logic_upload_teach`, or `global_two_channel_demo`. The Web still exposes an SOP node that generates `logic_path_sop`; treat that as an implementation gap, not a supported feature.

## Choose the smallest correct extension point

| Need | Extension point |
|---|---|
| One channel, each business frame | `src/logic/modules/<logic>/` |
| Cross-channel or periodic aggregation | `src/logic/global_modules/<logic>/` |
| Logic-specific user parameter | Module `logic.json.parameters` plus `param_*()` |
| Shared engine configuration | `src/config/` plus runtime/Web propagation |
| Button for one Logic | Module `actions` plus `REGISTER_LOGIC_ACTION` or `REGISTER_GLOBAL_LOGIC_ACTION` |
| Event creation | `report_event()` plus module `event_types`/`report_fields` |
| Remote request shape | Logic-owned/app report template |
| Address, token, timeout | Application `connections.yaml` through Web “应用集成” |
| HTTP/Dify transport mechanics | `service/upload/adapters/` |
| Web-only workflow | FastAPI route + `api/client.ts` + React component |

Do not hardcode endpoints or credentials in a Logic. Do not add a central config field for a parameter that belongs to one module. Do not add transport branches to the event outbox for ordinary business mappings.

## Implement in dependency order

1. Freeze the business contract: input, trigger, temporal rule, reset, deduplication, observable output, event fields, media, and failure behavior.
2. Define the module manifest contract before using its keys in C++.
3. Implement state and decision logic using the owning context API.
4. Add Action handlers only when runtime control is required.
5. Add `report_event()` only after the event type and field contract are defined.
6. Add or revise a report template only when the remote protocol is part of the requirement.
7. Outside an isolated `develop_feature` session, change Web/framework code only when metadata-driven behavior cannot
   express the requirement; inside that session, stop without changing it.
8. Outside the isolated wizard, update the matching Skill/reference when a public contract or workflow changes; inside
   it, report the documentation follow-up without editing files outside the allowlist.

## Validate proportionally

Always run the manifest check after Logic changes:

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

Then use the task-specific commands in [`references/acceptance-checklist.md`](references/acceptance-checklist.md). Generated App `logics.json` and `report_templates/` must come from the same source revision as the binary; never patch generated package files as source.

The Python files under this Skill's `scripts/` directory predate the 2026-08-22 source refactor. In particular, `validate_logic.py` incorrectly rejects currently supported global Actions; the other scripts do not cover the complete current contract. Read [`references/legacy-scripts.md`](references/legacy-scripts.md), and do not use any one of them as validation or scaffolding authority. They are unchanged because repository code was explicitly out of scope for this documentation audit.

## Handoff

Report:

- the requirement interpretation and chosen extension point;
- files changed and why each was necessary;
- runtime semantics, especially state reset, queuing, media, and delivery guarantees;
- validations actually run and any hardware-only checks remaining;
- known limitations or source/UI mismatches encountered.

Never claim remote delivery, hardware behavior, or a successful build unless it was observed.
