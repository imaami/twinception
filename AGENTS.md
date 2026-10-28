# Guidelines for contributors of an agentic persuasion

This file provides guidance to agents such as ChatGPT, Claude, CodeRabbit, Copilot, and ambitious Markov chains.

## Conventions

### Attitude

- Use some time during every task to optimize, simplify, and harden.
- It's better to delete than to add LoC. Given equal performance and binary size, a red diff beats a green one.
- Elegance and performance often correlate. Trust your sense of beauty. Verify your sense of trust.
- All code is bad, everywhere, always. Never trust code you read _or_ write.
- All code is bad, everywhere, always; coders are not. Finding a bug is a happy and inclusive event.
- Undefined Behavior is a wrathful cosmic force. "It's not UB if it works" is what your bugs want you to believe.
- Use compile-time features such as `_Generic`, `typeof`, and `sizeof` to your advantage.

### C

- Prefer C23 and later. The notion that some obsolete definition is the "true C" is a golden age delusion. C is defined by the current standard.
- Use `int` only if needed. Habitual use of `int` often necessitates more integer conversions, which translates to costly sign extension instructions.
- Trace your call chains to see if you're e.g. calling `strlen()` multiple times over the same input. Measure once and pass down the variable.
- Stay aware of the program flow. Are you repeating some task more than once when you could just use a variable? Fix it.
- Use helper macros when it's justified and reasonable, but undefine macros that don't need to be exposed ASAP. Typically this means defining something above a function and undefining it below.
- If you call `strlen()` inside a loop condition or on a string literal, you must spend a full day downtown pushing a baby stroller full of boiled cabbage.
- Arrange struct members so that implicit padding is minimal. Wider types first, narrower towards the end, grouped by width.
- Be mindful of width guarantees. Given 3 struct members, an `int64_t`, a `long`, and an `int32_t`, `long` goes between the fixed-width types because its size could match either one. Remember the corner cases; e.g. `int` is only _required_ to be 16 bits.
- If a struct has trailing padding, and the last member is an integer type or `bool`, change the the type such that it occupies the padding, unless it would introduce more complexity (such as additional casts downstream).
- Prefer RAII-like variable use. Initialize variables at declaration time whenever possible, but avoid initializing with a useless value "just because".
- Don't declare variables at the top of a function out of habit. It's no longer idiomatic C. It's vestigial. Declare where it's needed, and only when it's possible to initialize with a meaningful value.
- Scope variables as narrowly as possible.
- Use anonymous structs and unions to combat nesting hell as needed.
- Practically nothing should be a `typedef`. The few exceptions when `typedef` is genuinely defensible are:
  - implementing opaque handle types in APIs (for example when library instantiation returns an instance pointer);
  - API callback function types (note: _not_ callback _pointer_ types - more about this later);
  - uniform interface semantics for C and C++ users of an API (`typedef struct Foo Foo` lets C code pretend `struct` is implicit);
  - `unsigned _BitInt()` because aligning it vertically with much shorter type names is awful, and/or you need to type it often:
    ```c
    // this is kind of ok. regrettably.
    typedef unsigned _BitInt(48) u48_type;
    ```
- Structs and unions with a tag but no type alias are great despite being somewhat more verbose to type:
  - they make it possible to have RAII initializer functions that return by value and are named like the tag;
  - an alias can be a `struct`, `union`, or a number of other things, but a tag tells the type semantics immediately.
- Function type aliases should not be pointer type aliases. While the former has its use cases, the latter is
  - unnecessary because appending an asterisk makes it a function pointer just like with any other type alias;
  - harmful because all pointer type aliases obscure pointer-ness, which is fundamentally unacceptable;
  - limited because a function type can forward declare a function but a function pointer type can not.
- `PATH_MAX` is not for defining array sizes. Don't trust its value to be reasonable.
- Always initialize and invalidate file descriptors by setting them to `-1`.
- Don't use an external flag to track a file descriptor's state. `-1` means not in use. Always uphold this rule.
- Immediately set pointers to `nullptr` after calling `free()`. Passing a null pointer to `free()` is a no-op per the C standard.
- If you must compare a signed integer variable to a `sizeof` expression, don't cast the variable to `size_t`, cast `sizeof` to the signed type instead. The former is a runtime conversion but the latter is not.
- If you keep casting signed variables to unsigned or vice versa, you're not planning ahead. Think of what you need to accomplish and pick the types based on that.
- Always check the return value of libc calls when they're not completely inconsequential. `snprintf()` returns a negative value on error and the untruncated length otherwise: check both before using the count, and never cast it to an unsigned type first.
- Do not assign to `errno` as a way to pass error information forward.
- In some cases `errno` must be cleared of stale values to guarantee its usefulness, e.g. before calling `strtoul()` and related functions. These are documented workarounds for known gotchas, not a suggestion to adopt `errno` as your error propagation channel.
- Give every atomic object a valid state exactly once (C23 7.17.2): an initializer, static or thread storage, or `atomic_init()` for allocated or mapped storage and for automatic objects declared without an initializer. Never initialize one twice, and never write over atomics in use with `memset()`, `memcpy()` or struct assignment; wiping an object right before `free()` is fine.

