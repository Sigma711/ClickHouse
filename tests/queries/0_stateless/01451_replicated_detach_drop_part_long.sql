-- Tags: long, replica, no-replicated-database, no-shared-merge-tree
-- Tag no-replicated-database: Fails due to additional replicas or shards
-- no-shared-merge-tree: depends on max_replicated_merges_in_queue

SET replication_alter_partitions_sync = 2;

DROP TABLE IF EXISTS replica1 SYNC;
DROP TABLE IF EXISTS replica2 SYNC;

CREATE TABLE replica1 (v UInt8) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/'||currentDatabase()||'test/01451/attach', 'r1') order by tuple() settings max_replicated_merges_in_queue = 0;
CREATE TABLE replica2 (v UInt8) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/'||currentDatabase()||'test/01451/attach', 'r2') order by tuple() settings max_replicated_merges_in_queue = 0;

INSERT INTO replica1 SETTINGS insert_keeper_fault_injection_probability=0 VALUES (0);
INSERT INTO replica1 SETTINGS insert_keeper_fault_injection_probability=0 VALUES (1);
INSERT INTO replica1 SETTINGS insert_keeper_fault_injection_probability=0 VALUES (2);

ALTER TABLE replica1 DETACH PART 'all_100_100_0'; -- { serverError NO_SUCH_DATA_PART }

SELECT v FROM replica1 ORDER BY v;

SYSTEM SYNC REPLICA replica2;
ALTER TABLE replica2 DETACH PART 'all_1_1_0';

SELECT v FROM replica1 ORDER BY v;

SELECT name FROM system.detached_parts WHERE table = 'replica2' AND database = currentDatabase();

ALTER TABLE replica2 ATTACH PART 'all_1_1_0' SETTINGS insert_keeper_fault_injection_probability=0;

SYSTEM SYNC REPLICA replica1;
SELECT v FROM replica1 ORDER BY v;

SELECT name FROM system.detached_parts WHERE table = 'replica2' AND database = currentDatabase();

SELECT '-- drop part --';

ALTER TABLE replica1 DROP PART 'all_3_3_0';

ALTER TABLE replica1 ATTACH PART 'all_3_3_0'; -- { serverError BAD_DATA_PART_NAME }

SELECT v FROM replica1 ORDER BY v;

SELECT '-- resume merges --';

ALTER TABLE replica1 MODIFY SETTING max_replicated_merges_in_queue = 1;
OPTIMIZE TABLE replica1 FINAL;

SELECT v FROM replica1 ORDER BY v;

SELECT name FROM system.parts WHERE table = 'replica2' AND active AND database = currentDatabase();

DROP TABLE replica1 SYNC;
DROP TABLE replica2 SYNC;
