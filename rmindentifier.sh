# Remove all files in all subdirectories that end with .Identifier
find . -type f -name "*.Identifier" -exec rm -f {} \;
