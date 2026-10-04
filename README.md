# Inverted-Indexer

## Fetching Data

This project indexes the [MS MARCO passage-ranking](https://ir-datasets.com/msmarco-passage.html) collection (8.8M passages).

```bash
mkdir -p data && cd data

# Passages to collection.tsv
wget https://msmarco.z22.web.core.windows.net/msmarcoranking/collection.tar.gz
tar -xzf collection.tar.gz

# Queries for testing 
wget https://msmarco.z22.web.core.windows.net/msmarcoranking/queries.tar.gz
tar -xzf queries.tar.gz

# Sanity check: should print 8841823
wc -l collection.tsv

# Small subset for development
head -500000 collection.tsv > sample.tsv
```

Each line of `collection.tsv` is `pid<TAB>passage text`.


## Build

```bash
make
```

Compiles `build_runs` and `merge` into `build/`.


## Run

```bash
mkdir -p data/runs
```

# Part 1: parse collection.tsv -> sorted intermediate runs

```bash
./build/build_runs "../collection.tsv" data/runs 40000000
```

- "../collection.tsv" points path to collection.

- 40000000 is the `buffer_threshold` (default 5,000,000). This are the number of postings held in memory before the parser writes a run file to disk. 


# Part 2: merge the runs -> final compressed index


```bash
./build/merge data/runs data 128
```
 
- `block_size` (default 128) — postings per compressed block in the final
  index. 


## What `build_runs` does

Reads `collection.tsv` **once**, line by line (never loading the whole
file into memory), and for each passage:

1. Tokenizes the text (lowercase ASCII alphanumerics only; any non-ASCII
   byte — foreign-language text — acts as a word separator instead of
   crashing).
2. Counts term frequencies within that one document.
3. Assigns the document a sequential `docID` (0, 1, 2, ...) and appends
   `(docID, freq)` postings for each of its terms into an in-memory map.
4. Records the document's token count into `doc_lengths.bin` and its
   original passage ID into `page_table.bin` (both fixed-width binary,
   4 bytes/doc, so either can be looked up by seeking to `docID * 4`).

Once the in-memory postings map accumulates `buffer_threshold` entries, it's
sorted by term and written out as one **run file** (`run_N.txt`) in the format `term\tdocID\tfreq` per line, then cleared.
This repeats until the collection is exhausted. A final `stats.txt`
records total docs, total tokens, and how many runs were produced.

**Output:** `run_0.txt ... run_N.txt`, `doc_lengths.bin`, `page_table.bin`,
`stats.txt`.

## What `merge` does

Takes the sorted run files created by build_runs and performs a **k-way merge**
(a min-heap over all open run files, ordered by term) to combine them into
a single global, alphabetically sorted posting list per term.

For each term, once all its postings across every run are collected:

1. Splits them into fixed-size blocks (`block_size` postings each).
2. Within each block, encode 
   both the docID gaps and the frequencies in separate arrays using **varbyte compression**.
3. Writes a small per-block header first (last docID in the block, and the
   byte size of its compressed docID and frequencies) so a future query
   processor can seek directly to any block without decompressing the
   ones before it.
4. Records the term's name, its byte offset into `index.bin`, and its
   document frequency in `lexicon.txt` (plain text, sorted by term).

**Output:** `index.bin` (the compressed inverted lists) and `lexicon.txt`
(term -> offset/df lookup table).
