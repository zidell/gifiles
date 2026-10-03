#!/bin/sh
# Refreshes the translation sources from the code and compiles them (the .qm files are committed,
# so builds don't need Qt Linguist). Korean in the code is the source text; edit i18n/*.ts
# (Qt Linguist or by hand), then run this again.
set -e
cd "$(dirname "$0")/.."
for lang in en ja zh_CN; do
    lupdate -silent -locations none -no-obsolete -source-language ko -target-language "$lang" src -ts "i18n/gifiles_$lang.ts"
done
lrelease -silent i18n/gifiles_en.ts i18n/gifiles_ja.ts i18n/gifiles_zh_CN.ts
