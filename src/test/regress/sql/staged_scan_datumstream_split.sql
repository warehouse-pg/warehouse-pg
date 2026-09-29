-- Staged Scan S1: verify the header/content split of datumstreamread_block().
--
-- datumstreamread_block() is now a thin combination of
-- datumstreamread_block_header_only() + datumstreamread_block_content(), and a
-- third entry point datumstreamread_block_skip_content() steps over a block's
-- content without decompressing it.
--
-- Two fault injectors count the two halves of an AOCO varblock read
-- independently:
--   AppendOnlyStorageRead_ReadBlockHeader        -- once per varblock header read
--   AppendOnlyStorageRead_DecompressBlockContent -- once per content materialization

CREATE TABLE staged_split_aoco(i int, j bigint, k text)
  USING ao_column DISTRIBUTED BY (i);
INSERT INTO staged_split_aoco
  SELECT i, i, 'payload ' || i FROM generate_series(1, 100000) i;

-- Arm a fault as a pure counter (never fails, fires forever) scoped to our table.
CREATE FUNCTION staged_split_arm(faultname text) RETURNS bool AS $$
    SELECT bool_and(gp_inject_fault(faultname, 'skip', '', '',
                                    'staged_split_aoco', 1, -1, 0, dbid) = 'Success:')
      FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
$$ LANGUAGE sql;

-- Total number of times a fault fired across all primary segments.
CREATE FUNCTION staged_split_hits(faultname text) RETURNS bigint AS $$
    SELECT sum(substring(gp_inject_fault(faultname, 'status', dbid)
                         from 'num times hit:''(\d+)''')::bigint)::bigint
      FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
$$ LANGUAGE sql;

CREATE FUNCTION staged_split_reset(faultname text) RETURNS bool AS $$
    SELECT bool_and(gp_inject_fault(faultname, 'reset', dbid) = 'Success:')
      FROM gp_segment_configuration WHERE content > -1 AND role = 'p';
$$ LANGUAGE sql;

--
-- Part 1: a full sequential scan reads and decompresses every block.
--
-- This is the assertion that pins the refactor: datumstreamread_block() used to
-- read the header inline, it now delegates to
-- datumstreamread_block_header_only(), and the block counts must be unchanged.
--
SELECT staged_split_arm('AppendOnlyStorageRead_ReadNextBlock_success');
SELECT staged_split_arm('AppendOnlyStorageRead_ReadBlockHeader');
SELECT staged_split_arm('AppendOnlyStorageRead_DecompressBlockContent');

COPY (SELECT * FROM staged_split_aoco) TO '/dev/null';

-- Sanity: the scan actually read blocks.
SELECT staged_split_hits('AppendOnlyStorageRead_ReadBlockHeader') > 0
  AS seqscan_read_blocks;

-- The new header injector fires exactly once per varblock, i.e. it agrees with
-- the pre-existing block-read injector.
SELECT staged_split_hits('AppendOnlyStorageRead_ReadBlockHeader') =
       staged_split_hits('AppendOnlyStorageRead_ReadNextBlock_success')
  AS headers_equal_blocks;

-- Nothing is skipped on a sequential scan: every header read is followed by a
-- content read.
SELECT staged_split_hits('AppendOnlyStorageRead_ReadBlockHeader') =
       staged_split_hits('AppendOnlyStorageRead_DecompressBlockContent')
  AS headers_equal_contents;

SELECT staged_split_reset('AppendOnlyStorageRead_ReadNextBlock_success');
SELECT staged_split_reset('AppendOnlyStorageRead_ReadBlockHeader');
SELECT staged_split_reset('AppendOnlyStorageRead_DecompressBlockContent');

--
-- Part 2: the fetch (index scan) path, which is the one routed through
-- datumstreamread_block_skip_content().
--
-- NOTE: as of S1 the AOCO block directory pinpoints the exact varblock holding
-- the wanted row, so datumstreamread_find_block() finds it on the first header
-- read and never actually skips. The counts below therefore come out equal and
-- tiny. They are recorded as raw numbers on purpose: when Staged Scan (S2/S3)
-- introduces a real header-scan-and-skip consumer, these numbers change and
-- this test fails loudly, which is the reminder to extend it.
--
CREATE INDEX staged_split_aoco_i_idx ON staged_split_aoco(i);
ANALYZE staged_split_aoco;

SELECT staged_split_arm('AppendOnlyStorageRead_ReadBlockHeader');
SELECT staged_split_arm('AppendOnlyStorageRead_DecompressBlockContent');

-- Force a plain index scan: ORCA ignores enable_*, so use the Postgres planner.
SET optimizer = off;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT k FROM staged_split_aoco WHERE i = 77777;
SELECT k FROM staged_split_aoco WHERE i = 77777;
RESET enable_bitmapscan;
RESET enable_seqscan;
RESET optimizer;

SELECT staged_split_hits('AppendOnlyStorageRead_ReadBlockHeader') AS fetch_headers,
       staged_split_hits('AppendOnlyStorageRead_DecompressBlockContent') AS fetch_contents;

-- Whatever the block directory does, a single-row fetch must stay far below the
-- whole-table block count that Part 1 measured.
SELECT staged_split_hits('AppendOnlyStorageRead_ReadBlockHeader') < 10
  AS fetch_reads_few_blocks;

SELECT staged_split_reset('AppendOnlyStorageRead_ReadBlockHeader');
SELECT staged_split_reset('AppendOnlyStorageRead_DecompressBlockContent');

DROP FUNCTION staged_split_arm(text);
DROP FUNCTION staged_split_hits(text);
DROP FUNCTION staged_split_reset(text);
DROP TABLE staged_split_aoco;
