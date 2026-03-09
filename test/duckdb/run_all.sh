#!/bin/bash
#
# DuckDB CloudSync Full Test Suite
# Runs unit tests and sync roundtrip tests using file-based databases.
#
# Usage: test/duckdb/run_all.sh [path/to/duckdb]
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB="${1:-/Users/marco/SQLiteAI/duckdb/build/release/duckdb}"

if [ ! -x "$DUCKDB" ]; then
    echo "ERROR: DuckDB binary not found at $DUCKDB"
    exit 1
fi

# Temp directory for test databases
TMPDIR="${TMPDIR:-/tmp}"
DB_UNIT="$TMPDIR/cloudsync_test_unit.duckdb"
DB1="$TMPDIR/cloudsync_test_db1.duckdb"
DB2="$TMPDIR/cloudsync_test_db2.duckdb"
PAYLOAD1="$TMPDIR/cloudsync_db1_payload.bin"
PAYLOAD2="$TMPDIR/cloudsync_db2_payload.bin"

# Cleanup previous test artifacts
rm -f "$DB_UNIT" "$DB1" "$DB2" "$PAYLOAD1" "$PAYLOAD2"
rm -f "$DB_UNIT.wal" "$DB1.wal" "$DB2.wal"

FAILED=0
TOTAL_PASS=0
TOTAL_FAIL=0

count_results() {
    local output="$1"
    local pass_count fail_count
    pass_count=$(echo "$output" | grep -c '^\[PASS\]' || true)
    fail_count=$(echo "$output" | grep -c '^\[FAIL\]' || true)
    TOTAL_PASS=$((TOTAL_PASS + pass_count))
    TOTAL_FAIL=$((TOTAL_FAIL + fail_count))
    if [ "$fail_count" -gt 0 ]; then
        FAILED=1
    fi
}

echo "============================================"
echo "DuckDB CloudSync Test Suite"
echo "Binary: $DUCKDB"
echo "============================================"
echo ""

# -----------------------------------------------
# Part 1: Unit Tests
# -----------------------------------------------
echo "--- Part 1: Unit Tests ---"
OUTPUT=$("$DUCKDB" "$DB_UNIT" < "$SCRIPT_DIR/run_tests.sql" 2>&1) || true
echo "$OUTPUT" | grep -E '^\[(PASS|FAIL)\]'
count_results "$OUTPUT"

# Show any errors that aren't expected
ERRORS=$(echo "$OUTPUT" | grep -i 'error\|exception' | grep -iv '\[PASS\]\|\[FAIL\]\|rejected\|integer PK' || true)
if [ -n "$ERRORS" ]; then
    echo ""
    echo "UNEXPECTED ERRORS:"
    echo "$ERRORS"
fi
echo ""

# -----------------------------------------------
# Part 2: Sync Roundtrip (DB1 → DB2 → DB1)
# -----------------------------------------------
echo "--- Part 2: Sync Roundtrip ---"

# Step 1: Setup DB1 with data, save payload
echo "  Step 1: DB1 setup + save payload"
OUTPUT=$("$DUCKDB" "$DB1" < "$SCRIPT_DIR/run_sync_tests.sql" 2>&1) || true
echo "$OUTPUT" | grep -E '^\[(PASS|FAIL)\]'
count_results "$OUTPUT"

# Step 2: Setup DB2, load DB1's payload, make changes, save DB2's payload
echo "  Step 2: DB2 setup + load DB1 payload + save DB2 payload"
OUTPUT=$("$DUCKDB" "$DB2" < "$SCRIPT_DIR/run_sync_db2_setup.sql" 2>&1) || true
echo "$OUTPUT" | grep -E '^\[(PASS|FAIL)\]'
count_results "$OUTPUT"

# Step 3: DB1 loads DB2's payload (bidirectional sync)
echo "  Step 3: DB1 loads DB2 payload (bidirectional merge)"
OUTPUT=$("$DUCKDB" "$DB1" < "$SCRIPT_DIR/run_sync_db1_merge.sql" 2>&1) || true
echo "$OUTPUT" | grep -E '^\[(PASS|FAIL)\]'
count_results "$OUTPUT"

echo ""

# -----------------------------------------------
# Cleanup
# -----------------------------------------------
rm -f "$DB_UNIT" "$DB1" "$DB2" "$PAYLOAD1" "$PAYLOAD2"
rm -f "$DB_UNIT.wal" "$DB1.wal" "$DB2.wal"
rm -f /tmp/cloudsync_duckdb_test_payload.bin /tmp/cloudsync_duckdb_test_full.bin

# -----------------------------------------------
# Summary
# -----------------------------------------------
echo "============================================"
echo "Results: $TOTAL_PASS passed, $TOTAL_FAIL failed"
echo "============================================"

if [ "$TOTAL_FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
