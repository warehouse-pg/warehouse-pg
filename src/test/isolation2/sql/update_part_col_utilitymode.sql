CREATE EXTENSION IF NOT EXISTS gp_debug_numsegments;

SELECT gp_debug_set_create_table_default_numsegments(2);

CREATE TABLE t_update_part_col_utilitymode (id INT, rank INT, year INT, gender CHAR(1), count INT)
DISTRIBUTED BY (id)
PARTITION BY RANGE (year)
( START (2006) INCLUSIVE END (2008) EXCLUSIVE EVERY (1),
DEFAULT PARTITION extra);

INSERT INTO t_update_part_col_utilitymode VALUES (543,1,2006,'m',1);
INSERT INTO t_update_part_col_utilitymode VALUES (543,1,2007,'f',1);

ALTER TABLE t_update_part_col_utilitymode EXPAND PARTITION PREPARE;

ALTER TABLE t_update_part_col_utilitymode_1_prt_2 SET WITH (REORGANIZE=true) DISTRIBUTED BY (id);

1U: UPDATE t_update_part_col_utilitymode SET year=2006 WHERE gender='f';

SELECT gp_debug_reset_create_table_default_numsegments();

DROP TABLE t_update_part_col_utilitymode;
