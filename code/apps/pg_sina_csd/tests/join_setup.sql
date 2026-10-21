\set ON_ERROR_STOP on
DELETE FROM sina_csd.relations WHERE relid IN
 (to_regclass('public.snia_join_build'),to_regclass('public.snia_join_probe'),to_regclass('public.snia_join_empty'),to_regclass('public.snia_join_strings'));
DROP TABLE IF EXISTS snia_join_build,snia_join_probe,snia_join_empty,snia_join_strings;
CREATE TABLE snia_join_build AS
SELECT CASE WHEN i%97=0 THEN NULL ELSE (i%7001)-3500 END::bigint AS k,
       CASE WHEN i%13=0 THEN NULL WHEN i%3=0 THEN 'PROMO BRUSHED' ELSE 'STANDARD' END::text AS flag,
       (i%31)::numeric(12,2) AS amount,
       CASE WHEN i=1 THEN 1e30::numeric ELSE 0::numeric END AS bad,
       repeat('x',48) AS unused
FROM generate_series(1,70003) g(i);
CREATE TABLE snia_join_probe AS
SELECT CASE WHEN i%113=0 THEN NULL ELSE (i%8001)-4000 END::bigint AS k,
       CASE WHEN i%17=0 THEN NULL ELSE (i%100001)/100.0 END::numeric(12,2) AS price,
       (i%11/100.0)::numeric(12,2) AS discount,
       date '1995-09-01'+i%31 AS d,
       CASE WHEN i=1 THEN 1e30::numeric ELSE 0::numeric END AS bad,
       repeat('y',48) AS unused
FROM generate_series(1,150003) g(i);
CREATE TABLE snia_join_empty (LIKE snia_join_build);
VACUUM (FREEZE, ANALYZE) snia_join_build;
VACUUM (FREEZE, ANALYZE) snia_join_probe;
VACUUM (FREEZE, ANALYZE) snia_join_empty;
SELECT sina_csd.prepare('snia_join_build');
SELECT sina_csd.prepare('snia_join_probe');
SELECT sina_csd.prepare('snia_join_empty');
CREATE TABLE snia_join_strings(id int,label text,fixed char(10),varying varchar(20),v numeric);
INSERT INTO snia_join_strings VALUES
 (1,'Z','Z','Z',1.00),(2,'MAIL','MAIL','MAIL',2.0),(3,'SHIP','SHIP','SHIP',3.00),
 (4,'OTHER','OTHER','OTHER',4.00),(5,'MAIL ','MAIL ','MAIL ',5.00),
 (6,'','','',6.00),(7,NULL,NULL,NULL,7.00),(8,'SHIP','SHIP','SHIP',NULL),
 (9,'MAIL','MAIL','MAIL',-2.0),(10,'ZZZ','ZZZ','ZZZ',10.00);
VACUUM (FREEZE, ANALYZE) snia_join_strings;
SELECT sina_csd.prepare('snia_join_strings');
