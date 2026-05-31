#!/bin/bash
TESTS_DIR=$(realpath ./z3-tests-2026)
WORK_DIR=/tmp/tapedev_test
DEVICES="/dev/tapedev0s0 /dev/tapedev0s1 /dev/tapedev0s2 /dev/tapedev0s3 /dev/tapedev0s4"

EXT4_DEVICES="/dev/tapedev0s1 /dev/tapedev0s2 /dev/tapedev0s3 /dev/tapedev0s4"

C_TESTS=(
    "ioctl-read"
    "eject"
    "rw-basic"
    "rw-vector"
    "rw-direct"
    "zig-zag-order"
    "backward-order"
    "random-order"
)

PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=()

run_test() {
    local desc="$1"
    shift
    echo ""
    echo "  --- $desc ---"
    if "$@"; then
        echo "  ✓ PASS: $desc"
        PASS=$((PASS+1))
        return 0
    else
        local rc=$?
        echo "  ✗ FAIL: $desc (exit code $rc)"
        FAIL=$((FAIL+1))
        FAILED_TESTS+=("$desc")
        return $rc
    fi
}

phase_header() {
    echo ""
    echo "##########################################"
    echo "# $1"
    echo "##########################################"
}

if [ ! -d "$TESTS_DIR" ]; then
    echo "ERROR: Tests directory $TESTS_DIR not found"
    exit 1
fi

if [ ! -e /dev/tapedev0s0 ]; then
    echo "ERROR: /dev/tapedev0s0 not found. Is tapedev module loaded?"
    echo "Try: sudo insmod tapedev.ko"
    exit 1
fi

for t in "${C_TESTS[@]}"; do
    if [ ! -x "$TESTS_DIR/$t" ]; then
        echo "ERROR: $TESTS_DIR/$t not found or not executable."
        echo "Run: cd $TESTS_DIR && make"
        exit 1
    fi
done

sudo umount /mnt/tapedev 2>/dev/null || true
sudo umount /mnt/tape 2>/dev/null || true
mkdir -p $WORK_DIR
rm -f $WORK_DIR/urandom_* $WORK_DIR/output_*

echo "Tests dir: $TESTS_DIR"
echo "Work dir:  $WORK_DIR"
echo "Devices:   $DEVICES"

# sysfs
phase_header "Phase 1: sysfs (read-only)"

run_test "sysfs all sections" sudo $TESTS_DIR/sysfs.sh

# ioctle
phase_header "Phase 2: ioctl tests"

for dev in $DEVICES; do
    run_test "ioctl-read $dev" sudo $TESTS_DIR/ioctl-read $dev
    run_test "eject $dev"      sudo $TESTS_DIR/eject $dev
done

# R/W per-device
phase_header "Phase 3: Basic R/W tests per device"

for dev in $DEVICES; do
    echo ""
    echo "=========================================="
    echo "Device: $dev"
    echo "=========================================="

    run_test "rw-basic $dev"       sudo $TESTS_DIR/rw-basic $dev
    run_test "rw-vector $dev"      sudo $TESTS_DIR/rw-vector $dev
    run_test "rw-direct $dev"      sudo $TESTS_DIR/rw-direct $dev
    run_test "zig-zag-order $dev"  sudo $TESTS_DIR/zig-zag-order $dev
    run_test "backward-order $dev" sudo $TESTS_DIR/backward-order $dev
    run_test "random-order $dev"   sudo $TESTS_DIR/random-order $dev
done

# ext4 test
phase_header "Phase 4: ext4 filesystem tests"

for dev in $EXT4_DEVICES; do
    run_test "ext4 on $dev" sudo $TESTS_DIR/ext4.sh $dev
    sudo umount /mnt/tapedev 2>/dev/null || true
done

# dd test
phase_header "Phase 5: dd bulk test (parallel on all devices)"
run_test "dd bulk parallel" bash -c "cd $WORK_DIR && sudo $TESTS_DIR/dd.sh"

# Cleanup
sudo umount /mnt/tapedev 2>/dev/null || true
rm -f $WORK_DIR/urandom_* $WORK_DIR/output_*

# Summary
echo ""
echo "=========================================="
echo "SUMMARY"
echo "=========================================="
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for t in "${FAILED_TESTS[@]}"; do
        echo "  ✗ $t"
    done
    echo ""
    echo "Sprawdź dmesg po szczegóły:"
    echo "  dmesg | tail -100 | grep -i tapedev"
    exit 1
fi

echo ""
echo "ALL TESTS PASSED ✓"