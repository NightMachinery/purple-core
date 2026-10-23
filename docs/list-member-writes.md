# List member writes and account forks

Headless verification: `tests/run.sh` passed with 10,403 config checks and
1,102 sync-flow checks, zero failures. Client integration is tested separately.

`purple/purple_splice.h` exposes three file-local operations for the clients:

- `SetListMembers(text, path, list, ids, title)` writes exactly the supplied
  member order, including an explicit empty array. A missing list gets a normal
  `[lists.<name>]` table with only `members`. It never copies titles, kinds,
  presets, other lists or inherited settings into the supplied text. The list
  argument is the literal decoded name, so dots, quotes and Unicode are names,
  rather than a table path. The caller creates a new layered file at version 2.
- `ListMemberComments(text, path, list)` returns a
  `std::map<PeerIdValue, QString>` of trailing member comments without `#` or
  outer whitespace. On a line containing several members, only the last member
  can own its trailing comment. A comment after `]` belongs to the array, and
  standalone comment lines belong to no member. Duplicate ids use the first
  trailing comment found. A missing list, missing array or invalid TOML returns
  an empty map.
- `SetAccountWorkMode(text, path, account, mode)` changes one device-local
  `[account_work_mode]` value to `own`, `follow` or `stock`. It creates the table
  or a quoted canonical account key when needed. It validates the account and
  enum, retains unrelated local choices, unknown data, comments and spacing,
  and does not touch settings files. Choosing `own` writes an explicit value;
  a missing entry already means `own` when read.

Every writer reparses its output and compares the complete expected TOML tree,
including unknown data. A refusal returns the unchanged input and an error;
setting an existing value returns unchanged text without an error. Existing LF
or CRLF line endings are preserved. Neither operation performs file I/O.

## Preservation and refusals

First-line value replacements preserve an input UTF-8 BOM. Literal list names
are escaped without whitespace normalization, including CR/LF and other control
characters, so an inherited escaped name can be copied into an own-list fork.

For a normal one-member-per-line array, retained member lines keep their
spelling, indentation and comments. They move with the requested member order.
Independent comment and blank lines stay in the array; removed member lines
and their own trailing comments go away. A missing comma is inserted when a
retained line needs one. Only new member lines use the title callback.

Compact or otherwise noncanonical arrays are normalized inside their bracketed
span. Retained members keep their trailing comment text, independent comments
survive, and all bytes outside the brackets stay unchanged. New member comments
come from the title callback, with line breaks replaced by spaces. Copied
comment text retains its internal spacing, including tabs. Supplying
an unchanged member order is a byte-preserving no-op, even for compact arrays.
Invalid TOML, scalar/inline list tables and noninteger member arrays are refused.
Adding `members` requires a normal explicit list header. An existing array in a
dotted assignment can be replaced without changing its address.

An existing local-choice value can also be replaced inside a dotted assignment
or inline table. Adding an entry to such a table requires giving it a normal
explicit `[account_work_mode]` header first. This avoids rewriting other choices.

## Fork and reset

For the first edit of an inherited list, the client reads the effective members
and `ListMemberComments` from the inherited file. Its title callback returns
those comments for copied members and a fresh title for the edited chat from
that chat's own account. It passes the resulting member order to
`SetListMembers` on the account's own file. No core call discovers accounts or
looks up names from other sessions.

`follow` and `stock` change only the local choice. An own-list fork remains in
its account file and can resume when the account returns to `own`. Clients
apply the existing `AccountWorkModeFor` and layering policy to those choices;
`follow` on the global account still reads as `own`.

To delete a safe members-only fork, clients use the unchanged
`RemoveTableKey(text, path, table, "members")` with a TOML table path such as
`lists."work.one"`. This is distinct from the literal list-name argument above.
Reset removes an empty uncommented header and retains unrelated text. Clients
must restrict this action to a layered, members-only fork so that it cannot
empty a list defined by an account's whole file on another device.

The existing `tests/run.sh` exercises whole and layered files, explicit empty
members, inheritance and reset, follow/stock/own choices, quoted and Unicode
names, member and standalone comments, unknown data, LF/CRLF, no-op round trips
and atomic refusals. The full-tree reparse check runs in each production splice.
