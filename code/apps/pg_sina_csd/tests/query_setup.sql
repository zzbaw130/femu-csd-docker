\set ON_ERROR_STOP on
DELETE FROM sina_csd.relations WHERE relid IN
 (to_regclass('public.snia_query_fixture'),to_regclass('public.snia_query_empty'),to_regclass('public.snia_query_parallel'));
DROP TABLE IF EXISTS public.snia_query_fixture,public.snia_query_empty,public.snia_query_parallel;
CREATE TABLE public.snia_query_fixture(k char(1), a numeric, b numeric, d date, unused text);
INSERT INTO public.snia_query_fixture VALUES
 ('A',1.00,0.10,'1995-01-01','unused'),('A',NULL,0.20,'1995-01-02','unused'),
 ('A',2.50,NULL,'1995-01-03','unused'),('B',-1.25,0.05,'1995-01-04','unused'),
 ('B',0.0,0.00,'1995-01-05','unused'),(NULL,0.01,0.07,'1995-01-06','unused'),
 (' ',123.45,0.06,'1993-01-01','unused'),('Z',NULL,NULL,NULL,'unused');
CREATE TABLE public.snia_query_empty(k char(1), a numeric, b numeric, d date);
VACUUM (FREEZE, ANALYZE) public.snia_query_fixture;
VACUUM (FREEZE, ANALYZE) public.snia_query_empty;
SELECT sina_csd.prepare('public.snia_query_fixture');
SELECT sina_csd.prepare('public.snia_query_empty');
-- Several chunks, including a partial final chunk. The bad value is only
-- projected by the explicit error/cleanup test, not the normal comparisons.
CREATE TABLE public.snia_query_parallel AS
SELECT i%7 AS k, (i%101)::numeric(12,2) AS a,
       CASE WHEN i=1 THEN 1e30::numeric ELSE 0::numeric END AS bad,
       repeat('x',48) AS padding
FROM generate_series(1,450000) AS g(i);
VACUUM (FREEZE, ANALYZE) public.snia_query_parallel;
SELECT sina_csd.prepare('public.snia_query_parallel');
