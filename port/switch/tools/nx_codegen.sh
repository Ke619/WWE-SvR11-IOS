#!/bin/bash
# Generates the Switch's recompiled code: port/svr2011_config.toml with the options below and
# switch/svr2011_codegen.toml, into WORK/generated/default, then tools/fix_generated.py.
#   nx_codegen.sh <port folder> <work folder> <rexglue.exe>
# The result is copied to /work/nx/generated/default by hand. PC and Android don't use it.
set -e
PORT="$1"; WORK="$2"; REXGLUE="$3"
mkdir -p "$WORK/assets"
[ -f "$WORK/assets/default.xex" ] || cp "$PORT/assets/default.xex" "$WORK/assets/"
cp "$PORT/svr2011_manifest.toml" "$WORK/"
{
  printf 'ctr_as_local = true\nxer_as_local = true\nreserved_as_local = true\ncr_as_local = true\nnon_volatile_as_local = true\n\n'
  cat "$PORT/svr2011_config.toml"
  printf '\n'
  cat "$PORT/switch/svr2011_codegen.toml"
} > "$WORK/svr2011_config.toml"
cd "$WORK"
"$REXGLUE" codegen svr2011_manifest.toml --ignore-stamp > codegen.log 2>&1
tail -2 codegen.log
python "$PORT/switch/tools/fix_generated.py" "$WORK/generated/default"
