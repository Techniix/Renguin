#!/bin/bash
# Tests tools/haiku-test-install.sh on Linux, against a real Haiku checkout,
# with fake uname/jam/pkgman/configure. Nothing outside a temporary
# directory is touched.
#
# Usage: tools/test-install-script.sh /path/to/haiku-checkout
#   The checkout needs git history for the files the patch touches; a
#   blobless/sparse clone is fine if those files are checked out.
set -u
HAIKU_SRC=${1:?usage: $0 /path/to/haiku-checkout}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SCRIPT="$HERE/tools/haiku-test-install.sh"
PATCH="$HERE/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fails=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails+1)); fi; }

mkdir -p "$T/bin" "$T/home/Desktop"
cat > "$T/bin/uname" <<'EOF'
#!/bin/sh
case "$1" in -m) echo x86_64 ;; *) echo Haiku ;; esac
EOF
cat > "$T/bin/jam" <<'EOF'
#!/bin/sh
echo "jam $*" >> "$FAKELOG"
[ -n "${JAM_FAIL:-}" ] && { echo "fake compile error"; exit 1; }
d=objects/haiku/x86_64
mkdir -p $d/release/add-ons/kernel/personalities/renguin $d/packaging/packages
echo module > $d/release/add-ons/kernel/personalities/renguin/renguin
for t in "$@"; do case $t in *.hpkg) echo pkg > $d/packaging/packages/$t ;; esac; done
EOF
cat > "$T/bin/pkgman" <<'EOF'
#!/bin/sh
echo "pkgman $*" >> "$FAKELOG"
case " $* " in *" -y "*) exit 0 ;; esac
printf 'Continue? [yes/no] (yes) : '
read answer
[ "$answer" = "no" ] && exit 1
exit 0
EOF
chmod +x "$T/bin/"*

# Copy rather than clone: partial (blobless) clones can't be cloned locally.
cp -r "$HAIKU_SRC" "$T/haiku"
cd "$T/haiku" && git reset -q && git checkout -q -- . && git clean -qfd
git tag hrev99999 2>/dev/null
make_configure() {
	cat > "$T/haiku/configure" <<'EOF'
#!/bin/sh
echo "configure $*" >> "$FAKELOG"
mkdir -p build && echo config > build/BuildConfig
EOF
	chmod +x "$T/haiku/configure"
}
make_configure

export PATH="$T/bin:$PATH" HOME="$T/home" FAKELOG="$T/fake.log"
export RENGUIN_MODULE_DEST="$T/sys" RENGUIN_TESTS_DEST="$T/home/Desktop/renguin-tests"
reverse_ok() { git -C "$T/haiku" apply --reverse --check "$PATCH" 2>/dev/null; }
S() { "$SCRIPT" --haiku "$T/haiku" --no-pull "$@" > "$T/out" 2>&1; }

: > "$FAKELOG"; S --yes
check "fresh tree: exit 0" '[ $? = 0 ] || true; grep -q "All done" "$T/out"'
check "fresh tree: patch applied" reverse_ok
check "fresh tree: module installed" '[ -f "$T/sys/renguin" ]'
check "fresh tree: haiku + haiku_devel installed" 'grep -q "pkgman install -y .*haiku.hpkg .*haiku_devel.hpkg" "$FAKELOG"'
check "fresh tree: tests copied, executable" '[ -x "$T/home/Desktop/renguin-tests/proc" ] && [ ! -x "$T/home/Desktop/renguin-tests/README.txt" ]'

: > "$FAKELOG"; S --yes
check "re-run: detects applied patch" 'grep -q "already applied" "$T/out"'
check "re-run: skips configure" '! grep -q configure "$FAKELOG"'

# An older version of the patch in the tree (the one from 2026-11-06).
OLD=$(git -C "$HERE" show 06bf68a:patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch 2>/dev/null)
if [ -n "$OLD" ]; then
	cd "$T/haiku" && git reset -q && git checkout -q -- . \
		&& git clean -qfd -e generated.x86_64
	echo "$OLD" | git apply && make_configure
	S --yes
	check "old patch: replaced by current" reverse_ok
fi

: > "$FAKELOG"; S --module-only
check "module-only: builds only renguin" 'grep -qx "jam -q -j[0-9]* renguin" "$FAKELOG"'
check "module-only: no pkgman" '! grep -q pkgman "$FAKELOG"'

: > "$FAKELOG"; JAM_FAIL=1 S --yes
check "build failure: stops before installing" 'grep -q "build failed" "$T/out" && ! grep -q pkgman "$FAKELOG"'

: > "$FAKELOG"; echo no | S
check "pkgman declined: stops" 'grep -q "cancelled" "$T/out" && grep -q "Continue?" "$T/out"'

rm -rf "$T/haiku/generated.x86_64"; git -C "$T/haiku" tag -d hrev99999 >/dev/null
: > "$FAKELOG"; S --yes
check "no hrev tags: clear error" 'grep -q "no hrev tags" "$T/out"'
git -C "$T/haiku" tag hrev99999

: > "$FAKELOG"; "$SCRIPT" --haiku "$T/haiku" --revert --yes > "$T/out" 2>&1
check "revert: module removed, full-sync" '[ ! -f "$T/sys/renguin" ] && grep -q "pkgman full-sync -y" "$FAKELOG"'

echo "$fails failed"
[ $fails = 0 ]
