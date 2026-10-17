# Contributing

Thanks for your interest in contributing. We're glad to have you.

## Contributor License Agreement

Contributions to this project require you to accept the Niobium Contributor
License Agreement (CLA). You do this once — it covers all Niobium open-source
projects and all of your future contributions.

**[Review and accept the Niobium CLA](https://cla.niobium.co)**

You sign in with GitHub, so your acceptance is tied to your account. When you
open a pull request, a check named `cla/niobium` reports whether everyone
who authored a commit on it is covered. If someone isn't, the check leaves a
comment with a link.

**Contributing on behalf of a company?** If your contributions are or will be
owned by your employer, your employer must complete the agreement, and only
someone authorised to bind the company may do so. Choose "Entity" on the signing
page. Entity agreements are reviewed by a person before they take effect, so
allow a little time for that.

### If the CLA check fails on a commit you did author

That almost always means the commit's author email isn't associated with your
GitHub account. Add it at [github.com/settings/emails](https://github.com/settings/emails)
(it can stay private), or re-commit with an address GitHub already knows:

```bash
git config user.email "you@example.com"
git commit --amend --reset-author --no-edit
git push --force-with-lease
```

Then comment `recheck` on the pull request.

## Before you open a pull request

- **Open an issue first** for anything substantial, so we can agree on the
  approach before you spend time on it. Small fixes can go straight to a PR.
  Opening an issue does not require the CLA.
- **Send code as a pull request, not as a patch in an issue.** We cannot merge
  code pasted into an issue, because only pull requests go through the CLA
  check.
- **Keep pull requests focused.** One change per PR is much easier to review.
- **Include tests** where the project has them.
- **Make sure CI passes.**

## Reporting a security issue

Please do **not** open a public issue for a security vulnerability. Report it
privately as described in [SECURITY.md](SECURITY.md).

## Code of conduct

Be decent to each other. We reserve the right to decline contributions or
interactions that aren't.

## Questions

[Contact us](https://niobium.co/contact) — we're happy to help.
