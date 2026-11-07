#!/bin/bash
# Builds Renguin into your Haiku source tree and installs it on THIS Haiku
# machine ("Option B" in docs/TESTING.md): the patched haiku and haiku_devel
# packages, the Renguin kernel module, and the test programs.
#
# Run it on Haiku, from anywhere:
#     ~/renguin-build/renguin/tools/haiku-test-install.sh
#
# Undo it:
#     ~/renguin-build/renguin/tools/haiku-test-install.sh --revert
# or, if the machine doesn't boot: hold Shift (or press Space) at boot, then
# "Select boot volume",
# pick your disk, "Select state", and choose the state from before.
#
# Everything is logged to ~/renguin-install.log.

set -o pipefail

usage() {
	cat <<EOF
Usage: $0 [options]

  --haiku DIR      Haiku source checkout (default: ~/renguin-build/haiku)
  --jobs N         parallel build jobs (default: number of CPUs)
  --yes            don't ask before installing packages (pkgman -y)
  --no-pull        don't update the Renguin checkout first
  --module-only    only rebuild and install the Renguin module (no kernel
                   or package changes; for module-only fixes)
  --no-install     build only; install nothing
  --revert         remove the Renguin module and return the system packages
                   to the repository versions (pkgman full-sync)
  --dry-run        print the commands instead of running them
  -h, --help       this text
EOF
}

# --- Options -----------------------------------------------------------------

ORIGINAL_ARGS=("$@")

RENGUIN_DIR=$(cd "$(dirname "$0")/.." && pwd)
HAIKU_DIR="$HOME/renguin-build/haiku"
JOBS=""
PKGMAN_YES=""
PULL=1
MODULE_ONLY=0
INSTALL=1
REVERT=0
DRY_RUN=0

while [ $# -gt 0 ]; do
	case "$1" in
		--haiku) HAIKU_DIR=$2; shift ;;
		--haiku=*) HAIKU_DIR=${1#*=} ;;
		--jobs) JOBS=$2; shift ;;
		--jobs=*) JOBS=${1#*=} ;;
		--yes) PKGMAN_YES="-y" ;;
		--no-pull) PULL=0 ;;
		--module-only) MODULE_ONLY=1 ;;
		--no-install) INSTALL=0 ;;
		--revert) REVERT=1 ;;
		--dry-run) DRY_RUN=1 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "Unknown option: $1"; usage; exit 2 ;;
	esac
	shift
done

PATCH="$RENGUIN_DIR/patches/haiku/0001-kernel-foreign-ABI-personality-hooks.patch"
MODULE_SRC="$RENGUIN_DIR/haiku/src/add-ons/kernel/personalities"
TESTS_SRC="$RENGUIN_DIR/tests/linux/prebuilt/renguin-tests"
BUILD_DIR="$HAIKU_DIR/generated.x86_64"
PACKAGES_DIR="$BUILD_DIR/objects/haiku/x86_64/packaging/packages"
MODULE_DEST="${RENGUIN_MODULE_DEST:-/boot/system/non-packaged/add-ons/kernel/personalities}"
TESTS_DEST="${RENGUIN_TESTS_DEST:-$HOME/Desktop/renguin-tests}"
LOG="$HOME/renguin-install.log"

# --- Helpers -----------------------------------------------------------------

: > "$LOG" 2>/dev/null || LOG=/dev/null

say() { printf '\n==> %s\n' "$*" | tee -a "$LOG"; }
note() { printf '    %s\n' "$*" | tee -a "$LOG"; }
die() {
	printf '\nERROR: %s\n' "$*" | tee -a "$LOG"
	printf 'The full log is in %s\n' "$LOG"
	exit 1
}

# Runs a command, echoing it, logging its output. In dry-run mode only
# prints it.
run() {
	printf '    $ %s\n' "$*" | tee -a "$LOG"
	[ $DRY_RUN = 1 ] && return 0
	"$@" 2>&1 | tee -a "$LOG"
	return ${PIPESTATUS[0]}
}

# For commands that ask questions (pkgman): run them directly on the
# terminal. Piping them through tee would make their output block-buffered,
# so a prompt might never show.
run_interactive() {
	printf '    $ %s\n' "$*" | tee -a "$LOG"
	[ $DRY_RUN = 1 ] && return 0
	"$@"
	local status=$?
	printf '    (exit status %s)\n' "$status" >> "$LOG"
	return $status
}

