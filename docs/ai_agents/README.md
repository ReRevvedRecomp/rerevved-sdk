# Contribution guidance

Read [Contributing](../README.md#contributing), [Development](../development.md),
and the [style guide](../style-guide.md) before proposing a change.

- [Comments and prose](comments-and-prose.md) sets writing and ownership rules.
- [Evidence and claims](evidence-and-claims.md) defines verification and
  reporting.
- [Test authoring](test-authoring.md) defines test scope and fixture selection.

Keep SDK changes reusable. Title-specific behavior belongs in the title
repository. Work only within the requested scope and permissions; a local edit
does not authorize publication or changes to dependencies.

Do not rewrite inherited upstream documentation for style alone. Preserve
upstream disclaimers, credits, provenance, and license files. Do not edit
generated output, vendored trees, or submodule contents as ordinary SDK source.
Use [maintenance policy](../MAINTAINING.md) for upstream integration and exact
consumer locks. Submit SDK pull requests into `main`.
