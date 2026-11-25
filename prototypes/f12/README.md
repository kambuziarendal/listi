# Manual version history experiment

Standalone CLI, not installed by Listi. Requires Python 3.7+ with sqlite3.
Use one quiet local directory and a new SQLite file in a separate trusted
directory. The database contains plaintext paths, metadata and content.

    python3 history.py STORE save SOURCE
    python3 history.py STORE points
    python3 history.py STORE browse POINT_ID
    python3 history.py STORE restore POINT_ID NEW_DIRECTORY

Format: config.root binds the store to one absolute source. points contains a
monotonic integer ID, Unix creation time and a JSON manifest (relative paths,
stat mode, mtime in ns, link target or SHA-256 reference, attributes).
objects maps SHA-256 to whole file bytes. One SQLite transaction publishes a
complete point and all new objects; browsing opens the database read-only.
No hard links. Restore verifies the digest and creates independent files.
Use a writable store directory for SQLite crash recovery before read-only
browsing if the previous writer was killed with a hot journal.

## Native acceptance: run from this directory on BFS

    c++ -std=c++11 bfs-attrs.cpp -lbe -o bfs-attrs
    python3 test_history.py
    T=$(mktemp -d /boot/home/f12-test.XXXXXX)
    mkdir -p "$T/src/dir"
    cd "$T/src"
    printf original > dir/note
    ln -s dir/note pointer
    addattr -t mime BEOS:TYPE text/plain dir/note
    addattr -t string Status initial dir/note
    addattr -t int32 Count 42 dir/note
    printf '\000\377\001' > "$T/raw"
    addattr -f "$T/raw" -t raw Bytes dir/note
    addattr -t string Project alpha dir
    cd -
    python3 history.py "$T/history" save "$T/src"
    addattr -t string Status approved "$T/src/dir/note"
    python3 history.py "$T/history" save "$T/src"
    mv "$T/src/dir/note" "$T/src/dir/new"
    python3 history.py "$T/history" save "$T/src"
    printf changed > "$T/src/dir/new"
    python3 history.py "$T/history" save "$T/src"
    rm "$T/src/dir/new"
    python3 history.py "$T/history" save "$T/src"
    python3 history.py "$T/history" browse 5
    python3 history.py "$T/history" restore 1 "$T/one"
    python3 history.py "$T/history" restore 2 "$T/two"
    python3 history.py "$T/history" restore 3 "$T/three"
    python3 history.py "$T/history" restore 4 "$T/four"
    python3 -c 'import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute("select count(*) from objects").fetchone())' "$T/history"
    listattr -l "$T/one/dir/note"
    catattr Count "$T/one/dir/note"
    catattr BEOS:TYPE "$T/one/dir/note"
    catattr Status "$T/one/dir/note"
    catattr Status "$T/two/dir/note"
    catattr Project "$T/one/dir"
    catattr -r Bytes "$T/one/dir/note" | cmp - "$T/raw"
    readlink "$T/one/pointer"
    cat "$T/three/dir/new"
    cat "$T/four/dir/new"

Check exactly two objects; MIME text/plain, Status initial then approved,
int32 42, raw bytes identical, directory Project alpha, pointer target
dir/note, renamed contents original then changed, no file in point 5.
Native attributes
are captured directly via BNode, never by dereferencing a symlink. Attributes
are hex-encoded records: three big-endian uint32s (name length, type code,
value length) followed by name and unmodified value bytes.

Limits: experimental, no scheduler/retention, in-place rollback, volume
indexes, ownership, ACL or symlink mode/time restoration. Directory/file
permissions preserve rwx bits only. Non-Haiku captures omit native attributes.
One file and its attributes must fit memory and SQLite limits. Scanning is not
an atomic filesystem snapshot; quiesce writers. Use a private destination
parent. A killed restore can leave staging files or an empty reserved name;
it does not modify the source or database. No hostile-store support.
