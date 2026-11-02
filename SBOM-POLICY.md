# Source SBOMs

This repository generates its **own** source inventory for every published release,
including patch, minor, major and prerelease versions. It does not substitute a
parent application's SBOM for this repository's evidence.

- [Release assets](https://github.com/wave-owl/fldigi-olivia-core/releases):
  `SBOM.cdx.json` (CycloneDX 1.6) and `SBOM.md` (readable report).
- [Automation](https://github.com/wave-owl/fldigi-olivia-core/actions/workflows/sbom-release.yml):
  published-release events, daily reconciliation and manual existing-tag backfill.
- [Declarations](.sbom.json): exact gitlink mappings, vendored provenance, licence
  evidence, models and unresolved external build/runtime requirements.

The readable report is generated automatically from the same immutable source and
configuration as the JSON. Existing complete asset pairs are preserved. Daily
reconciliation also covers releases made using `GITHUB_TOKEN`, which do not trigger
another release workflow. Schedules must remain enabled in GitHub Actions.

Source is inventoried without executing its build, package hooks or application
code. Tracked source, exact Git identities, recursive gitlinks and declared
vendored/model files provide evidence; unpinned external requirements remain
unresolved. Licence-file evidence is retained, not replaced by an inferred grant.
Composition completeness is **UNKNOWN**: this source inventory does not establish
OS-package, runtime or static-link closure of a compiled release artifact.

## Reproduce locally

Use Python 3, Git, an authenticated `gh`, and Syft 1.55.0 with a clean existing
release-tag checkout:

```sh
python3 scripts/repository_sbom.py --root . --config .sbom.json --output-dir build/sbom
```

For an older release, use its clean checkout as `--root` and the trusted default
branch's generator/configuration. Cross-repository private evidence requires
explicit authorized read access; the workflow deliberately supplies no personal
token and fails visibly rather than publishing an incomplete inventory.
