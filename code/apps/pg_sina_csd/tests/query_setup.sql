\set ON_ERROR_STOP on
DROP TABLE IF EXISTS public.snia_query_fixture,public.snia_query_empty;
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
