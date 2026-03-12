---
name: New Platform / Architecture Support
about: Request or propose support for a new platform or CPU architecture
title: "[Platform] "
labels: platform
assignees: ''
---

## Platform / Architecture

- **OS:** (e.g., NetBSD, OpenBSD, QNX)
- **Architecture:** (e.g., ppc64, loongarch64, s390x)

## Runtime Support Status

Does the [PIR runtime](https://github.com/mrzaxaryan/Position-Independent-Runtime) already support this platform?

- [ ] Yes - runtime support exists, agent just needs build presets and CI
- [ ] No - runtime support must be added first (file an issue in the runtime repo)

## Implementation Plan

Outline what needs to be implemented:

- [ ] CMake presets (`CMakePresets.json`)
- [ ] CI workflow (`.github/workflows/`)
- [ ] VSCode configuration (`.vscode/`)
- [ ] Platform-specific command handler adjustments (if any)
- [ ] README build matrix update

## References

Links to relevant documentation, man pages, or ABI specifications.

## Additional Context

Any other relevant information, known challenges, or related issues.