### Modules

- A module is a `name.c` and its `name.h`, and its functions, static ones included, are named `name_whatever()`. A second object type of the same module extends the prefix: `name_part_whatever()`.
- A function takes a pointer to its own object as its first argument.
- The life cycle functions each have exactly one meaning:
  - `struct name name (args, int *e);` initializes an object at its declaration and returns it by value, the error number in `*e`;
  - `int name_init (struct name *dest, args);` initializes storage that the caller provides and returns 0 or an error number;
  - `void name_fini (struct name *obj);` frees what the object holds, but not the object itself;
  - `struct name *name_create (args, int *e);` allocates and initializes an object and returns a pointer to it;
  - `void name_destroy (struct name **p_obj);` finalizes and frees the object, then sets the caller's pointer to `nullptr`.

  Neither `name()` nor `name_init()` allocates the object, and nothing named init returns a pointer.
- A function that can fail returns an error number, 0 on success, or the object it makes and the error number in an `int *e` argument. Words that a caller may need go into a caller-owned buffer. Never `bool` or `void` with a side channel for the error.

### Make

- Never use tabs for indentation in makefiles; tabs are syntactic in make, and will cause unexpected situations.

### Readability

- Tabs indent, spaces align: continuation lines of a declaration or argument list are tab-indented to the statement's level, then space-padded into column alignment. Don't "fix" space-aligned continuations into tabs.
- Multi-line macro definitions are the exception: indent their continuation lines with spaces, not tabs, or the aligned trailing backslashes move out of line when the tab width changes. Use 8 spaces by default; 1, 2, or 4 are fine when that keeps every line of the macro within a column limit.
- Spotting the worst cases of tab abuse is easy: anything `git grep -P '[^\t]\t' -- '**\.[ch]'` matches is wrong. But to understand line continuations you need to imagine switching tab width from 8 to 3 - do the continuation lines stay aligned? If not, think about why.
- Place angle-bracketed include statements above double-quoted ones. Separate these into groups by placing an empty line in between the two.
- A separating empty line is only mandatory between `<>` and `""` groups, but it is also allowed within these two groups at your discretion.
- Sort grouped includes - those not separated by empty lines - by header name in the C locale. Ignore whitespace so that e.g. `#include` and `# include` sort equal.
- Use Doxygen comments `/** */` to document the code for which it makes sense. Comments inside function bodies must be ordinary non-doxygenated comments.

### Repository structure

- Untracked files (check `git status`) are arbitrary local temporaries, not part of the repo — only work with repository content.
- `.gitignore` must stay sorted in the C locale (`LC_ALL=C sort`).
- Keep every commit message line at most 72 characters long.
- Runtime `assert()` belongs in tests, not in `src/`. Production code must
  enforce required error handling even when compiled with `-DNDEBUG`.

## Craplets for tasks

- To sort `.gitignore` run this _after_ `git add` but _before_ `git commit`:

  ```bash
  x=$(LC_ALL=C sort .gitignore) && {
    sha256sum <<< "$x" | grep -Fqx \
    "$(sha256sum < .gitignore)" || {
      echo "$x" > .gitignore;
      x=$(git diff -- .gitignore) &&
      {
        echo '+============================+';
        echo '[ .gitignore has been sorted ]';
        echo '+============================+';
        echo "$x";
        echo '+============================+';
        echo '[ the working tree needs you ]';
        echo '+============================+';
      } >&2;
    };
  };
  ```

- If this matches anything, your tab skills are below the 50% mark:

  ```bash
  git grep -P '[^\t]\t' -- '**\.[ch]'
  ```

- This lists the continuation lines of multi-line macros that start with a tab:

  ```bash
  git ls-files -z -- '*.[ch]' | xargs -0 awk 'FNR == 1 { c = 0 }
    c && /^\t/ { print FILENAME ":" FNR ": " $0 } { c = /\\$/ }'
  ```