# The files the patch touches, as paths relative to the Haiku checkout.
patch_files() {
	grep '^diff --git ' "$PATCH" | awk '{ print $3 }' | sed 's|^a/||'
}

# --- Checks ------------------------------------------------------------------

say "Checking the environment"
if [ "$(uname -s)" != "Haiku" ] && [ $DRY_RUN = 0 ]; then
	die "This script runs on Haiku. (Use --dry-run elsewhere to see what it does.)"
fi
if [ "$(uname -m)" != "x86_64" ] && [ $DRY_RUN = 0 ]; then
	die "Renguin needs Haiku x86_64; this is $(uname -m)."
fi
[ -f "$PATCH" ] || die "Can't find the patch in $RENGUIN_DIR. Run the script from the Renguin checkout."
note "Renguin: $RENGUIN_DIR"
note "Haiku:   $HAIKU_DIR"
note "Log:     $LOG"

if [ -z "$JOBS" ]; then
	JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
fi

# --- Revert ------------------------------------------------------------------

if [ $REVERT = 1 ]; then
	say "Removing the Renguin module"
	run rm -f "$MODULE_DEST/renguin"
	say "Returning the system packages to the repository versions"
	note "pkgman shows what it will change and asks first (unless --yes)."
	run_interactive pkgman full-sync $PKGMAN_YES \
		|| die "pkgman full-sync failed. You can also roll back from the boot menu (hold Shift at boot, Select boot volume, your disk, Select state)."
	say "Done. Reboot to finish."
	exit 0
fi

# --- Update Renguin ----------------------------------------------------------

if [ $PULL = 1 ]; then
	say "Updating the Renguin checkout"
	before=$(git -C "$RENGUIN_DIR" rev-parse HEAD 2>/dev/null)
	run git -C "$RENGUIN_DIR" pull --ff-only \
		|| die "Couldn't update Renguin (local changes?). Use --no-pull to skip this."
	after=$(git -C "$RENGUIN_DIR" rev-parse HEAD 2>/dev/null)
	if [ "$before" != "$after" ] && [ $DRY_RUN = 0 ]; then
		# This script may itself have changed, and bash reads scripts while
		# running them: start over with the new version.
		note "Renguin was updated; restarting with the new version of this script."
		exec bash "$RENGUIN_DIR/tools/haiku-test-install.sh" --no-pull \
			"${ORIGINAL_ARGS[@]}"
	fi
fi

# --- Prepare the Haiku tree --------------------------------------------------

[ -d "$HAIKU_DIR/.git" ] || die "No Haiku checkout at $HAIKU_DIR. Clone it first:
    git clone https://review.haiku-os.org/haiku.git $HAIKU_DIR
or point the script at yours with --haiku DIR."

if [ $MODULE_ONLY = 0 ]; then
	say "Applying the kernel patch"
	if git -C "$HAIKU_DIR" apply --reverse --check "$PATCH" 2>/dev/null; then
		note "The current patch is already applied."
	else
		# Put every file the patch touches back the way git has it (this
		# also removes an older version of the patch), then apply.
		note "Resetting the files the patch touches..."
		for f in $(patch_files); do
			if git -C "$HAIKU_DIR" ls-files --error-unmatch -- "$f" \
					>/dev/null 2>&1; then
				run git -C "$HAIKU_DIR" checkout -- "$f" \
					|| die "Couldn't reset $f"
			else
				run rm -f "$HAIKU_DIR/$f"
			fi
		done
		if ! git -C "$HAIKU_DIR" apply --check "$PATCH" 2>>"$LOG"; then
			die "The patch doesn't apply to this Haiku checkout. Its base is
    $(cat "$RENGUIN_DIR/patches/haiku/BASE_COMMIT")
Your checkout is at $(git -C "$HAIKU_DIR" rev-parse --short HEAD). Send the log."
		fi
		run git -C "$HAIKU_DIR" apply "$PATCH" || die "git apply failed"
	fi
fi

say "Copying the Renguin module source into the Haiku tree"
run rm -rf "$HAIKU_DIR/src/add-ons/kernel/personalities"
run cp -r "$MODULE_SRC" "$HAIKU_DIR/src/add-ons/kernel/" \
	|| die "Couldn't copy the module source"

# --- Build -------------------------------------------------------------------

say "Configuring the build"
if [ -f "$BUILD_DIR/build/BuildConfig" ]; then
	note "Already configured ($BUILD_DIR)."
