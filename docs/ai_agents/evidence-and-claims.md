# Evidence and claims

Verify behavior against the same source revision being documented. Use:

- `CMakeLists.txt` and `CMakePresets.json` for compiler, configuration, and
  target requirements.
- Workflow files for triggers, platform coverage, tests, artifacts, and
  publication guards; a preset alone is not a CI or runtime support claim.
- Cvar definitions and their readers for defaults, audiences, persistence,
  precedence, and restart requirements.
- Installed headers and implementations for public API contracts; tests for the
  cases they exercise, not a broader compatibility claim.
- [Maintenance policy](../MAINTAINING.md) for integration and publication rules.

Separate code inspection, build success, test execution, artifact publication,
and title runtime validation. Never report one as proof of another. Record
commands actually run, their results, and coverage not run. Do not build,
launch, contact a service, commit, or push when the task excludes that action.

For documentation changes, check relative links and heading anchors, ASCII,
trailing whitespace, private-reference boundaries, and unchanged licenses.
Review the result as a player following the title link, a contributor building
the SDK, and a maintainer validating a consumer. If an archive lacks history, do
not claim exact upstream provenance: preserve uncertain inherited material and
report the boundary that could not be established.
