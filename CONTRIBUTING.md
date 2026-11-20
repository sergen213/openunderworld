# Contributing

Thanks for helping improve this project. Changes reach the project only
through pull requests that are reviewed and merged into the main branch by
the maintainer.

## How to contribute

1. Fork this repository on GitHub.
2. Make your change on a branch in your fork.
3. Open a pull request against this repository's default branch and
   describe what the change does and how you tested it.

Your fork may be used only to prepare pull requests to this repository. Do
not publish or distribute your fork or modified builds separately (see
[LICENSE](LICENSE)).

## Before changing a subsystem

OpenUnderworld preserves its existing systems (SDL3, Jolt, the water shaders, every AO option, RT and the rest of
[docs/audit/PROTECTED_SYSTEMS.md](docs/audit/PROTECTED_SYSTEMS.md)) and repairs them rather than replacing them. Every
change to a subsystem answers these seven questions in its commit message or pull request (one line each is enough
for a small fix; write "n/a" with the reason where one does not apply):

```
1. Current:     how is it implemented now (files, config keys, defaults)?
2. Works:       what works today, and which tests prove it?
3. Broken:      what is wrong (BUGS.md ID, reproduction command, observed numbers)?
4. Original:    what does tru.exe do (address or Wine measurement; [I] if inferred, [U] if unknown)?
5. Change:      what is the smallest change that fixes it in place?
6. Replacement: if anything is replaced, why it cannot be fixed, what migrates, and what could be lost
                (otherwise "none")?
7. Verify:      which tests show the fix and that nothing protected regressed (`ctest -L protected`, the
                touched subsystem's suites, the Original preset's images)?
```

A replacement also needs its justification and migration plan written down before the code (docs/audit/BUGS.md
entry or the subsystem's doc); defaults never change silently.

## Contribution terms

By submitting a pull request, issue attachment, or any other contribution,
you agree that:

1. The contribution is your own original work, or you have the right to
   submit it under these terms.
2. You grant sergen213 a perpetual, worldwide, irrevocable, royalty-free,
   non-exclusive license to use, copy, modify, distribute, sublicense, and
   relicense your contribution, as part of this project or otherwise,
   under any terms sergen213 chooses.
3. You keep no right to distribute the project, or your contribution as
   part of it, beyond what the project's LICENSE allows.
4. Your contribution is provided "as is", without warranty.

If you cannot agree to these terms, please do not submit a contribution.
