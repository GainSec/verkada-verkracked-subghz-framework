# Standalone Sub-GHz Public Release Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Produce a complete, functional, sanitized standalone public release of the BH-series Sub-GHz interoperability framework.

**Architecture:** Reconstruct the public tree from an explicit allowlist of reviewed independently authored source, tests, fixtures, and research models. Replace private evidence dependencies with safe synthetic/reconstructed provenance and public documentation, validate out of tree, then create one fresh root commit.

**Tech Stack:** C++20, CMake/CTest, Python 3, JSON, SigMF, optional libhackrf, Semgrep and repository-specific structural audit scripts.

---

### Task 1: Inventory and classify the private source

**Files:**
- Inspect: source ZIP and extracted temporary inspection tree
- Create outside public repository: `../PUBLIC_RELEASE_AUDIT_WIRELESS_SUBGHZ.md`

1. Inventory paths, sizes, MIME types, executables, symlinks, archives, opaque
   blobs, fixtures, dependencies, credentials, identifiers, and embargo terms.
2. Record allowlist and omission decisions without recording sensitive values.
3. Verify the source ZIP and its original repository remain unchanged.

### Task 2: Reconstruct the executable framework

**Files:**
- Create: `CMakeLists.txt`, `cmake/Bh61Options.cmake`
- Create: reviewed files under `include/bh61/`, `src/`, and portable `tests/`

1. Copy only reviewed C++/CMake files through an explicit allowlist.
2. Search the copied content for private paths, identifiers, endpoints, secrets,
   proprietary excerpts, and vulnerability references.
3. Apply the minimum sanitizing edits needed while preserving behavior.

### Task 3: Curate safe fixtures and research models

**Files:**
- Create: reviewed `fixtures/phy/`, `fixtures/protocol/`, `fixtures/passive/`
- Create: safe `research/efr32/` model/generator files
- Create: safe tests under `tests/research/`

1. Trace each fixture to its generator or source description.
2. Retain only synthetic or independently reconstructed fixtures.
3. Regenerate and hash retained corpus files where practical.
4. Exclude captured, vendor-derived, exact-evidence, and unknown-provenance data.
5. Preserve analytical and physical-validation boundaries in machine-readable
   sidecars and tests.

### Task 4: Build the public documentation set

**Files:**
- Create: `README.md`, `AUTHORS.md`, `SECURITY.md`, `CONTRIBUTING.md`
- Create: `LICENSE-TODO.md`, `PUBLIC_RELEASE_NOTES.md`, `.gitignore`
- Create: `docs/ARCHITECTURE.md`, `docs/PROVENANCE.md`
- Create: `docs/FIXTURES.md`, `docs/HARDWARE-SUPPORT.md`
- Create: `docs/RESEARCH-METHODOLOGY.md`

1. Document passive/RX scope, build, tests, HackRF, SigMF, safe capture use,
   analytical limitations, exclusions, and author attribution.
2. Record claim and fixture provenance without upgrading uncertain claims.
3. Add security and contribution policies that reject sensitive submissions.
4. Record unresolved licensing rather than inventing a license.

### Task 5: Validate functionality outside the public tree

**Files:**
- Build outside repository: temporary CMake build directory

1. Run CMake configure with hardware disabled.
2. Build all targets and run CTest with failures visible.
3. Run retained Python research tests.
4. If libhackrf is installed, configure/build its RX-only backend; otherwise
   record that hardware-dependent validation was not run.
5. Run available static analysis and formatting checks without rewriting code.

### Task 6: Perform the adversarial release audit

**Files:**
- Update outside public repository: `../PUBLIC_RELEASE_AUDIT_WIRELESS_SUBGHZ.md`

1. Inventory every final file using `find`, `file`, size, and SHA-256 tools.
2. Inspect opaque files, filenames, hidden files, permissions, and symlinks.
3. Run keyword, secret, entropy, identifier, path, endpoint, and embargo scans.
4. Run installed secret scanners and manually inspect configuration/fixtures.
5. Resolve findings by omission or sanitization and rerun all affected tests.

### Task 7: Create fresh history

**Files:**
- Create: `.git/` only after all prior tasks pass

1. Initialize Git and create branch `main`.
2. Stage the complete public tree.
3. Review staged filenames, file types, statistics, and content.
4. Run the final secret scan against staged content.
5. Create one initial commit and confirm no remote is configured.