else
	if [ $MODULE_ONLY = 0 ] && ! git -C "$HAIKU_DIR" describe --tags \
			--match='hrev*' >/dev/null 2>&1; then
		die "Your Haiku clone has no hrev tags, which the package build needs.
Fetch them with:  git -C $HAIKU_DIR fetch --tags"
	fi
	run mkdir -p "$BUILD_DIR" || die "Couldn't create $BUILD_DIR"
	if [ $DRY_RUN = 1 ]; then
		note "$ (cd $BUILD_DIR && ../configure --target-arch x86_64)"
	else
		(cd "$BUILD_DIR" && ../configure --target-arch x86_64) 2>&1 \
			| tee -a "$LOG"
		[ ${PIPESTATUS[0]} = 0 ] || die "configure failed"
	fi
fi

if [ $MODULE_ONLY = 1 ]; then
	TARGETS="renguin"
else
	# haiku_devel must match haiku exactly (its requires: haiku == version),
	# so both are built and installed together.
	TARGETS="renguin kernel_x86_64 haiku.hpkg haiku_devel.hpkg"
fi

say "Building: $TARGETS"
note "The first package build takes a while; later runs only rebuild what changed."
if [ $DRY_RUN = 1 ]; then
	note "$ (cd $BUILD_DIR && jam -q -j$JOBS $TARGETS)"
else
	(cd "$BUILD_DIR" && jam -q -j"$JOBS" $TARGETS) 2>&1 | tee -a "$LOG"
	[ ${PIPESTATUS[0]} = 0 ] || die "The build failed. The errors are above and in the log."
fi

# The built module. Its exact place under objects/ depends on the build
# type, so look for it.
MODULE_BUILT=""
if [ $DRY_RUN = 0 ]; then
	MODULE_BUILT=$(find "$BUILD_DIR/objects" -type f -name renguin \
		-path '*add-ons/kernel/personalities/renguin/*' 2>/dev/null | head -1)
	[ -n "$MODULE_BUILT" ] || die "Built module not found under $BUILD_DIR/objects."
	note "Module: $MODULE_BUILT"
	if [ $MODULE_ONLY = 0 ]; then
		for p in haiku.hpkg haiku_devel.hpkg; do
			[ -f "$PACKAGES_DIR/$p" ] || die "Built package not found: $PACKAGES_DIR/$p"
		done
		note "Packages: $PACKAGES_DIR/haiku.hpkg, haiku_devel.hpkg"
	fi
fi

if [ $INSTALL = 0 ]; then
	say "Built. Nothing installed (--no-install)."
	exit 0
fi

# --- Install -----------------------------------------------------------------

if [ $MODULE_ONLY = 0 ]; then
	say "Installing the patched haiku and haiku_devel packages"
	note "pkgman lists what it will change and asks before doing it (unless"
	note "--yes). It should only replace haiku and haiku_devel. If it wants to"
	note "remove anything else, answer no and send the log."
	note "Haiku keeps the previous state; you can go back from the boot menu."
	run_interactive pkgman install $PKGMAN_YES "$PACKAGES_DIR/haiku.hpkg" \
			"$PACKAGES_DIR/haiku_devel.hpkg" \
		|| die "pkgman install failed or was cancelled. Nothing else was changed."
fi

say "Installing the Renguin module"
run mkdir -p "$MODULE_DEST" || die "Couldn't create $MODULE_DEST"
run cp "${MODULE_BUILT:-<built module>}" "$MODULE_DEST/renguin" \
	|| die "Couldn't copy the module"

say "Copying the test programs to $TESTS_DEST"
run rm -rf "$TESTS_DEST"
run cp -r "$TESTS_SRC" "$TESTS_DEST" || die "Couldn't copy the tests"
for f in "$TESTS_SRC"/*; do
	case "$f" in *.txt) ;; *) run chmod +x "$TESTS_DEST/$(basename "$f")" ;; esac
done

# --- Done --------------------------------------------------------------------

say "All done. Reboot now."
cat <<EOF | tee -a "$LOG"

After the reboot, open Terminal and run:

    cd ~/Desktop/renguin-tests
    ./run-tests.sh .
    grep -i "renguin\\|personalit" /var/log/syslog

If the machine doesn't boot: hold Shift (or press Space) at boot, choose
"Select boot volume",
your disk, "Select state", and pick the state from before this install. To
boot without Renguin's module only, choose safe mode with "Disable user
add-ons" instead.

To undo everything later:  $0 --revert
EOF
