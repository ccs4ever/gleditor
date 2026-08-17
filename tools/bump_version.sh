#!/bin/sh
set -e

# 1. Ensure we are in a Git repository
if [ \! -d .git ]; then
    echo "Error: Not inside a git repository root." >&2
    exit 1
fi

# 2. Check for the VERSION file
VERSION_FILE="VERSION"
if [ \! -f "$VERSION_FILE" ]; then
    echo "Error: $VERSION_FILE file not found in the current directory." >&2
    exit 1
fi

# 3. Read current version and parse Major.Minor.Patch
CURRENT_VERSION=$(cat "$VERSION_FILE")
echo "Current version: $CURRENT_VERSION"

# Extract components using basic string manipulation
MAJOR=$(echo "$CURRENT_VERSION" | cut -d. -f1)
MINOR=$(echo "$CURRENT_VERSION" | cut -d. -f2)
PATCH=$(echo "$CURRENT_VERSION" | cut -d. -f3)

# 4. Increment the patch version (Default behavior)
# Adjust arithmetic here if you want to bump Major or Minor instead
PATCH=$((PATCH + 1))
NEW_VERSION="$MAJOR.$MINOR.$PATCH"
echo "Bumping to new version: $NEW_VERSION"

# 5. Write out the new version
echo "$NEW_VERSION" > "$VERSION_FILE"

# 6. Force Makefile flag evaluation to refresh version banners
# (This clears artifacts that cache old compilation variables)
if [ -f "Makefile" ]; then
    echo "Cleaning temporary build artifacts to enforce new flags..."
    make clean >/dev/null 2>&1 || true
fi

# 7. Commit changes and mint the release tag
git add "$VERSION_FILE"
git commit -m "Bump version to $NEW_VERSION"
git tag -a "v$NEW_VERSION" -m "Release version $NEW_VERSION"

echo "Success! Version updated locally to v$NEW_VERSION and tagged."
echo "Run 'git push origin main --tags' when you are ready to publish."
