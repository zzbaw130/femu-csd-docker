\set ON_ERROR_STOP on
CREATE EXTENSION IF NOT EXISTS pg_sina_csd;
DROP TABLE IF EXISTS public.pg_sina_fixture;
CREATE TABLE public.pg_sina_fixture(i int, t text);
INSERT INTO public.pg_sina_fixture VALUES (1, 'a'), (NULL, 'bb'), (-1, NULL);
DO $$ BEGIN
    BEGIN
        PERFORM sina_csd.prepare('public.pg_sina_fixture');
        RAISE EXCEPTION 'unfrozen table was accepted';
    EXCEPTION WHEN others THEN
        IF SQLERRM NOT LIKE 'table must be fully visible and frozen%' THEN RAISE; END IF;
    END;
END $$;
VACUUM (FREEZE, ANALYZE) public.pg_sina_fixture;
SELECT sina_csd.prepare('public.pg_sina_fixture');
DO $$ BEGIN
    IF (SELECT sum(bytes) FROM sina_csd.extents WHERE relid='public.pg_sina_fixture'::regclass)
        <> pg_relation_size('public.pg_sina_fixture') THEN
        RAISE EXCEPTION 'incomplete mapping';
    END IF;
    IF (SELECT count(*) FROM sina_csd.columns WHERE relid='public.pg_sina_fixture'::regclass) <> 2 THEN
        RAISE EXCEPTION 'missing physical attributes';
    END IF;
END $$;
DROP TABLE IF EXISTS public.pg_sina_empty;
CREATE TABLE public.pg_sina_empty(i int);
SELECT sina_csd.prepare('public.pg_sina_empty');
DO $$ BEGIN
    IF EXISTS(SELECT FROM sina_csd.extents WHERE relid='public.pg_sina_empty'::regclass) THEN
        RAISE EXCEPTION 'empty heap has extents';
    END IF;
END $$;
