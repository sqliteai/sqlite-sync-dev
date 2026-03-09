-- DuckDB CloudSync Sync Roundtrip Test Suite
-- Tests payload save/load between two separate DuckDB databases.
--
-- Run with: test/duckdb/run_sync.sh
-- (This file is sourced by the shell script which manages two databases)

-- This SQL is for DB1 setup. The shell script orchestrates the full roundtrip.

.mode list
.separator ' '
.nullvalue NULL

CREATE OR REPLACE MACRO test_pass(name) AS (SELECT printf('[PASS] %s', name));
CREATE OR REPLACE MACRO test_fail(name) AS (SELECT printf('[FAIL] %s', name));

-- ============================================================================
-- PHASE 1: Setup DB1 with data
-- ============================================================================

-- Single PK table
CREATE TABLE customers (
    id VARCHAR PRIMARY KEY NOT NULL,
    name VARCHAR,
    age INTEGER,
    note VARCHAR
);
SELECT cloudsync_init('customers');

-- Composite PK table
CREATE TABLE orders (
    customer_id VARCHAR NOT NULL,
    order_id VARCHAR NOT NULL,
    amount DOUBLE,
    status VARCHAR,
    PRIMARY KEY(customer_id, order_id)
);
SELECT cloudsync_init('orders');

-- PK-only table
CREATE TABLE tags (
    category VARCHAR NOT NULL,
    tag VARCHAR NOT NULL,
    PRIMARY KEY(category, tag)
);
SELECT cloudsync_init('tags');

-- Insert data into customers
INSERT INTO customers VALUES ('c1', 'Alice', 30, 'VIP customer');
INSERT INTO customers VALUES ('c2', 'Bob', 25, 'Regular');
INSERT INTO customers VALUES ('c3', 'Charlie', 35, 'Premium');
INSERT INTO customers VALUES ('c4', 'Diana', 28, 'New customer');
INSERT INTO customers VALUES ('c5', 'Eve', 40, 'Wholesale');
INSERT INTO customers VALUES ('c6', 'Frank', 55, 'To delete');
INSERT INTO customers VALUES ('c7', 'Grace', 33, 'To delete too');

SELECT cloudsync_insert('customers', 'c1');
SELECT cloudsync_insert('customers', 'c2');
SELECT cloudsync_insert('customers', 'c3');
SELECT cloudsync_insert('customers', 'c4');
SELECT cloudsync_insert('customers', 'c5');
SELECT cloudsync_insert('customers', 'c6');
SELECT cloudsync_insert('customers', 'c7');

-- Insert data into orders
INSERT INTO orders VALUES ('c1', 'o1', 99.99, 'shipped');
INSERT INTO orders VALUES ('c1', 'o2', 149.50, 'pending');
INSERT INTO orders VALUES ('c2', 'o3', 200.00, 'delivered');
INSERT INTO orders VALUES ('c3', 'o4', 50.00, 'cancelled');

SELECT cloudsync_insert('orders', 'c1', 'o1');
SELECT cloudsync_insert('orders', 'c1', 'o2');
SELECT cloudsync_insert('orders', 'c2', 'o3');
SELECT cloudsync_insert('orders', 'c3', 'o4');

-- Insert data into tags
INSERT INTO tags VALUES ('color', 'red');
INSERT INTO tags VALUES ('color', 'blue');
INSERT INTO tags VALUES ('size', 'large');

SELECT cloudsync_insert('tags', 'color', 'red');
SELECT cloudsync_insert('tags', 'color', 'blue');
SELECT cloudsync_insert('tags', 'size', 'large');

-- Perform some updates
UPDATE customers SET age = 31, note = 'VIP updated' WHERE id = 'c1';
SELECT cloudsync_update('customers', new_val, old_val) FROM (VALUES
    ('c1', 'c1'), ('Alice', 'Alice'), ('31', '30'), ('VIP updated', 'VIP customer')
) AS v(new_val, old_val);

UPDATE customers SET name = 'Bobby' WHERE id = 'c2';
SELECT cloudsync_update('customers', new_val, old_val) FROM (VALUES
    ('c2', 'c2'), ('Bobby', 'Bob'), ('25', '25'), ('Regular', 'Regular')
) AS v(new_val, old_val);

-- Perform some deletes
DELETE FROM customers WHERE id = 'c6';
SELECT cloudsync_delete('customers', 'c6');

DELETE FROM customers WHERE id = 'c7';
SELECT cloudsync_delete('customers', 'c7');

-- Delete an order
DELETE FROM orders WHERE customer_id = 'c3' AND order_id = 'o4';
SELECT cloudsync_delete('orders', 'c3', 'o4');

-- Verify counts
SELECT CASE WHEN (SELECT count(*) FROM customers) = 5
    THEN test_pass('DB1: 5 customers after deletes')
    ELSE test_fail('DB1: 5 customers after deletes') END;

SELECT CASE WHEN (SELECT count(*) FROM orders) = 3
    THEN test_pass('DB1: 3 orders after delete')
    ELSE test_fail('DB1: 3 orders after delete') END;

SELECT CASE WHEN (SELECT count(*) FROM tags) = 3
    THEN test_pass('DB1: 3 tags')
    ELSE test_fail('DB1: 3 tags') END;

-- Save payload
SELECT cloudsync_payload_save('/tmp/cloudsync_db1_payload.bin');

SELECT CASE WHEN cloudsync_db_version() > 0
    THEN test_pass('DB1: db_version > 0 after operations')
    ELSE test_fail('DB1: db_version > 0 after operations') END;

SELECT '--- DB1 customers ---';
SELECT * FROM customers ORDER BY id;
SELECT '--- DB1 orders ---';
SELECT * FROM orders ORDER BY customer_id, order_id;
SELECT '--- DB1 tags ---';
SELECT * FROM tags ORDER BY category, tag;
