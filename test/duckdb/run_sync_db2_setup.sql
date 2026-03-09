-- DuckDB CloudSync Sync Roundtrip: DB2 Setup
-- Creates identical schema in DB2 and loads payload from DB1

.mode list
.separator ' '
.nullvalue NULL

CREATE OR REPLACE MACRO test_pass(name) AS (SELECT printf('[PASS] %s', name));
CREATE OR REPLACE MACRO test_fail(name) AS (SELECT printf('[FAIL] %s', name));

-- ============================================================================
-- Create same tables as DB1 (empty)
-- ============================================================================
CREATE TABLE customers (
    id VARCHAR PRIMARY KEY NOT NULL,
    name VARCHAR,
    age INTEGER,
    note VARCHAR
);
SELECT cloudsync_init('customers');

CREATE TABLE orders (
    customer_id VARCHAR NOT NULL,
    order_id VARCHAR NOT NULL,
    amount DOUBLE,
    status VARCHAR,
    PRIMARY KEY(customer_id, order_id)
);
SELECT cloudsync_init('orders');

CREATE TABLE tags (
    category VARCHAR NOT NULL,
    tag VARCHAR NOT NULL,
    PRIMARY KEY(category, tag)
);
SELECT cloudsync_init('tags');

-- Verify DB2 is empty
SELECT CASE WHEN (SELECT count(*) FROM customers) = 0
    THEN test_pass('DB2: customers empty before load')
    ELSE test_fail('DB2: customers empty before load') END;

-- ============================================================================
-- Load payload from DB1
-- ============================================================================
SELECT CASE WHEN cloudsync_payload_load('/tmp/cloudsync_db1_payload.bin') >= 0
    THEN test_pass('DB2: payload_load succeeded')
    ELSE test_fail('DB2: payload_load succeeded') END;

-- ============================================================================
-- Verify data matches DB1
-- ============================================================================
SELECT CASE WHEN (SELECT count(*) FROM customers) = 5
    THEN test_pass('DB2: 5 customers after sync')
    ELSE test_fail('DB2: 5 customers after sync') END;

SELECT CASE WHEN (SELECT count(*) FROM orders) = 3
    THEN test_pass('DB2: 3 orders after sync')
    ELSE test_fail('DB2: 3 orders after sync') END;

SELECT CASE WHEN (SELECT count(*) FROM tags) = 3
    THEN test_pass('DB2: 3 tags after sync')
    ELSE test_fail('DB2: 3 tags after sync') END;

-- Verify specific values
SELECT CASE WHEN (SELECT name FROM customers WHERE id = 'c1') = 'Alice'
              AND (SELECT age FROM customers WHERE id = 'c1') = 31
    THEN test_pass('DB2: c1 Alice age=31 (updated)')
    ELSE test_fail('DB2: c1 Alice age=31 (updated)') END;

SELECT CASE WHEN (SELECT name FROM customers WHERE id = 'c2') = 'Bobby'
    THEN test_pass('DB2: c2 name=Bobby (updated)')
    ELSE test_fail('DB2: c2 name=Bobby (updated)') END;

SELECT CASE WHEN (SELECT note FROM customers WHERE id = 'c1') = 'VIP updated'
    THEN test_pass('DB2: c1 note updated')
    ELSE test_fail('DB2: c1 note updated') END;

-- Verify deleted rows do not appear
SELECT CASE WHEN (SELECT count(*) FROM customers WHERE id IN ('c6', 'c7')) = 0
    THEN test_pass('DB2: deleted rows c6,c7 not present')
    ELSE test_fail('DB2: deleted rows c6,c7 not present') END;

-- Verify orders
SELECT CASE WHEN (SELECT amount FROM orders WHERE customer_id = 'c1' AND order_id = 'o1') = 99.99
    THEN test_pass('DB2: order o1 amount correct')
    ELSE test_fail('DB2: order o1 amount correct') END;

-- Verify deleted order not present
SELECT CASE WHEN (SELECT count(*) FROM orders WHERE customer_id = 'c3' AND order_id = 'o4') = 0
    THEN test_pass('DB2: deleted order o4 not present')
    ELSE test_fail('DB2: deleted order o4 not present') END;

-- Verify tags
SELECT CASE WHEN (SELECT count(*) FROM tags WHERE category = 'color') = 2
    THEN test_pass('DB2: 2 color tags synced')
    ELSE test_fail('DB2: 2 color tags synced') END;

SELECT '--- DB2 customers ---';
SELECT * FROM customers ORDER BY id;
SELECT '--- DB2 orders ---';
SELECT * FROM orders ORDER BY customer_id, order_id;
SELECT '--- DB2 tags ---';
SELECT * FROM tags ORDER BY category, tag;

-- ============================================================================
-- Now make changes on DB2 and save payload back
-- ============================================================================
INSERT INTO customers VALUES ('c8', 'Heidi', 22, 'DB2 customer');
SELECT cloudsync_insert('customers', 'c8');

UPDATE customers SET age = 32 WHERE id = 'c1';
SELECT cloudsync_update('customers', new_val, old_val) FROM (VALUES
    ('c1', 'c1'), ('Alice', 'Alice'), ('32', '31'), ('VIP updated', 'VIP updated')
) AS v(new_val, old_val);

INSERT INTO orders VALUES ('c8', 'o5', 75.00, 'new');
SELECT cloudsync_insert('orders', 'c8', 'o5');

DELETE FROM customers WHERE id = 'c4';
SELECT cloudsync_delete('customers', 'c4');

SELECT cloudsync_payload_save('/tmp/cloudsync_db2_payload.bin');

SELECT CASE WHEN (SELECT count(*) FROM customers) = 5
    THEN test_pass('DB2: 5 customers after DB2 changes')
    ELSE test_fail('DB2: 5 customers after DB2 changes') END;
