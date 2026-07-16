# hello

The 15-line Sonar app: an `App`, a `Text`, and a `q`-to-quit bind.

```sh
trident run              # launch in your terminal
SONAR_SCRIPT=1 trident run   # scripted mode: print a snapshot, don't grab the tty
```

`SONAR_SCRIPT=1` binds the `TestRenderer` + `ScriptedInput` harness and prints a
two-channel snapshot instead of taking over the terminal — the same path
`hello.expected` pins as a differential test across the oracle, IR, and LLVM
engines.
