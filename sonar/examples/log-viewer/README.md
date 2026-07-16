# log-viewer

The animation / timer showcase: a `ListView` over a growing source with a
follow-mode toggle and a `Spinner` footer. In a real run `App.every(...)` polls
the tail; scripted mode injects the same append steps deterministically (timers
only fire inside `App.run()`).

```sh
trident run              # press f to toggle follow
SONAR_SCRIPT=1 trident run   # append rows, snapshot, prove follow tracks the tail
```
