-- DuckDB CloudSync Sync Roundtrip: DB1 merges DB2's payload
-- DB1 loads the payload saved by DB2 and verifies convergence

.mode list
.separator ' '
.nullvalue NULL

CREATE OR REPLACE MACRO test_pass(name) AS (SELECT printf('[PASS] %s', name));
CREATE OR REPLACE MACRO test_fail(name) AS (SELECT printf('[FAIL] %s', name));

-- ============================================================================
-- Re-initialize CloudSync context (new DuckDB process, tables already exist)
-- ============================================================================
SELECT cloudsync_init('customers');
SELECT cloudsync_init('orders');
SELECT cloudsync_init('tags');

-- ============================================================================
-- Load DB2's payload into DB1
-- ============================================================================
SELECT CASE WHEN cloudsync_payload_load('/tmp/cloudsync_db2_payload.bin') >= 0
    THEN test_pass('DB1: loaded DB2 payload')
    ELSE test_fail('DB1: loaded DB2 payload') END;

-- ============================================================================
-- Verify DB1 has DB2's new data
-- ============================================================================

-- c8 was added by DB2
SELECT CASE WHEN (SELECT name FROM customers WHERE id = 'c8') = 'Heidi'
    THEN test_pass('DB1: c8 Heidi from DB2 present')
    ELSE test_fail('DB1: c8 Heidi from DB2 present') END;

-- c1 age was updated to 32 by DB2 (was 31 in DB1)
SELECT CASE WHEN (SELECT age FROM customers WHERE id = 'c1') = 32
    THEN test_pass('DB1: c1 age=32 merged from DB2')
    ELSE test_fail('DB1: c1 age=32 merged from DB2') END;

-- c4 was deleted by DB2
SELECT CASE WHEN (SELECT count(*) FROM customers WHERE id = 'c4') = 0
    THEN test_pass('DB1: c4 deleted by DB2 merge')
    ELSE test_fail('DB1: c4 deleted by DB2 merge') END;

-- o5 was added by DB2
SELECT CASE WHEN (SELECT amount FROM orders WHERE customer_id = 'c8' AND order_id = 'o5') = 75.00
    THEN test_pass('DB1: order o5 from DB2 present')
    ELSE test_fail('DB1: order o5 from DB2 present') END;

-- Final counts
SELECT CASE WHEN (SELECT count(*) FROM customers) = 5
    THEN test_pass('DB1: 5 customers after bidirectional sync')
    ELSE test_fail('DB1: 5 customers after bidirectional sync') END;

SELECT CASE WHEN (SELECT count(*) FROM orders) = 4
    THEN test_pass('DB1: 4 orders after bidirectional sync')
    ELSE test_fail('DB1: 4 orders after bidirectional sync') END;

SELECT '--- DB1 final customers ---';
SELECT * FROM customers ORDER BY id;
SELECT '--- DB1 final orders ---';
SELECT * FROM orders ORDER BY customer_id, order_id;
