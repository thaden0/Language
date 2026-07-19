# file-manager

The enterprise centerpiece: a `SplitBox` pairing a file `ListView` with a
preview `ContentBox`, over a `ContentBar` status line. Arrow keys move the
selection; the preview and status follow.

```sh
trident run
SONAR_SCRIPT=1 trident run   # selects files with Down, snapshots the split view
```

A production build would back the `ListView` with a lazy directory source over
the `File`/`sys` natives; this example uses an in-memory listing to stay
hermetic and differential. Mutable fields are padded to a fixed width
(see Sonar bug #4).
