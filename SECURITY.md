# Security Policy

## Responsible Use

Position-Independent Agent (PIA) is designed for **legitimate security research, authorized penetration testing, embedded systems development, and educational purposes**. Users must ensure they have proper authorization before deploying PIA in any environment.

## Supported Versions

Security updates are applied to the latest version on the `main` branch only. We do not maintain separate release branches at this time.

| Version | Supported |
|---------|-----------|
| `main` (latest) | Yes |
| Older commits | No |

## Reporting a Vulnerability

If you discover a security vulnerability in PIA, please report it **privately** rather than opening a public issue.

### How to Report

1. **Email:** Send a detailed report to the project maintainer (see GitHub profile for contact information)
2. **GitHub Security Advisories:** Use [GitHub's private vulnerability reporting](https://github.com/mrzaxaryan/Position-Independent-Agent/security/advisories/new) to submit a confidential advisory

### What to Include

- A clear description of the vulnerability
- Steps to reproduce the issue
- The affected platform(s) and architecture(s)
- Any potential impact or exploit scenario
- Suggested fix, if available

### Response Timeline

- **Acknowledgment:** Within 48 hours of receiving the report
- **Initial assessment:** Within 7 days
- **Fix or mitigation:** Depends on severity and complexity; we aim to resolve critical issues as quickly as possible

### What to Expect

- We will acknowledge your report and keep you informed of progress
- We will credit you in the fix (unless you prefer to remain anonymous)
- We ask that you do not publicly disclose the vulnerability until a fix is available

## Scope

The following are considered in scope for security reports:

- Vulnerabilities in the command protocol (buffer overflows, out-of-bounds reads, etc.)
- Path traversal or unauthorized file access via command handlers
- WebSocket communication security issues
- Memory safety issues (buffer overflows, use-after-free, etc.)
- Issues that could lead to unintended code execution or privilege escalation
- Flaws in the position-independence guarantees (e.g., unexpected data section generation)

The following are **out of scope**:

- Issues in third-party tools (LLVM, CMake, Ninja)
- Misuse of PIA for unauthorized purposes
- Vulnerabilities that require physical access to the target machine
- Social engineering attacks

## Disclosure Policy

We follow a coordinated disclosure process:

1. Reporter submits vulnerability privately
2. We acknowledge and assess the report
3. We develop and test a fix
4. We release the fix and publicly disclose the vulnerability
5. Reporter is credited (if desired)
