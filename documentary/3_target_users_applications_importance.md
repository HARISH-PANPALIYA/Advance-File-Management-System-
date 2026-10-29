# 3. Target Users, Applications, and Importance

## 3.1 Who SafeVault FS is made for

### Software engineers

Developers frequently run scripts, refactoring tools, migrations, and build
commands that can modify many files. SafeVault provides an additional
recovery layer beyond source control.

### Data scientists and researchers

Experiments often overwrite datasets, notebooks, generated reports, and model
outputs. Historical versions help compare and recover experimental states.

### Enterprise security teams

Security teams need evidence about which process changed a file and when.
Process attribution and a tamper-resistant journal support investigation.

### System administrators

Administrators can monitor important directories and inspect operational
activity without manually creating backups for every change.

### Power users and consumers

Users can recover accidentally changed documents and find files through
natural-language descriptions instead of exact filenames.

## 3.2 Applications

- Developer workspace protection.
- Research and laboratory data recovery.
- Document and records protection.
- Configuration rollback.
- Build and deployment investigation.
- Malware and ransomware detection.
- Compliance and audit support.
- Digital forensics.
- Local semantic file search.
- Educational demonstrations of operating-system concepts.

## 3.3 Importance of the project

### Data loss prevention

The most important benefit is reducing the impact of accidental or malicious
data destruction.

### Faster recovery

Users should be able to restore a previous state without searching through
manual backup folders.

### Better accountability

Recording process and timestamp information makes changes more explainable.

### Reduced backup duplication

The planned chunk-level deduplication system will store identical content
once while allowing multiple versions to reference it.

### Security visibility

Ransomware protection depends on observing behavior over time. A journal gives
the threat-detection layer the history required to recognize unusual activity.

### User-friendly access

The AI search interface makes filesystem history accessible to users who do
not remember exact paths or filenames.

## 3.4 Educational importance

SafeVault FS combines several operating-system topics in one project:

- Filesystem interfaces.
- System calls.
- Event monitoring.
- Persistence.
- Process management.
- Concurrency.
- Access control.
- Recovery.
- Security monitoring.

It therefore provides a practical platform for demonstrating how low-level
OS facilities become user-facing reliability features.
