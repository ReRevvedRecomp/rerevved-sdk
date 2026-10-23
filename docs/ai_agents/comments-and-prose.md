# Comments and prose

Write for the reader's next task. Put setup and required inputs before commands;
state what a check proves and what it leaves untested.

- Use plain ASCII and direct sentences. Wrap Markdown prose near 80 columns;
  keep lines within 100 except URLs, tables, and code.
- Give each fact one home. Link to the canonical guide instead of copying
  settings, commands, API contracts, or release policy into several pages.
- Describe current behavior. Keep work logs, migration narratives, roadmap
  promises, and assistant narration out of public documentation.
- Keep public guides self-contained. Do not name or link private repositories or
  describe private tools as available to contributors.
- Preserve identifiers, addresses, hashes, measurements, and source citations.
  Do not paraphrase a protocol value or public symbol into a different contract.
- Preserve upstream attribution, disclaimers, credits, and license text.

For a documentation-only pass, leave shortened code comments alone unless code
proves a factual error. Record the exact correction and evidence. For comments
needed by a code change, follow the [style guide](../style-guide.md#comments):
explain an invariant, ownership, lifetime, or constraint rather than restating
an operation.
