# form-wizard

The forms showcase: `Input` / `CheckBox` / `RadioGroup` / `Button`, an email
`validator`, and Tab-order traversal.

```sh
trident run
SONAR_SCRIPT=1 trident run   # types into the name field, Tabs across controls, snapshots
```

Scripted mode drives real keystrokes through `ScriptedInput` (encoded from
chord specs) and asserts the validator + focus behaviour, then snapshots the
form.
