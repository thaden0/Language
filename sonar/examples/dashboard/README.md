# dashboard

The theming showcase: a `GridBox` of `ContentBox` tiles, a runtime theme swap
(`Theme::Default()` <-> `Theme::Dark()`), and the `DebugOverlay`.

```sh
trident run
SONAR_SCRIPT=1 trident run   # snapshot the grid, flip the theme, toggle the overlay
```

Scripted mode snapshots the grid under both themes and proves the same style key
resolves differently after the swap.
