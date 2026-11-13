CREATE EXTENSION IF NOT EXISTS gp_debug_numsegments;

SELECT gp_debug_set_create_table_default_numsegments(2);

CREATE TABLE rank (id INT, rank INT, year INT, gender CHAR(1), count INT)
DISTRIBUTED BY (id)
PARTITION BY RANGE (year)
( START (2006) INCLUSIVE END (2008) EXCLUSIVE EVERY (1),
DEFAULT PARTITION extra);

INSERT INTO rank VALUES (543,1,2006,'m',1);
INSERT INTO rank VALUES (543,1,2007,'f',1);

ALTER TABLE rank EXPAND PARTITION PREPARE;

ALTER TABLE rank_1_prt_2 SET WITH (REORGANIZE=true) DISTRIBUTED BY (id);

1U: UPDATE rank SET year=2006 WHERE gender='f';

SELECT gp_debug_reset_create_table_default_numsegments();

DROP TABLE rank;