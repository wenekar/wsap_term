# Comment cleanup

When the user asks to delete or clean up comments, reduce comments to
practically zero across the entire project, not only the file currently under
discussion.

Apply the cleanup to first-party source files, headers, tests, scripts, build
files, and ignore files. Remove narrative explanations, section dividers,
restatements of names or code, test-case descriptions, inline annotations, and
comments attached to closing preprocessor directives.

Preserve only content that is not an ordinary code comment:

- Copyright and license notices.
- Comments in vendored or third-party files.
- Documentation in generated protocol sources and headers.
- A short provenance marker identifying a generated file and its generator.
- Script shebangs.
- Preprocessor directives and other syntax that merely begins with `#`.

Do not edit generated or vendored files solely to remove their comments. If a
first-party generator emits the preserved provenance marker, leave both the
generator and generated output consistent.

Start with a project-wide `rg` inventory. Exclude `.git`, dependency trees,
vendored sources, and known generated files as appropriate. Distinguish actual
comments from preprocessor directives, shebangs, and comment-like text inside
strings such as URLs.

Perform removal with syntax awareness. Do not use a blind expression that can
delete comment delimiters occurring inside string or character literals.
`python3 tools/strip_comments.py FILE...` removes comments with syntax
awareness. It keeps license and generator headers. It skips shell scripts,
so clean those by hand.
Preserve surrounding tokens when removing an inline comment, inserting
whitespace when necessary. Remove trailing whitespace and whitespace-only
residue left by deleted comments.

After cleanup:

1. Repeat the project-wide `rg` inventory and inspect every remaining match.
2. Confirm that remaining matches belong only to the exceptions above or are
   false positives such as URLs and directives.
3. Run `git diff --check`.
4. Build the project and run the relevant test suite.
5. Report the retained exception categories and any unrelated test failures.

Do not discard or overwrite unrelated working-tree changes while performing
the cleanup.
