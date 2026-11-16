CREATE TABLE ibex_group_boundaries (
    indexid oid REFERENCES indexes ON DELETE CASCADE,
    groups_before bigint NOT NULL,
    upper_key bigint NOT NULL,
    PRIMARY KEY (indexid, groups_before)
);

-- Build exact distinct-key checkpoints once for an already prepared index.
CREATE FUNCTION prepare_ibex(idx regclass) RETURNS bigint
LANGUAGE plpgsql STRICT
SET search_path = pg_catalog, sina_csd
SET enable_hashagg = off
SET enable_seqscan = off
AS $$
DECLARE
    heap regclass;
    key_name name;
BEGIN
    SELECT m.heaprelid, a.attname INTO heap, key_name
    FROM indexes m
    JOIN pg_attribute a ON a.attrelid = m.heaprelid AND a.attnum = m.attnum
    WHERE m.indexid = idx AND a.attnotnull;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'Ibex requires a prepared B-tree with a non-null leading key';
    END IF;

    EXECUTE format('LOCK TABLE %s IN SHARE MODE', heap);
    PERFORM FROM indexes m JOIN relations r ON r.relid = m.indexid
    JOIN relations h ON h.relid = m.heaprelid
    WHERE m.indexid = idx AND m.heaprelid = heap
      AND r.relfilenode = pg_relation_filenode(idx) AND r.bytes = pg_relation_size(idx)
      AND h.relfilenode = pg_relation_filenode(heap) AND h.bytes = pg_relation_size(heap)
    FOR UPDATE OF m;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'CSD mapping for % is missing or stale', idx;
    END IF;

    DELETE FROM ibex_group_boundaries b WHERE b.indexid = idx;
    EXECUTE format($query$
        INSERT INTO ibex_group_boundaries
        SELECT $1, groups_before, key::bigint
        FROM (
            SELECT key, row_number() OVER w - 1 AS groups_before,
                   lead(key) OVER w AS next_key
            FROM (SELECT DISTINCT %1$I AS key FROM %2$s ORDER BY 1) keys
            WINDOW w AS (ORDER BY key)
        ) ranked
        WHERE groups_before %% 4096 = 0 OR next_key IS NULL
    $query$, key_name, heap) USING idx;

    RETURN (SELECT COALESCE(max(b.groups_before) + 1, 0)
            FROM ibex_group_boundaries b WHERE b.indexid = idx);
END;
$$;
