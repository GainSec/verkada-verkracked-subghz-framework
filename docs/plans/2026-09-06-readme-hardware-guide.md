# README Hardware Guide Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add practical, accurate README guidance covering the target BH hardware, related alarm devices, and the SDR equipment needed to use the passive receive tooling.

**Architecture:** Keep the root README as the newcomer-facing overview and retain `docs/HARDWARE-SUPPORT.md` as the detailed capability matrix. Clearly distinguish vendor ecosystem context from capabilities implemented and validated by this repository, without splitting the README into excessive support classifications.

**Tech Stack:** Markdown, official Verkada product documentation, CMake/HackRF build configuration.

---

### Task 1: Add the newcomer-facing hardware guide

**Files:**
- Modify: `README.md`

**Step 1: Add target-device information**

Add a `Supported devices and required hardware` section identifying the BH61 Wireless Alarm Hub as the primary research target, linking to official setup and installation documentation, and describing BH31 as unverified rather than presenting inferred compatibility as confirmed.

**Step 2: Add ecosystem context**

Mention the BR31, BR32, BR33, BR34, BR35, and BX21 devices documented for the BH61 ecosystem, with an explicit sentence that listing them does not claim individual physical validation by this project.

**Step 3: Add practical receiver requirements**

Document HackRF One/Pro, an appropriate regional Sub-GHz antenna, USB host connectivity, and a C++20/CMake development computer. Explain that file and SigMF analysis needs no SDR, and that live HackRF RX transport is implemented but direct CLI capture remains incomplete.

### Task 2: Improve the detailed support matrix

**Files:**
- Modify: `docs/HARDWARE-SUPPORT.md`

**Step 1: Add authoritative device links**

Link the BH61 entry and ecosystem context to official vendor documentation while preserving current evidence limitations.

**Step 2: Add required-equipment guidance**

Add concise receiver, antenna, host, and regional-frequency notes consistent with the actual implementation.

### Task 3: Verify and publish the documentation change

**Files:**
- Test: `README.md`
- Test: `docs/HARDWARE-SUPPORT.md`

**Step 1: Check formatting and repository safety**

Run: `git diff --check`

Expected: no whitespace errors.

**Step 2: Check documentation links and sensitive patterns**

Verify every newly added local path exists, official URLs resolve, and no credential, private identifier, or production-targeting executable default was introduced.

**Step 3: Review the exact patch**

Run: `git diff -- README.md docs/HARDWARE-SUPPORT.md`

Expected: only the approved hardware/device documentation changes.

**Step 4: Commit and push**

```bash
git add README.md docs/HARDWARE-SUPPORT.md docs/plans/2026-09-06-readme-hardware-guide.md
git commit -m "docs: expand supported hardware guidance"
git push origin main
```

**Step 5: Confirm publication state**

Run: `git status --short --branch` and compare local `HEAD` with `origin/main`.

Expected: clean working tree and matching commits.
