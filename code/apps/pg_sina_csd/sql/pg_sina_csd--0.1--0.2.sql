-- Native B-tree metadata prepared with its immutable heap.
CREATE TABLE indexes (
    indexid oid PRIMARY KEY REFERENCES relations(relid) ON DELETE CASCADE,
    heaprelid oid NOT NULL REFERENCES relations(relid) ON DELETE CASCADE,
    attnum smallint NOT NULL,
    root bigint NOT NULL,
    key_bytes smallint NOT NULL
);
