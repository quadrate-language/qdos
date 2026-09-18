# Building is Meson's job; see README.md. This is here for the housekeeping
# that wants one spelling across both repositories.
#
#   make format     reformat in place
#   make fmtcheck   report what is not formatted, change nothing

# The .clang-format is Quadrate's, copied so the two trees read alike. QDOS is
# mostly C where Quadrate is C++, so this sweeps .c as well.
#
# The generated font tables are not listed here: they carry `clang-format off`
# themselves, emitted by tools/genfont.py and tools/genpadfont.py, so an editor
# formatting on save leaves them alone too.
SOURCES = $(shell find src include tests examples -type f \
	\( -name '*.c' -o -name '*.h' -o -name '*.cc' \) 2>/dev/null)

.PHONY: format fmtcheck

format:
	clang-format -i $(SOURCES)

fmtcheck:
	@clang-format --dry-run --Werror $(SOURCES)
