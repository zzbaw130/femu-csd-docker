CREATE TABLE relations (
    relid oid PRIMARY KEY,
    relfilenode oid NOT NULL,
    device text NOT NULL,
    namespace_id integer NOT NULL,
    lba_bytes integer NOT NULL,
    bytes bigint NOT NULL,
    nattrs integer NOT NULL,
    boot_id text NOT NULL,
    prepared_at timestamptz NOT NULL DEFAULT clock_timestamp()
);
CREATE TABLE columns (
    relid oid REFERENCES relations ON DELETE CASCADE,
    attnum smallint NOT NULL,
    name text NOT NULL,
    type_oid oid NOT NULL,
    type_modifier integer NOT NULL,
    length smallint NOT NULL,
    alignment "char" NOT NULL,
    not_null boolean NOT NULL,
    collation_oid oid NOT NULL,
    PRIMARY KEY (relid, attnum)
);
CREATE TABLE extents (
    relid oid REFERENCES relations ON DELETE CASCADE,
    logical_offset bigint NOT NULL,
    start_lba bigint NOT NULL,
    bytes bigint NOT NULL,
    PRIMARY KEY (relid, logical_offset)
);
-- Native B-tree metadata prepared with its immutable heap.
CREATE TABLE indexes (
    indexid oid PRIMARY KEY REFERENCES relations(relid) ON DELETE CASCADE,
    heaprelid oid NOT NULL REFERENCES relations(relid) ON DELETE CASCADE,
    attnum smallint NOT NULL,
    root bigint NOT NULL,
    key_bytes smallint NOT NULL
);
CREATE FUNCTION version() RETURNS text
AS 'MODULE_PATHNAME', 'pg_sina_csd_version' LANGUAGE C STRICT;
CREATE FUNCTION prepare(regclass, text DEFAULT '/dev/nvme0n1') RETURNS bigint
AS 'MODULE_PATHNAME', 'pg_sina_csd_prepare' LANGUAGE C STRICT;
