"""Merge settings from stdin into an sdkconfig file. Prints key names only, never values.

    sdkconfig_merge.py <sdkconfig>          stdin lines are KEY<TAB>VALUE; each is written as
                                            CONFIG_KEY="VALUE", a quoted string
    sdkconfig_merge.py --raw <sdkconfig>    stdin lines are already sdkconfig syntax
                                            (CONFIG_X=value, or "# CONFIG_X is not set"); each is
                                            appended unchanged, so it can write y and hex numbers

Either way, an existing line for the same key (set or "is not set") is removed first.
Use it to add build-time settings such as Wi-Fi credentials or a developer token to a
build tree's sdkconfig without putting them in the source or on a command line.
"""
import re, sys

raw = sys.argv[1:2] == ["--raw"]
path = sys.argv[2] if raw else sys.argv[1]
new = {}
if raw:
    for n, line in enumerate(sys.stdin.read().splitlines(), 1):
        if not line.strip() or (line.startswith("#") and not line.endswith(" is not set")):
            continue
        m = re.match(r"(CONFIG_[A-Za-z0-9_]+)=", line) or re.fullmatch(r"# (CONFIG_[A-Za-z0-9_]+) is not set", line)
        if not m:
            sys.exit(f"--raw: line {n} is not sdkconfig syntax")
        new[m.group(1)] = line
else:
    for line in sys.stdin.read().splitlines():
        if line:
            key, value = line.split("\t", 1)
            new["CONFIG_" + key] = value
with open(path) as f:
    lines = f.read().splitlines()
kept = [l for l in lines
        if not any(l.startswith(k + "=") or l == f"# {k} is not set" for k in new)]
for k, v in new.items():
    if raw:
        kept.append(v)
    else:
        kept.append(f'{k}="{v.replace(chr(92), chr(92) * 2).replace(chr(34), chr(92) + chr(34))}"')
with open(path, "w") as f:
    f.write("\n".join(kept) + "\n")
print("set in sdkconfig:", " ".join(new))
