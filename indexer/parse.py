#!/usr/bin/env python3
import os
import re
import sys
import time
from array import array
from collections import Counter

import click

TOKEN_RE = re.compile(rb"[a-z0-9]+")


def flush_run(buffer, runs_dir, run_no):
    """Write the in-memory postings to a sorted run file and return its path."""
    path = os.path.join(runs_dir, "run_%03d.txt" % run_no)
    with open(path, "wb", buffering=1 << 22) as out:
        # bytes keys sort the same way for std::string in the merger.
        for term in sorted(buffer):
            postings = buffer[term]  # flattened [docID, freq, docID, freq, ...]
            prefix = term + b" "
            out.write(b"".join(
                b"%s%d %d\n" % (prefix, postings[i], postings[i + 1])
                for i in range(0, len(postings), 2)
            ))
    return path


def parse(input_path, outdir, max_postings, limit):
    runs_dir = os.path.join(outdir, "runs")
    os.makedirs(runs_dir, exist_ok=True)

    # term -> array of interleaved (docID, freq). array('I') costs ~8 bytes per
    # posting, versus ~100 bytes for a list of tuples.
    buffer = {}
    buffered = 0
    pagetable = array("I")  # interleaved (pid, doclen), indexed by docID. This is unsigned int typecode (4byte so almost 4bil)
    total_terms = 0
    run_no = 0
    doc_id = 0
    start = time.time()

    with open(input_path, "rb", buffering=1 << 22) as f:
        for line in f:
            if limit is not None and doc_id >= limit:
                break
            tab = line.find(b"\t")
            if tab < 0:
                continue  # malformed line so skip it
            pid = int(line[:tab])
            tokens = TOKEN_RE.findall(line[tab + 1:].lower())

            pagetable.append(pid)
            pagetable.append(len(tokens))
            total_terms += len(tokens)

            for term, freq in Counter(tokens).items():
                postings = buffer.get(term)
                if postings is None:
                    postings = buffer[term] = array("I")
                postings.append(doc_id)
                postings.append(freq)
                buffered += 1

            doc_id += 1

            if buffered >= max_postings:
                path = flush_run(buffer, runs_dir, run_no)
                print("[%7.1fs] docs=%d wrote %s (%d postings)"
                      % (time.time() - start, doc_id, path, buffered),
                      file=sys.stderr)
                buffer = {}
                buffered = 0
                run_no += 1

    if buffered:
        path = flush_run(buffer, runs_dir, run_no)
        print("[%7.1fs] docs=%d wrote %s (%d postings)"
              % (time.time() - start, doc_id, path, buffered), file=sys.stderr)
        run_no += 1

    if sys.byteorder != "little":
        pagetable.byteswap()
    with open(os.path.join(outdir, "pagetable.bin"), "wb") as out:
        pagetable.tofile(out)

    with open(os.path.join(outdir, "meta.txt"), "w") as out:
        out.write("num_docs %d\n" % doc_id)
        out.write("total_terms %d\n" % total_terms)
        out.write("num_runs %d\n" % run_no)

    print("[%7.1fs] done: %d docs, %d terms, %d runs"
          % (time.time() - start, doc_id, total_terms, run_no), file=sys.stderr)


@click.command(help=__doc__)
@click.option("--input", "input_path", default="data/collection.tsv",
              show_default=True, type=click.Path(exists=True, dir_okay=False))
@click.option("--outdir", default="build", show_default=True,
              type=click.Path(file_okay=False))
@click.option("--max-postings", default=40_000_000, show_default=True,
              help="Postings buffered in memory before writing a run.")
@click.option("--limit", type=int, default=None,
              help="Only parse the first N passages.")
def main(input_path, outdir, max_postings, limit):
    parse(input_path, outdir, max_postings, limit)


if __name__ == "__main__":
    main()
