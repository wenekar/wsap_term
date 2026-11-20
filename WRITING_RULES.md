# Writing rules

This guide governs documentation, code comments, and work summaries in `wsap_term`, including the public API comments in `subprojects/wsap-framework`.

## Facts and explanations

Put lookup facts such as defaults, limits, units, and return values in tables or labeled lines when readers need to scan for them. Use prose for reasoning, interactions, and exceptions. In a short API comment, keep a condition with its consequence rather than splitting it into a table. Omit the period after a table cell or list item that is a phrase. Punctuate complete sentences normally. Use a labeled line for a single note.

## Language and terminology

1. **Use standard technical English.** Prefer short sentences, active voice, and one point per sentence. Keep established terms such as “rasterizes,” “device pixels,” and “allocation failure.” Explain local terms instead of replacing standard terms with imprecise paraphrases.
2. **Use verbs that describe the operation.** Write “returns,” “clips,” “invalidates,” or “borrows” instead of “gives,” “handles,” or “uses” when the precise verb matters. Distinguish allocating, copying, and borrowing.
3. **Use natural articles, countable nouns, and precise modals.** Use “a” or “an” for a generic singular and “one” for a count or limit. Treat “text” as a mass noun; count strings, code points, or glyphs as appropriate. Use “can” for capability or permission and “must” for a requirement.
4. **Give pronouns clear antecedents.** Name the object on first mention. Use a pronoun when it has one possible referent. Repeat the name when two objects could fit.
5. **Make the syntax unambiguous.** Keep words such as “that” when they prevent a misreading. Give time limits an explicit anchor: “until the callback returns,” not “until then.” Put conditions next to the behavior they govern.
6. **Name the actor.** Distinguish a requirement on the caller from a check the function performs. “Call this after all surfaces stop using GPU presentation” states a precondition; “releases the device when no surface uses it” can imply a runtime check.
7. **State each fact once.** Cut prose that repeats a heading, declaration, or table label. Remove emphasis that adds no fact. Use “only” for an actual restriction, not a contrast. Use a colon for a short explanation when it makes the relationship clearer.
8. **Define concepts before relying on them.** Introduce a type or concept before discussing its exceptions or contrasting it with a related term. If a forward reference is necessary, add a short definition or link.
9. **Use US English and consistent headings.** Use the Oxford comma. Preserve the spelling of identifiers and quoted upstream names. Use one H1 per Markdown document.
10. **Define each term by its class and distinguishing property.** For example, a glyph cache slot is a storage location for one glyph or tile. Do not define the term by a field name or source path.
11. **Use one concept per glossary row.** Start definitions with a noun phrase for nouns and a verb phrase for verbs. Group headwords only when they are true synonyms.
12. **Distinguish concepts from their spellings.** Define the concept before citing its representation. In Markdown, use backticks for literal identifiers, paths, and values, and quotation marks for words discussed as words and verbatim UI labels.
13. **Disambiguate overloaded terms in prose.** Write “glyph cache slot” or “font face slot” when “slot” could mean either. Do not rely on capitalization, backticks, or nearby code to distinguish meanings.
14. **Define related terms in parallel.** When two terms contrast, explain the distinction from both sides. An owned reference must be released by its recipient; a borrowed reference conveys no release obligation and stays valid for a specified lifetime.
15. **Separate stable definitions from implementation status.** Keep phrases such as “as today,” “not yet,” and “for now” out of definitions and API contracts. Put temporary status in a separate note pinned to a commit or ISO date. State behavior that callers can rely on without a historical qualifier.

## Code comments

### Public API comments state the contract

Place the contract next to the declaration so callers can use the API without reading its implementation. Start a function comment with a verb such as “Creates,” “Returns,” or “Releases.” Start a type or constant comment with a noun phrase that identifies what it represents. Use the header's existing comment syntax.

Document the details a caller needs, where applicable:

| Topic | Contract details |
|---|---|
| Inputs | Accepted ranges and units, coordinate space, and the meaning of `NULL`, zero, or other sentinel values |
| Ownership | What the function copies, borrows, or takes ownership of; who releases each resource; whether ownership changes on failure |
| Lifetime | How long inputs and results stay valid, and which calls or events invalidate them |
| Call order | Required initialization, caller preconditions, thread affinity, and concurrency restrictions |
| Callbacks | When callbacks run, how long callback data stays valid, and which calls are safe during the callback |
| Results | What each status or sentinel means, what outputs are valid on failure, and whether failure leaves partial changes |

A declaration already names its parameters and types. Do not repeat them unless the comment adds a constraint or explains their meaning. For example, the comment for `glyph_cache_shape_segment()` should tell callers that a span is valid until `emit` returns and that `emit` must not call `glyph_cache_shape_segment()` recursively. Keep the explanation of shared scratch buffers in `subprojects/wsap-framework/docs/font-api.md`.

Check the implementation and relevant tests before stating a guarantee. If they disagree with existing documentation, report the discrepancy rather than treating incidental behavior as a contract.

### Inline comments explain why

Explain a non-obvious reason, constraint, or trap. Do not narrate an assignment, restate a function name, label a block that is already clear, or record change history. Preserve tool directives and their syntax. A TODO states what needs to change and why.

## Documents and summaries

Start a document with a sentence that states its scope. Write for someone who was not part of the discussion. Use descriptive headings and link directly to the source of a claim when it helps the reader verify it. State findings separately from proposed fixes. In a work summary, report what changed, what was checked, and any limits on the check.
