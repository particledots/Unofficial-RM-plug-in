#!/bin/bash
# Generic installer for any "Unofficial ..." Audio Unit plugin.
# Put this file in the same folder as the .component (the unzipped plugin folder) and double-click it.
# It finds every .component next to it, so the same file works for every plugin with no editing.
# Installs the AU for all users in /Library/Audio/Plug-Ins/Components (Logic loads AU only). Does not install VST3.
# Asks for the Mac password once. It touches only: the .component files next to this file,
# the same-named copy in /Library/Audio/Plug-Ins/Components, and an old same-named copy in ~/Library/Audio/Plug-Ins/Components.
cd "$(dirname "$0")" || exit 1
SRC="$(pwd)"
DEST="/Library/Audio/Plug-Ins/Components"
OLDDIR="$HOME/Library/Audio/Plug-Ins/Components"

shopt -s nullglob
COMPONENTS=("$SRC"/*.component)
if [ ${#COMPONENTS[@]} -eq 0 ]; then
  COMPONENTS=("$SRC"/../*.component)   # fall back to the folder above
fi

if [ ${#COMPONENTS[@]} -eq 0 ]; then
  echo "Cannot find any .component file next to this file."
  echo "Leave this file in the unzipped plugin folder, beside the .component."
  read -n 1 -s -r -p "Press any key to close."
  exit 1
fi

echo "Found:"
for C in "${COMPONENTS[@]}"; do echo "  $(basename "$C")"; done
echo
xattr -dr com.apple.quarantine "$SRC" 2>/dev/null
echo "Your Mac password is needed to write to $DEST (nothing shows while you type)."
sudo mkdir -p "$DEST" || { read -n 1 -s -r -p "Password failed. Press any key to close."; exit 1; }

FAILED=0
for C in "${COMPONENTS[@]}"; do
  NAME="$(basename "$C")"
  rm -rf "$OLDDIR/$NAME"
  sudo rm -rf "$DEST/$NAME"
  sudo cp -R "$C" "$DEST/"
  if [ -d "$DEST/$NAME" ]; then
    echo "Installed: $DEST/$NAME"
  else
    echo "Install FAILED: $NAME is not in $DEST"
    FAILED=1
  fi
done
killall -9 AudioComponentRegistrar 2>/dev/null

echo
if [ $FAILED -eq 0 ]; then
  echo "Now open Logic. If it does not appear, Plug-in Manager, Reset & Rescan Selection."
fi
echo
read -n 1 -s -r -p "Press any key to close."
