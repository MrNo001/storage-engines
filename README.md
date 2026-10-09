## Storage Engine implementations using different indexing structures

### Simple Engine
    No indexing structure,append only file

### Hash Index
    Uses an in memory hash-map to keep the file and offset inside the file where the data 
    associated with the key is.

### LSM Tree
    Writes go to a write-ahead log and an in memory sorted memtable, which is flushed to
    immutable sorted SST files on disk when full. A manifest tracks the live SST files,
    and reads check the memtable first, then the SSTs from newest to oldest.

### B+Tree
    Keeps keys sorted in a balanced tree of fixed size pages, with all values stored in
    the leaf nodes and leaves linked together for efficient range scans. Updates are done
    in place, splitting nodes when they overflow.
